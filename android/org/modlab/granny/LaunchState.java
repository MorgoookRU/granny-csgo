package org.modlab.granny;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Context;
import android.content.Intent;
import java.io.File;
import java.util.UUID;

final class LaunchState {
    private static final String PREFS="launch-state-7",TOKEN="pending-token";
    static boolean interrupted(Context context) {
        String token=context.getSharedPreferences(PREFS,0).getString(TOKEN,"");
        return !token.isEmpty()&&!new File(context.getFilesDir(),"unity-ready-"+token).isFile();
    }
    static void launch(Activity activity,boolean openGL,boolean withoutMod) {
        String previous=activity.getSharedPreferences(PREFS,0).getString(TOKEN,"");
        boolean running=false;
        ActivityManager manager=(ActivityManager)activity.getSystemService(Context.ACTIVITY_SERVICE);
        java.util.List<ActivityManager.RunningAppProcessInfo> processes=manager.getRunningAppProcesses();
        if(processes!=null)for(ActivityManager.RunningAppProcessInfo process:processes)
            if(process.uid==android.os.Process.myUid()&&process.processName.equals(activity.getPackageName()+":game"))running=true;
        String token=running&&!previous.isEmpty()?previous:UUID.randomUUID().toString();
        activity.getSharedPreferences(PREFS,0).edit().putString(TOKEN,token).commit();
        Intent intent=new Intent();intent.setClassName(activity,"com.unity3d.player.UnityPlayerActivity");
        intent.putExtra("launch_token",token).putExtra("without_mod",withoutMod);
        if(openGL)intent.putExtra("unity","-force-gles");
        CrashJournal.append(activity,"Starting one Unity session token="+token+"; mod="+!withoutMod);
        activity.startActivity(intent);
    }
    static void stopGame(Context context) {
        try {
            ActivityManager manager=(ActivityManager)context.getSystemService(Context.ACTIVITY_SERVICE);
            java.util.List<ActivityManager.RunningAppProcessInfo> processes=manager.getRunningAppProcesses();
            if(processes==null)return;
            for(ActivityManager.RunningAppProcessInfo process:processes) {
                if(process.uid==android.os.Process.myUid()&&process.processName.equals(context.getPackageName()+":game")) {
                    CrashJournal.append(context,"Stop own game process pid="+process.pid);
                    android.os.Process.killProcess(process.pid);
                }
            }
        } catch(Exception failure) { CrashJournal.append(context,"Could not stop game: "+failure); }
    }
}
