package org.modlab.granny;

/** Minimal startup allows the diagnostic activity to run before Unity or SDKs. */
public final class LabApplication extends android.app.Application {
    @Override protected void attachBaseContext(android.content.Context base) {
        super.attachBaseContext(base);
        CrashJournal.install(this);
        CrashJournal.append(this, "Application attached; process=" +
            (android.os.Build.VERSION.SDK_INT >= 28 ? getProcessName() : "unknown"));
    }
    @Override public void onCreate() {
        super.onCreate();
        CrashJournal.append(this, "Application.onCreate complete");
        if(android.os.Build.VERSION.SDK_INT>=28&&getPackageName().equals(getProcessName())) {
            try {
                android.content.Intent intent=new android.content.Intent(this,DiagnosticActivity.class)
                    .setAction(android.content.Intent.ACTION_VIEW).putExtra("diagnostics",true);
                android.content.pm.ShortcutInfo shortcut=new android.content.pm.ShortcutInfo.Builder(this,"diagnostics")
                    .setShortLabel("Отчёт").setLongLabel("Отчёт Granny Tactical Lab").setIntent(intent).build();
                getSystemService(android.content.pm.ShortcutManager.class).setDynamicShortcuts(java.util.Collections.singletonList(shortcut));
            } catch(Exception failure) { CrashJournal.append(this,"Diagnostic shortcut unavailable: "+failure); }
        }
    }
}
