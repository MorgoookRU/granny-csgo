package org.modlab.granny;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

/** One automatic launch; an interrupted startup returns to diagnostics. */
public final class LauncherActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        try {
            if(state!=null||LaunchState.interrupted(this)) {
                CrashJournal.append(this,"Interrupted automatic startup; opening report instead of restarting");
                startActivity(new Intent(this,DiagnosticActivity.class));
            } else {
                LaunchState.launch(this,false,false);
            }
        } catch(Throwable failure) {
            CrashJournal.append(this,"Automatic launch failed\n"+CrashJournal.stack(failure));
            startActivity(new Intent(this,DiagnosticActivity.class));
        }
        finish();
    }
}
