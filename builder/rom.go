package main

import (
	"bufio"
	"bytes"
	"crypto/sha1"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"sort"
	"strconv"
	"strings"
)

// A file of the DS ROM's file system (NitroFS).
type romFile struct {
	path string // e.g. "demo/title/titledemo.narc"
	data []byte // a slice of the ROM
}

// Reads the DS ROM's file system: every file with its path, as
// tools/extract_rom.py extracts it for the desktop build.
func readNitroFS(rom []byte) ([]romFile, error) {
	if len(rom) < 0x200 {
		return nil, fmt.Errorf("the file is too small to be a DS ROM")
	}
	if code := string(rom[0x0C:0x10]); code != "CPUE" {
		return nil, fmt.Errorf("this is not Pokémon Platinum (USA): its game code is %q, not \"CPUE\"", code)
	}
	fnt := int(binary.LittleEndian.Uint32(rom[0x40:]))
	fntSize := int(binary.LittleEndian.Uint32(rom[0x44:]))
	fat := int(binary.LittleEndian.Uint32(rom[0x48:]))
	fatSize := int(binary.LittleEndian.Uint32(rom[0x4C:]))
	if fnt <= 0 || fat <= 0 || fnt+fntSize > len(rom) || fat+fatSize > len(rom) {
		return nil, fmt.Errorf("the ROM's file tables lie outside the file (is the ROM complete?)")
	}
	var files []romFile
	var walk func(dirID int, prefix string, depth int) error
	walk = func(dirID int, prefix string, depth int) error {
		if depth > 32 {
			return fmt.Errorf("the ROM's directories nest too deeply")
		}
		entry := fnt + (dirID&0xFFF)*8
		if entry+8 > fnt+fntSize {
			return fmt.Errorf("bad directory %#x in the ROM", dirID)
		}
		p := fnt + int(binary.LittleEndian.Uint32(rom[entry:]))
		fileID := int(binary.LittleEndian.Uint16(rom[entry+4:]))
		for {
			if p >= fnt+fntSize {
				return fmt.Errorf("the ROM's file name table is cut short")
			}
			tl := int(rom[p])
			p++
			if tl == 0 {
				return nil
			}
			n := tl & 0x7F
			if p+n > fnt+fntSize {
				return fmt.Errorf("the ROM's file name table is cut short")
			}
			name := string(rom[p : p+n])
			p += n
			if name == "" || name == "." || name == ".." || strings.ContainsAny(name, "/\\") {
				return fmt.Errorf("bad file name %q in the ROM", name)
			}
			if tl&0x80 != 0 {
				child := int(binary.LittleEndian.Uint16(rom[p:]))
				p += 2
				if err := walk(child, prefix+name+"/", depth+1); err != nil {
					return err
				}
				continue
			}
			at := fat + fileID*8
			if at+8 > fat+fatSize {
				return fmt.Errorf("bad file number %d in the ROM", fileID)
			}
			start := int(binary.LittleEndian.Uint32(rom[at:]))
			end := int(binary.LittleEndian.Uint32(rom[at+4:]))
			if start > end || end > len(rom) {
				return fmt.Errorf("%s lies outside the ROM (is the ROM complete?)", prefix+name)
			}
			files = append(files, romFile{prefix + name, rom[start:end]})
			fileID++
		}
	}
	if err := walk(0xF000, "", 0); err != nil {
		return nil, err
	}
	sort.Slice(files, func(i, j int) bool { return files[i].path < files[j].path })
	return files, nil
}

// The files the port reads, with their sizes and SHA-1 (templates/manifest.txt,
// from a Pokémon Platinum (USA) Rev 1 ROM: "sha1 size path" lines).
type manifestEntry struct {
	sha1 string
	size int
}

func parseManifest(text []byte) (map[string]manifestEntry, error) {
	m := map[string]manifestEntry{}
	sc := bufio.NewScanner(bytes.NewReader(text))
	for sc.Scan() {
		f := strings.SplitN(sc.Text(), " ", 3)
		if len(f) != 3 {
			continue
		}
		size, err := strconv.Atoi(f[1])
		if err != nil {
			return nil, err
		}
		m[f[2]] = manifestEntry{f[0], size}
	}
	return m, sc.Err()
}

// Checks the ROM's files against the manifest: the same files, byte for byte.
// Returns the paths that differ or are missing.
func verifyFiles(files []romFile, manifest map[string]manifestEntry) []string {
	var bad []string
	seen := map[string]bool{}
	for _, f := range files {
		seen[f.path] = true
		want, ok := manifest[f.path]
		if !ok {
			bad = append(bad, f.path+" (not in Platinum)")
			continue
		}
		sum := sha1.Sum(f.data)
		if len(f.data) != want.size || hex.EncodeToString(sum[:]) != want.sha1 {
			bad = append(bad, f.path)
		}
	}
	for path := range manifest {
		if !seen[path] {
			bad = append(bad, path+" (missing)")
		}
	}
	sort.Strings(bad)
	return bad
}

// The ROM's own 32x32 DS icon (the game's icon on the DS menu), as RGB with
// a transparency mask.
func dsIcon(rom []byte) (rgb [32][32][3]uint8, opaque [32][32]bool, err error) {
	banner := int(binary.LittleEndian.Uint32(rom[0x68:]))
	if banner == 0 || banner+0x240 > len(rom) {
		return rgb, opaque, fmt.Errorf("the ROM has no DS icon")
	}
	bitmap, palette := rom[banner+0x20:banner+0x220], rom[banner+0x220:banner+0x240]
	for y := 0; y < 32; y++ {
		for x := 0; x < 32; x++ {
			tile := (y/8)*4 + x/8
			b := bitmap[tile*32+(y%8)*4+(x%8)/2]
			idx := b & 0x0F
			if x%2 == 1 {
				idx = b >> 4
			}
			if idx == 0 {
				continue
			}
			c := binary.LittleEndian.Uint16(palette[idx*2:])
			r, g, bl := uint8(c&31), uint8(c>>5&31), uint8(c>>10&31)
			rgb[y][x] = [3]uint8{r<<3 | r>>2, g<<3 | g>>2, bl<<3 | bl>>2}
			opaque[y][x] = true
		}
	}
	return rgb, opaque, nil
}
