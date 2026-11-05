package com.probe.g52;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Bundle;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.ArrayList;

public class MainActivity extends Activity {
  private TextView text;
  private String result = "";

  @Override
  protected void onCreate(Bundle b) {
    super.onCreate(b);
    LinearLayout root = new LinearLayout(this);
    root.setOrientation(LinearLayout.VERTICAL);
    root.setPadding(24, 48, 24, 24);

    Button copy = new Button(this);
    copy.setText("Copy results");
    copy.setOnClickListener(v -> {
      ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
      cm.setPrimaryClip(ClipData.newPlainText("g52probe", result));
      Toast.makeText(this, "Copied", Toast.LENGTH_SHORT).show();
    });
    root.addView(copy);

    ScrollView sv = new ScrollView(this);
    text = new TextView(this);
    text.setTypeface(Typeface.MONOSPACE);
    text.setTextSize(10f);
    text.setTextIsSelectable(true);
    text.setText("Running Vulkan probe...");
    sv.addView(text);
    root.addView(sv, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
    setContentView(root);

    new Thread(() -> {
      String out;
      try {
        out = runProbe();
      } catch (Throwable t) {
        out = "Probe crashed in Java: " + t;
      }
      final String fin = out;
      runOnUiThread(() -> {
        result = fin;
        text.setText(fin);
      });
    }).start();
  }

  private String runProbe() throws Exception {
    String[] files = getAssets().list("");
    ArrayList<String> names = new ArrayList<>();
    ArrayList<byte[]> blobs = new ArrayList<>();
    if (files != null) {
      for (String f : files) {
        if (!f.endsWith(".spv")) continue;
        try (InputStream in = getAssets().open(f)) {
          ByteArrayOutputStream bo = new ByteArrayOutputStream();
          byte[] buf = new byte[8192];
          int n;
          while ((n = in.read(buf)) > 0) bo.write(buf, 0, n);
          names.add(f.substring(0, f.length() - 4));
          blobs.add(bo.toByteArray());
        }
      }
    }
    String header = "G52 Vulkan probe\nPhone: " + Build.MANUFACTURER + " " + Build.MODEL
        + " | Android " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")\n"
        + "SoC: " + (Build.VERSION.SDK_INT >= 31 ? Build.SOC_MODEL : "?") + "\n"
        + "Shaders loaded: " + names.size() + "\n\n";
    String body = Probe.run(names.toArray(new String[0]), blobs.toArray(new byte[0][]));
    String full = header + body;
    try {
      File dir = getExternalFilesDir(null);
      if (dir != null) {
        try (FileOutputStream fo = new FileOutputStream(new File(dir, "probe.txt"))) {
          fo.write(full.getBytes("UTF-8"));
        }
      }
    } catch (Exception ignored) {
    }
    return full;
  }
}
