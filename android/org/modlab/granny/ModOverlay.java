package org.modlab.granny;

import android.app.Activity;
import android.app.Application;
import android.media.AudioAttributes;
import android.media.SoundPool;
import android.os.Bundle;

/** Nonvisual Android bridge. All gameplay controls are created in Unity. */
public final class ModOverlay {
    private static native void nativeStart(String directory,int mode,String token);
    private static native void nativeAction(int action,int value);
    private static boolean nativeLoaded,lifecycleInstalled;
    private static SoundPool sounds;
    private static final int[] soundIds=new int[8];
    public static void attach(final Activity activity) {
        CrashJournal.append(activity,"Iteration 10 Java bridge attached; no Android gameplay views");
        if(activity.getIntent().getBooleanExtra("without_mod",false)) {
            CrashJournal.append(activity,"Explicit diagnostic launch without native module");
            return;
        }
        try {
            if(sounds==null) {
                sounds=new SoundPool.Builder().setMaxStreams(10).setAudioAttributes(new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_GAME).setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build()).build();
                for(int i=0;i<8;i++) {
                    try(android.content.res.AssetFileDescriptor sound=activity.getAssets().openFd("granny_csgo/shot"+i+".wav")) {
                        soundIds[i]=sounds.load(sound,1);
                    } catch(Exception failure) { android.util.Log.w("GrannyCSGO","Sound loading",failure); }
                }
            }
            if(!lifecycleInstalled) {
                lifecycleInstalled=true;
                activity.getApplication().registerActivityLifecycleCallbacks(new Application.ActivityLifecycleCallbacks() {
                    public void onActivityCreated(Activity a,Bundle b) { }
                    public void onActivityStarted(Activity a) { }
                    public void onActivityResumed(Activity a) { if(nativeLoaded) nativeAction(8,0); }
                    public void onActivityPaused(Activity a) { if(nativeLoaded) nativeAction(8,1); }
                    public void onActivityStopped(Activity a) { }
                    public void onActivitySaveInstanceState(Activity a,Bundle b) { }
                    public void onActivityDestroyed(Activity a) { }
                });
            }
            if(!nativeLoaded) {
                CrashJournal.append(activity,"Automatic native startup; weapons and bots");
                System.loadLibrary("granny_csgo");nativeLoaded=true;
                nativeStart(activity.getFilesDir().getAbsolutePath(),2,activity.getIntent().getStringExtra("launch_token"));
                CrashJournal.append(activity,"Native module loaded and bootstrap requested");
            }
            nativeAction(8,0);
        } catch(Throwable failure) {
            nativeLoaded=false;
            CrashJournal.append(activity,"Native startup failed\n"+CrashJournal.stack(failure));
        }
    }
    public static void playShot(int kind,float volume) {
        SoundPool pool=sounds;
        if(pool!=null&&kind>=0&&kind<soundIds.length&&soundIds[kind]!=0)
            pool.play(soundIds[kind],volume,volume,1,0,1);
    }
}
