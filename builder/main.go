// Platinum 3HDS builder: makes the installable game (a CIA for FBI, a 3DSX
// for the Homebrew Launcher) from your own Pokémon Platinum (USA) ROM. The
// port's program comes with the builder (templates/, made by
// tools/make_builder.sh); the game's files come from the ROM.
package main

import (
	"bufio"
	"bytes"
	"compress/gzip"
	"embed"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"time"
)

const version = "0.0.1-beta"

//go:embed templates
var templates embed.FS

func template(name string) ([]byte, error) {
	data, err := templates.ReadFile("templates/" + name)
	if err != nil {
		return nil, fmt.Errorf("the builder is missing its %s (was it built with tools/make_builder.sh?)", name)
	}
	if strings.HasSuffix(name, ".gz") {
		z, err := gzip.NewReader(bytes.NewReader(data))
		if err != nil {
			return nil, err
		}
		return io.ReadAll(z)
	}
	return data, nil
}

func main() {
	out := flag.String("out", "", "folder for the game files (default: the ROM's folder)")
	force := flag.Bool("force", false, "build even if the ROM's files differ from Pokémon Platinum (USA)")
	noPause := flag.Bool("no-pause", false, "do not wait for Enter before closing")
	showVersion := flag.Bool("version", false, "print the version")
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Platinum 3HDS builder %s\n\nusage: %s [--out folder] [--force] [rom.nds]\n\n", version, filepath.Base(os.Args[0]))
		flag.PrintDefaults()
	}
	flag.Parse()
	if *showVersion {
		fmt.Println(version)
		return
	}

	fmt.Printf("Platinum 3HDS builder %s\n", version)
	fmt.Println("Makes Platinum 3HDS for your 3DS from your Pokémon Platinum (USA) ROM.")
	fmt.Println()

	interactive := flag.NArg() == 0
	// Windows closes the window when the program ends (a ROM dropped on the
	// .exe), so it waits there too.
	pause := !*noPause && (interactive || runtime.GOOS == "windows")
	romPath := flag.Arg(0)
	if interactive {
		var err error
		romPath, err = askForROM()
		if err != nil {
			fail(err, pause)
		}
	}
	if err := build(romPath, *out, *force); err != nil {
		fail(err, pause)
	}
	if pause {
		fmt.Print("\nPress Enter to close.")
		bufio.NewReader(os.Stdin).ReadString('\n')
	}
}

func fail(err error, pause bool) {
	fmt.Fprintf(os.Stderr, "\nError: %v\n", err)
	if pause {
		fmt.Print("\nPress Enter to close.")
		bufio.NewReader(os.Stdin).ReadString('\n')
	}
	os.Exit(1)
}

// Asks for the ROM's path: typed, pasted, or dropped on the window (which
// quotes it, or on macOS escapes its spaces with backslashes).
func askForROM() (string, error) {
	fmt.Println("Drag your Pokémon Platinum (USA) .nds file into this window, then press Enter.")
	fmt.Print("> ")
	line, err := bufio.NewReader(os.Stdin).ReadString('\n')
	if err != nil && line == "" {
		return "", fmt.Errorf("no ROM given")
	}
	path := strings.TrimSpace(line)
	if len(path) >= 2 && (path[0] == '"' || path[0] == '\'') && path[len(path)-1] == path[0] {
		path = path[1 : len(path)-1]
	}
	if _, err := os.Stat(path); err != nil && runtime.GOOS != "windows" && strings.Contains(path, "\\") {
		var b strings.Builder
		for i := 0; i < len(path); i++ {
			if path[i] == '\\' && i+1 < len(path) {
				i++
			}
			b.WriteByte(path[i])
		}
		path = b.String()
	}
	if path == "" {
		return "", fmt.Errorf("no ROM given")
	}
	return path, nil
}

func build(romPath, outDir string, force bool) error {
	start := time.Now()
	info, err := os.Stat(romPath)
	if err != nil {
		return fmt.Errorf("cannot open %s: %v", romPath, err)
	}
	if info.Size() > 512<<20 {
		return fmt.Errorf("%s is too big to be a DS ROM", filepath.Base(romPath))
	}
	fmt.Printf("Reading %s\n", filepath.Base(romPath))
	rom, err := os.ReadFile(romPath)
	if err != nil {
		return err
	}

	fmt.Println("Checking that it is Pokémon Platinum (USA)")
	files, err := readNitroFS(rom)
	if err != nil {
		return err
	}
	manifestText, err := template("manifest.txt")
	if err != nil {
		return err
	}
	manifest, err := parseManifest(manifestText)
	if err != nil {
		return err
	}
	if bad := verifyFiles(files, manifest); len(bad) > 0 {
		fmt.Printf("  %d of the game's files differ from Pokémon Platinum (USA):\n", len(bad))
		for i, p := range bad {
			if i == 5 {
				fmt.Printf("    and %d more\n", len(bad)-5)
				break
			}
			fmt.Printf("    %s\n", p)
		}
		if !force {
			return fmt.Errorf("this ROM is not an unmodified Pokémon Platinum (USA); the port may not run with it (--force builds it anyway)")
		}
		fmt.Println("  Building anyway (--force).")
	} else {
		fmt.Printf("  All %d game files match.\n", len(files))
	}

	ciaBytes, err := template("template.cia.gz")
	if err != nil {
		return err
	}
	cia, err := parseCIA(ciaBytes)
	if err != nil {
		return err
	}
	threedsx, err := template("template.3dsx.gz")
	if err != nil {
		return err
	}
	smdh, err := template("icon.smdh")
	if err != nil {
		return err
	}
	if rgb, opaque, err := dsIcon(rom); err == nil {
		large, small := iconFromDS(rgb, opaque)
		smdh = smdhWithIcon(smdh, large, small)
	} else {
		fmt.Printf("  (%v: keeping the plain icon)\n", err)
	}

	fmt.Println("Packing the game files")
	level3 := buildRomFS(files)

	if outDir == "" {
		outDir = filepath.Dir(romPath)
	}
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		return err
	}
	ciaPath := filepath.Join(outDir, "Platinum3HDS.cia")
	fmt.Printf("Writing %s\n", ciaPath)
	if err := writeAtomically(ciaPath, func(w io.Writer) error { return cia.write(w, level3, smdh) }); err != nil {
		return err
	}
	threedsxPath := filepath.Join(outDir, "Platinum3HDS.3dsx")
	fmt.Printf("Writing %s\n", threedsxPath)
	if err := writeAtomically(threedsxPath, func(w io.Writer) error { return write3DSX(w, threedsx, level3, smdh) }); err != nil {
		return err
	}

	fmt.Printf("\nDone in %.1f s.\n", time.Since(start).Seconds())
	fmt.Println("Install Platinum3HDS.cia with FBI (it appears on the HOME menu),")
	fmt.Println("or copy Platinum3HDS.3dsx to the SD card's 3ds folder for the Homebrew Launcher.")
	return nil
}

// Writes a file through a temporary one, so a failed build leaves no
// half-written game behind.
func writeAtomically(path string, write func(io.Writer) error) error {
	tmp := path + ".part"
	f, err := os.Create(tmp)
	if err != nil {
		return err
	}
	w := bufio.NewWriterSize(f, 1<<20)
	err = write(w)
	if err == nil {
		err = w.Flush()
	}
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		os.Remove(tmp)
		return err
	}
	os.Remove(path)
	return os.Rename(tmp, path)
}
