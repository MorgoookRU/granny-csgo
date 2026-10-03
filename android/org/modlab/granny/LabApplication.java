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
    }
}
