package io.github.mathibraps.platinum3hds;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;

/**
 * Picks a Pokémon Platinum (USA) ROM, runs the builder on a copy of it (the
 * same program as the desktop builders, in the app's native libraries) and
 * saves the CIA and 3DSX it makes wherever the user chooses.
 */
public class MainActivity extends Activity {
    private static final int PICK_ROM = 1, SAVE_CIA = 2, SAVE_3DSX = 3;
    private static final String CIA = "Platinum3HDS.cia", THREEDSX = "Platinum3HDS.3dsx";

    private TextView log;
    private ScrollView scroll;
    private Button pick, saveCia, save3dsx;
    private File outDir;
    private boolean busy;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        outDir = new File(getCacheDir(), "out");
        float dp = getResources().getDisplayMetrics().density;
        int pad = (int) (16 * dp);

        // The outer layout keeps clear of the system bars (its padding), the
        // inner one has the margins.
        LinearLayout outer = new LinearLayout(this);
        outer.setFitsSystemWindows(true);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);
        outer.addView(root, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.MATCH_PARENT));

        TextView title = new TextView(this);
        title.setText("Platinum 3HDS Builder");
        title.setTextSize(24);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);

        TextView about = new TextView(this);
        about.setText("Makes Platinum 3HDS for your 3DS from your own Pokémon Platinum (USA) ROM (.nds). "
            + "The game is not included: its files come from your ROM.\n\n"
            + "Install the CIA with FBI, or put the 3DSX in the SD card's 3ds folder.");
        about.setPadding(0, pad / 2, 0, pad);
        root.addView(about);

        pick = button(root, "Choose your ROM", v -> chooseROM());
        saveCia = button(root, "Save " + CIA, v -> saveAs(CIA, SAVE_CIA));
        save3dsx = button(root, "Save " + THREEDSX, v -> saveAs(THREEDSX, SAVE_3DSX));

        scroll = new ScrollView(this);
        log = new TextView(this);
        log.setTypeface(Typeface.MONOSPACE);
        log.setTextSize(12);
        log.setPadding(0, pad, 0, 0);
        scroll.addView(log);
        root.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        setContentView(outer);
        updateButtons();

        Intent intent = getIntent();
        String testRom = intent.getStringExtra("rom");
        Uri rom = null;
        if (Intent.ACTION_VIEW.equals(intent.getAction())) {
            rom = intent.getData();
        } else if (Intent.ACTION_SEND.equals(intent.getAction())) {
            rom = intent.getParcelableExtra(Intent.EXTRA_STREAM);
        }
        if (testRom != null) {
            buildFromTestFile(testRom);
        } else if (rom != null) {
            build(rom);
        }
    }

    private Button button(LinearLayout root, String text, View.OnClickListener onClick) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setOnClickListener(onClick);
        root.addView(b);
        return b;
    }

    private void updateButtons() {
        pick.setEnabled(!busy);
        saveCia.setEnabled(!busy && new File(outDir, CIA).isFile());
        save3dsx.setEnabled(!busy && new File(outDir, THREEDSX).isFile());
    }

    private void print(String line) {
        runOnUiThread(() -> {
            log.append(line + "\n");
            scroll.post(() -> scroll.fullScroll(View.FOCUS_DOWN));
        });
    }

    private void setBusy(boolean b) {
        runOnUiThread(() -> {
            busy = b;
            updateButtons();
        });
    }

    private void chooseROM() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, PICK_ROM);
    }

    private void saveAs(String name, int request) {
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("application/octet-stream");
        i.putExtra(Intent.EXTRA_TITLE, name);
        startActivityForResult(i, request);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        Uri uri = data.getData();
        if (request == PICK_ROM) {
            build(uri);
        } else if (request == SAVE_CIA || request == SAVE_3DSX) {
            String name = request == SAVE_CIA ? CIA : THREEDSX;
            setBusy(true);
            new Thread(() -> {
                try (InputStream in = new FileInputStream(new File(outDir, name));
                     OutputStream out = getContentResolver().openOutputStream(uri, "w")) {
                    copy(in, out);
                    print("Saved " + name + ".");
                } catch (IOException | RuntimeException e) {
                    print("Could not save " + name + ": " + e.getMessage());
                }
                setBusy(false);
            }).start();
        }
    }

    /** Copies the ROM into the app's cache (the builder reads a file), then builds from it. */
    private void build(Uri uri) {
        setBusy(true);
        runOnUiThread(() -> log.setText(""));
        new Thread(() -> {
            File rom = new File(getCacheDir(), "rom.nds");
            try {
                print("Copying the ROM");
                try (InputStream in = getContentResolver().openInputStream(uri);
                     OutputStream out = new FileOutputStream(rom)) {
                    if (in == null) {
                        throw new IOException("cannot open it");
                    }
                    copy(in, out);
                }
                runBuilder(rom);
            } catch (IOException | RuntimeException e) {
                print("Error: " + e.getMessage());
            } finally {
                rom.delete();
            }
            setBusy(false);
        }).start();
    }

    /** For tests (adb): a ROM in the app's external files folder, the game files left beside it. */
    private void buildFromTestFile(String path) {
        File dir = getExternalFilesDir(null);
        File rom = new File(path);
        try {
            if (dir == null || !rom.getCanonicalPath().startsWith(dir.getCanonicalPath() + File.separator)) {
                print("Error: test ROMs must be in " + dir);
                return;
            }
        } catch (IOException e) {
            print("Error: " + e.getMessage());
            return;
        }
        setBusy(true);
        new Thread(() -> {
            try {
                if (runBuilder(rom)) {
                    for (String name : new String[] { CIA, THREEDSX }) {
                        try (InputStream in = new FileInputStream(new File(outDir, name));
                             OutputStream out = new FileOutputStream(new File(dir, name))) {
                            copy(in, out);
                        }
                    }
                    print("Test: copied to " + dir);
                }
            } catch (IOException | RuntimeException e) {
                print("Error: " + e.getMessage());
            }
            setBusy(false);
        }).start();
    }

    /** Runs the builder program on a ROM file; its output goes to the log. */
    private boolean runBuilder(File rom) throws IOException {
        for (String name : new String[] { CIA, THREEDSX }) {
            new File(outDir, name).delete();
        }
        File exe = new File(getApplicationInfo().nativeLibraryDir, "libplatinum3hds.so");
        Process p = new ProcessBuilder(exe.getPath(), "--no-pause", "--out", outDir.getPath(), rom.getPath())
            .redirectErrorStream(true)
            .start();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream(), "UTF-8"))) {
            for (String line; (line = r.readLine()) != null; ) {
                print(line);
            }
        }
        try {
            int code = p.waitFor();
            if (code != 0) {
                print("The builder stopped (code " + code + ").");
                return false;
            }
        } catch (InterruptedException e) {
            p.destroy();
            return false;
        }
        print("\nNow save the CIA (and the 3DSX if you use the Homebrew Launcher).");
        return true;
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] buf = new byte[1 << 16];
        for (int n; (n = in.read(buf)) > 0; ) {
            out.write(buf, 0, n);
        }
    }
}
