package org.modlab.granny;

import android.content.Context;
import java.io.*;
import java.util.Date;

/** Crash information is stored locally and copied only at the user's request. */
public final class CrashJournal {
    static final String FILE = "startup-diagnostic.log";
    public static void unityStarting(Context context) {
        append(context,"UnityPlayerActivity.onCreate entered");
        append(context,resourceStatus(context, "game_view_content_description", "string"));
        append(context,resourceStatus(context, "unitySurfaceView", "id"));
    }
    private static String resourceStatus(Context context, String name, String type) {
        try {
            android.content.res.Resources resources = context.getResources();
            int direct = resources.getIdentifier(name,type,context.getPackageName());
            int id = ResourceLookup.identifier(resources,name,type,context.getPackageName());
            String status = type + "/" + name + " direct=0x" + Integer.toHexString(direct) + " resolved=0x" + Integer.toHexString(id);
            if(id != 0) status += " package=" + resources.getResourcePackageName(id);
            if(id != 0 && "string".equals(type)) status += " value=" + resources.getString(id);
            return status;
        } catch (Exception failure) { return name + " lookup failed: " + failure; }
    }
    public static synchronized void append(Context context, String message) {
        try (FileOutputStream stream = new FileOutputStream(new File(context.getFilesDir(), FILE), true)) {
            stream.write((new Date() + " pid=" + android.os.Process.myPid() + " " + message + "\n").getBytes("UTF-8"));
        } catch (Exception ignored) { android.util.Log.e("GrannyStartup", message); }
    }
    static String stack(Throwable error) {
        StringWriter text = new StringWriter(); error.printStackTrace(new PrintWriter(text)); return text.toString();
    }
    static void install(final Context context) {
        final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler(new Thread.UncaughtExceptionHandler() {
            public void uncaughtException(Thread thread, Throwable error) {
                append(context, "UNCAUGHT thread=" + thread.getName() + "\n" + stack(error));
                if (previous != null) previous.uncaughtException(thread, error);
                else { android.os.Process.killProcess(android.os.Process.myPid()); System.exit(10); }
            }
        });
    }
    static String read(InputStream stream, int limit) throws IOException {
        if (stream == null) return "(нет трассировки)\n";
        try (InputStream input = stream; ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[4096]; int count;
            while (output.size() < limit && (count = input.read(buffer, 0, Math.min(buffer.length, limit - output.size()))) > 0)
                output.write(buffer, 0, count);
            byte[] data = output.toByteArray();
            for (int i = 0; i < Math.min(256, data.length); i++) if (data[i] == 0)
                return "(Android предоставил двоичную трассировку, " + data.length + " байт)\n";
            return new String(data, "UTF-8");
        }
    }
    static String file(Context context, String filename) {
        try { return read(new FileInputStream(new File(context.getFilesDir(), filename)), 60000); }
        catch (Exception error) { return "(файл отсутствует)\n"; }
    }
    public static String report(Context context) {
        StringBuilder out = new StringBuilder("Granny Tactical Lab · iteration 5\n");
        out.append(android.os.Build.MANUFACTURER).append(' ').append(android.os.Build.MODEL)
           .append(" Android ").append(android.os.Build.VERSION.RELEASE).append(" API ").append(android.os.Build.VERSION.SDK_INT)
           .append("\nABI ").append(java.util.Arrays.toString(android.os.Build.SUPPORTED_ABIS)).append('\n');
        try { out.append("page size=").append(android.system.Os.sysconf(android.system.OsConstants._SC_PAGESIZE)).append('\n'); }
        catch (Exception ignored) { }
        out.append("Unity runs in :game; native mod defaults OFF; SDK startup providers disabled.\n");
        out.append("\nRESOURCE LOOKUP\n").append(resourceStatus(context,"game_view_content_description","string"))
           .append('\n').append(resourceStatus(context,"unitySurfaceView","id")).append('\n');
        out.append("\nSTARTUP JOURNAL\n").append(file(context, FILE));
        out.append("\nMOD JOURNAL\n").append(file(context, "granny-csgo.log"));
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            try {
                android.app.ActivityManager manager = (android.app.ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
                for (android.app.ApplicationExitInfo exit : manager.getHistoricalProcessExitReasons(context.getPackageName(), 0, 8)) {
                    out.append("\nEXIT ").append(new Date(exit.getTimestamp())).append(" process=").append(exit.getProcessName())
                       .append(" reason=").append(exit.getReason()).append(" status=").append(exit.getStatus())
                       .append(" description=").append(exit.getDescription()).append('\n');
                    try { out.append(read(exit.getTraceInputStream(), 50000)); }
                    catch (Exception failure) { out.append("Trace unavailable: ").append(failure).append('\n'); }
                }
            } catch (Exception failure) { out.append("Exit history unavailable: ").append(failure).append('\n'); }
        }
        return out.toString();
    }
}
