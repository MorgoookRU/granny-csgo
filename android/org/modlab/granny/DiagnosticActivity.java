package org.modlab.granny;

import android.app.Activity;
import android.content.*;
import android.os.Bundle;
import android.widget.*;

/** Uses framework widgets only; no Unity, native libraries, sounds, or ad SDKs. */
public final class DiagnosticActivity extends Activity {
    private TextView report;
    private final android.os.Handler handler = new android.os.Handler();
    private int refreshId;
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        CrashJournal.append(this, "Diagnostic screen onCreate");
        LinearLayout layout = new LinearLayout(this); layout.setOrientation(LinearLayout.VERTICAL);
        int padding = (int)(16 * getResources().getDisplayMetrics().density); layout.setPadding(padding,padding,padding,padding);
        TextView title = new TextView(this); title.setText("Granny Tactical Lab · 4"); title.setTextSize(22); layout.addView(title);
        TextView hint = new TextView(this);
        hint.setText("Нажми «Запустить игру». Если она вылетит, вернись сюда и скопируй отчёт. Мод пока выключен.");
        layout.addView(hint);
        Button launch = new Button(this); launch.setText("Запустить игру"); layout.addView(launch);
        launch.setOnClickListener(v -> launch(false));
        Button gl = new Button(this); gl.setText("Запустить через OpenGL"); layout.addView(gl);
        gl.setOnClickListener(v -> launch(true));
        Button copy = new Button(this); copy.setText("Копировать отчёт"); layout.addView(copy);
        copy.setOnClickListener(v -> {
            ClipboardManager clipboard = (ClipboardManager)getSystemService(CLIPBOARD_SERVICE);
            clipboard.setPrimaryClip(ClipData.newPlainText("Granny crash report",report.getText()));
            Toast.makeText(this,"Отчёт скопирован — пришли его в чат",Toast.LENGTH_LONG).show();
        });
        Button refresh = new Button(this); refresh.setText("Обновить отчёт"); layout.addView(refresh);
        refresh.setOnClickListener(v -> refresh());
        ScrollView scroll = new ScrollView(this); report = new TextView(this); report.setTextSize(12); report.setTextIsSelectable(true);
        scroll.addView(report); layout.addView(scroll,new LinearLayout.LayoutParams(-1,0,1)); setContentView(layout);
    }
    private void launch(boolean openGL) {
        CrashJournal.append(this,"Request Unity launch; OpenGL=" + openGL);
        try {
            Intent intent = new Intent(); intent.setClassName(this,"com.unity3d.player.UnityPlayerActivity");
            if(openGL) intent.putExtra("unity","-force-gles");
            startActivity(intent);
        } catch (Throwable failure) { CrashJournal.append(this,"Launch failed\n" + CrashJournal.stack(failure)); refresh(); }
    }
    @Override protected void onResume() { super.onResume(); refresh(); }
    @Override protected void onPause() { ++refreshId; super.onPause(); }
    private void refresh() {
        final int request = ++refreshId; report.setText("Читаю журнал…");
        new Thread(() -> {
            final String text = CrashJournal.report(getApplicationContext());
            handler.post(() -> { if(request == refreshId && !isFinishing()) report.setText(text); });
        },"ReadCrashReport").start();
    }
}
