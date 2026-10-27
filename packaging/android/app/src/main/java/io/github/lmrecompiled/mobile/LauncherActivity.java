package io.github.lmrecompiled.mobile;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

public class LauncherActivity extends Activity {
    public static final String EXTRA_STAY = "stay";

    private static final int PICK_DISC = 1;
    private static final int PICK_MODULE = 2;
    private static final String[] DISC_EXTENSIONS = {".iso", ".gcm", ".rvz", ".ciso", ".gcz", ".wia", ".nfs", ".tgc"};
    private static final byte[] MODULE_MARKER = "LMMODKEY:".getBytes(StandardCharsets.US_ASCII);

    private File folder;
    private TextView discStatus;
    private TextView moduleStatus;
    private TextView message;
    private Button discButton;
    private Button moduleButton;
    private Button playButton;
    private ProgressBar progress;
    private volatile boolean busy;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        folder = getExternalFilesDir(null);
        if (folder == null) {
            folder = getFilesDir();
        }
        folder.mkdirs();
        buildUi();
        refresh();
        if (findDisc() != null && findModule() != null && !getIntent().getBooleanExtra(EXTRA_STAY, false)) {
            startGame();
        }
    }

    private void buildUi() {
        float dp = getResources().getDisplayMetrics().density;
        int pad = (int) (20 * dp);

        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setPadding(pad, pad, pad, pad);
        column.setBackgroundColor(Color.rgb(0x1B, 0x10, 0x33));

        TextView title = text(getString(R.string.app_name), 26, Color.WHITE);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        column.addView(title);

        TextView warning = text(getString(R.string.launcher_vibe_coded), 14, Color.rgb(0xFF, 0xCC, 0x4D));
        column.addView(warning);

        column.addView(text(getString(R.string.launcher_intro), 15, Color.rgb(0xDD, 0xDD, 0xEE)));

        discStatus = text("", 15, Color.WHITE);
        column.addView(discStatus);
        discButton = button(getString(R.string.launcher_pick_disc));
        discButton.setOnClickListener(v -> pick(PICK_DISC));
        column.addView(discButton);

        moduleStatus = text("", 15, Color.WHITE);
        column.addView(moduleStatus);
        moduleButton = button(getString(R.string.launcher_pick_module));
        moduleButton.setOnClickListener(v -> pick(PICK_MODULE));
        column.addView(moduleButton);

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progress.setVisibility(ProgressBar.GONE);
        column.addView(progress, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        message = text("", 14, Color.rgb(0x9C, 0xFF, 0x8F));
        column.addView(message);

        playButton = button(getString(R.string.launcher_play));
        playButton.setOnClickListener(v -> startGame());
        column.addView(playButton);

        TextView where = text(getString(R.string.launcher_folder, folder.getAbsolutePath()), 12,
                Color.rgb(0x99, 0x99, 0xAA));
        column.addView(where);

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(Color.rgb(0x1B, 0x10, 0x33));
        scroll.addView(column);
        setContentView(scroll);
    }

    private TextView text(String value, int sp, int color) {
        TextView t = new TextView(this);
        t.setText(value);
        t.setTextSize(sp);
        t.setTextColor(color);
        int gap = (int) (8 * getResources().getDisplayMetrics().density);
        t.setPadding(0, gap, 0, gap);
        return t;
    }

    private Button button(String label) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setGravity(Gravity.CENTER);
        return b;
    }

    private void refresh() {
        File disc = findDisc();
        File module = findModule();
        discStatus.setText(disc != null ? getString(R.string.launcher_disc_ready, disc.getName())
                : getString(R.string.launcher_disc_missing));
        moduleStatus.setText(module != null ? getString(R.string.launcher_module_ready, module.getName())
                : getString(R.string.launcher_module_missing));
        discButton.setEnabled(!busy);
        moduleButton.setEnabled(!busy);
        playButton.setEnabled(!busy && disc != null && module != null);
    }

    private File findDisc() {
        File[] files = folder.listFiles();
        if (files == null) {
            return null;
        }
        for (File f : files) {
            String name = f.getName().toLowerCase(Locale.ROOT);
            for (String ext : DISC_EXTENSIONS) {
                if (f.isFile() && name.endsWith(ext)) {
                    return f;
                }
            }
        }
        return null;
    }

    private File findModule() {
        File[] files = folder.listFiles();
        if (files == null) {
            return null;
        }
        for (File f : files) {
            String name = f.getName();
            if (f.isFile() && name.startsWith("lm_game_") && name.endsWith(".so")) {
                return f;
            }
        }
        return null;
    }

    private void pick(int what) {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, what);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        Uri uri = data.getData();
        busy = true;
        message.setText(getString(R.string.launcher_copying));
        progress.setProgress(0);
        progress.setVisibility(ProgressBar.VISIBLE);
        refresh();
        new Thread(() -> {
            String result;
            boolean ok;
            try {
                result = requestCode == PICK_DISC ? importDisc(uri) : importModule(uri);
                ok = true;
            } catch (IOException e) {
                result = e.getMessage() != null ? e.getMessage() : getString(R.string.launcher_copy_failed);
                ok = false;
            }
            String shown = result;
            boolean success = ok;
            runOnUiThread(() -> {
                busy = false;
                progress.setVisibility(ProgressBar.GONE);
                message.setTextColor(success ? Color.rgb(0x9C, 0xFF, 0x8F) : Color.rgb(0xFF, 0x73, 0x66));
                message.setText(shown);
                refresh();
            });
        }).start();
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                String name = c.getString(0);
                if (name != null) {
                    return name.replaceAll("[\\\\/:*?\"<>|]", "_");
                }
            }
        }
        return "disc.iso";
    }

    private long size(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) {
                return c.getLong(0);
            }
        }
        return -1;
    }

    private File copy(Uri uri) throws IOException {
        File tmp = new File(folder, ".import.tmp");
        long total = size(uri);
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(tmp)) {
            if (in == null) {
                throw new IOException(getString(R.string.launcher_copy_failed));
            }
            byte[] buf = new byte[1 << 20];
            long done = 0;
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
                done += n;
                if (total > 0) {
                    int value = (int) (done * 1000 / total);
                    runOnUiThread(() -> progress.setProgress(value));
                }
            }
        } catch (IOException e) {
            tmp.delete();
            throw e;
        }
        return tmp;
    }

    private String importDisc(Uri uri) throws IOException {
        String name = displayName(uri);
        File tmp = copy(uri);
        String lower = name.toLowerCase(Locale.ROOT);
        boolean known = false;
        for (String ext : DISC_EXTENSIONS) {
            known |= lower.endsWith(ext);
        }
        if (!known) {
            name = name + discExtension(tmp);
        }
        if (isModule(tmp) != null) {
            tmp.delete();
            throw new IOException(getString(R.string.launcher_module_not_disc));
        }
        File dst = new File(folder, name);
        dst.delete();
        if (!tmp.renameTo(dst)) {
            tmp.delete();
            throw new IOException(getString(R.string.launcher_copy_failed));
        }
        return getString(R.string.launcher_disc_imported, name);
    }

    private String importModule(Uri uri) throws IOException {
        File tmp = copy(uri);
        String game = isModule(tmp);
        if (game == null) {
            tmp.delete();
            throw new IOException(getString(R.string.launcher_not_module));
        }
        File dst = new File(folder, "lm_game_" + game + ".so");
        dst.delete();
        if (!tmp.renameTo(dst)) {
            tmp.delete();
            throw new IOException(getString(R.string.launcher_copy_failed));
        }
        return getString(R.string.launcher_module_imported, game);
    }

    private String discExtension(File f) throws IOException {
        byte[] m = new byte[4];
        try (InputStream in = new FileInputStream(f)) {
            if (in.read(m) != 4) {
                return ".iso";
            }
        }
        String s = new String(m, StandardCharsets.ISO_8859_1);
        if (s.equals("RVZ\u0001")) {
            return ".rvz";
        }
        if (s.equals("WIA\u0001")) {
            return ".wia";
        }
        if (s.equals("CISO")) {
            return ".ciso";
        }
        if ((m[0] & 0xFF) == 0x01 && (m[1] & 0xFF) == 0xC0 && (m[2] & 0xFF) == 0x0B && (m[3] & 0xFF) == 0xB1) {
            return ".gcz";
        }
        return ".iso";
    }

    private String isModule(File f) throws IOException {
        try (InputStream in = new FileInputStream(f)) {
            byte[] head = new byte[4];
            if (in.read(head) != 4 || head[0] != 0x7F || head[1] != 'E' || head[2] != 'L' || head[3] != 'F') {
                return null;
            }
            int keep = MODULE_MARKER.length + 40 + 1 + 6;
            byte[] buf = new byte[(1 << 20) + keep];
            int filled = 0;
            int n;
            while ((n = in.read(buf, filled, buf.length - filled)) > 0) {
                filled += n;
                for (int i = 0; i + keep <= filled; i++) {
                    if (matches(buf, i)) {
                        String game = new String(buf, i + MODULE_MARKER.length + 41, 6, StandardCharsets.US_ASCII);
                        if (game.equals("GLME01") || game.equals("GLMP01")) {
                            return game;
                        }
                    }
                }
                int carry = Math.min(keep, filled);
                System.arraycopy(buf, filled - carry, buf, 0, carry);
                filled = carry;
            }
        }
        return null;
    }

    private boolean matches(byte[] buf, int at) {
        for (int i = 0; i < MODULE_MARKER.length; i++) {
            if (buf[at + i] != MODULE_MARKER[i]) {
                return false;
            }
        }
        return true;
    }

    private void startGame() {
        startActivity(new Intent(this, LMActivity.class));
        finish();
    }
}
