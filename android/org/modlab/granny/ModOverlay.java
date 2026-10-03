package org.modlab.granny;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.media.AudioAttributes;
import android.media.SoundPool;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;

/** Offline controls for the private Granny Tactical Lab test build. */
public final class ModOverlay {
    private static native void nativeStart(String directory, int mode);
    private static native void nativeAction(int action, int value);
    private static native String nativeHud();
    private static ModOverlay instance;
    private static volatile boolean nativeLoaded;
    private TextView shopButton;
    private static SoundPool sounds;
    private static final int[] soundIds = new int[8];
    private final Activity activity;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Root root;
    private final Reticle reticle;
    private final TextView info, message;
    private final LinearLayout panel;
    private final TextView[] entries = new TextView[Weapons.NAMES.length];
    private final View[] gameButtons = new View[5];
    private boolean[] owned = new boolean[Weapons.NAMES.length];
    private int current, money;
    private boolean game, shop;
    private final Runnable refresh = new Runnable() {
        @Override public void run() {
            if (root.getWindowToken() == null) { action(0, 0); return; }
            update(); handler.postDelayed(this, 100);
        }
    };

    public static void attach(final Activity activity) {
        activity.runOnUiThread(new Runnable() {
            @Override public void run() {
                if (instance != null && instance.activity == activity) return;
                try {
                    instance = new ModOverlay(activity);
                } catch (Throwable failure) {
                    new AlertDialog.Builder(activity).setTitle("Granny Tactical Lab")
                        .setMessage("Мод не загрузился: " + failure.toString())
                        .setPositiveButton("OK", null).show();
                }
            }
        });
    }
    private static void action(int action, int value) {
        if (nativeLoaded) nativeAction(action, value);
    }
    private void writeJavaLog(String text) {
        try {
            java.io.FileOutputStream out = new java.io.FileOutputStream(new File(activity.getFilesDir(), "granny-csgo.log"), true);
            out.write(("[Java] " + text + "\n").getBytes("UTF-8")); out.close();
        } catch (Exception failure) { android.util.Log.w("GrannyCSGO", "Diagnostic log unavailable", failure); }
    }
    private void enableMod() {
        new AlertDialog.Builder(activity).setTitle("Подключить мод")
            .setMessage("Сначала проверь обычный запуск меню и Practice. Затем начни с оружия без ботов.")
            .setPositiveButton("Оружие без ботов", new android.content.DialogInterface.OnClickListener() {
                @Override public void onClick(android.content.DialogInterface dialog, int which) { startMod(1); }
            })
            .setNeutralButton("Оружие и боты", new android.content.DialogInterface.OnClickListener() {
                @Override public void onClick(android.content.DialogInterface dialog, int which) { startMod(2); }
            }).setNegativeButton("Пока выключен", null).show();
    }
    private void startMod(int mode) {
        if (nativeLoaded) return;
        writeJavaLog("Iteration 2; enabling native mode=" + mode);
        try {
            System.loadLibrary("granny_csgo");
            writeJavaLog("Native library loaded");
            nativeLoaded = true;
            nativeStart(activity.getFilesDir().getAbsolutePath(), mode);
            shopButton.setText("SHOP");
        } catch (Throwable failure) {
            nativeLoaded = false;
            writeJavaLog("Native load failed: " + failure.toString());
            message.setText("Мод не загрузился. Открой LOG.");
        }
    }
    private String previousExits() {
        if (android.os.Build.VERSION.SDK_INT < 30) return "";
        try {
            android.app.ActivityManager manager = (android.app.ActivityManager) activity.getSystemService(Context.ACTIVITY_SERVICE);
            StringBuilder out = new StringBuilder("\nПредыдущие завершения приложения:\n");
            for (android.app.ApplicationExitInfo exit : manager.getHistoricalProcessExitReasons(activity.getPackageName(), 0, 3)) {
                out.append(new java.util.Date(exit.getTimestamp())).append(" · reason=").append(exit.getReason())
                   .append(" · ").append(exit.getDescription()).append('\n');
            }
            return out.toString();
        } catch (Exception failure) { return "\nExit history unavailable: " + failure.getClass().getSimpleName(); }
    }
    public static void playShot(int kind, float volume) {
        SoundPool p = sounds;
        if (p != null && kind >= 0 && kind < soundIds.length && soundIds[kind] != 0)
            p.play(soundIds[kind], volume, volume, 1, 0, 1);
    }
    private int dp(float value) { return Math.round(value * activity.getResources().getDisplayMetrics().density); }
    private TextView text(String label, int size) {
        TextView v = new TextView(activity);
        v.setText(label); v.setTextSize(size); v.setTextColor(0xffdcf8d3);
        v.setGravity(Gravity.CENTER); v.setPadding(dp(8), dp(4), dp(8), dp(4));
        v.setBackgroundColor(0xcf101820); return v;
    }
    private TextView button(String label, int width, int height, int gravity, int x, int y, final int action, final int value) {
        TextView v = text(label, 13);
        FrameLayout.LayoutParams p = new FrameLayout.LayoutParams(dp(width), dp(height), gravity);
        if ((gravity & Gravity.RIGHT) == Gravity.RIGHT) p.rightMargin = dp(x); else p.leftMargin = dp(x);
        if ((gravity & Gravity.BOTTOM) == Gravity.BOTTOM) p.bottomMargin = dp(y); else p.topMargin = dp(y);
        root.addView(v, p); v.setClickable(true);
        v.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View view) { action(action, value); }
        });
        return v;
    }
    private ModOverlay(Activity activity) {
        this.activity = activity;
        root = new Root(activity); root.setClickable(false);
        reticle = new Reticle(activity); reticle.setClickable(false);
        root.addView(reticle, new FrameLayout.LayoutParams(-1, -1));

        shopButton = button("ВКЛ. МОД", 95, 37, Gravity.TOP|Gravity.LEFT, 8, 8, 0, 0);
        shopButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { if (!nativeLoaded) { enableMod(); return; } shop = !shop; panel.setVisibility(shop ? View.VISIBLE : View.GONE); action(0, 0); }
        });
        TextView logButton = button("LOG", 50, 37, Gravity.TOP|Gravity.RIGHT, 8, 8, 0, 0);
        logButton.setOnClickListener(new View.OnClickListener() { @Override public void onClick(View v) { showLog(); } });
        info = text("TACTICAL LAB · ITERATION 2", 12);
        FrameLayout.LayoutParams p = new FrameLayout.LayoutParams(-2, dp(37), Gravity.TOP|Gravity.CENTER_HORIZONTAL);
        p.topMargin = dp(8); root.addView(info, p);
        message = text("Подключение мода…", 11);
        p = new FrameLayout.LayoutParams(-2, dp(27), Gravity.TOP|Gravity.CENTER_HORIZONTAL);p.topMargin=dp(48);root.addView(message,p);

        TextView fire = button("ОГОНЬ", 90, 72, Gravity.BOTTOM|Gravity.RIGHT, 20, 20, 0, 0);
        fire.setTextColor(0xffffde85);fire.setTextSize(17);
        fire.setOnClickListener(null);
        fire.setOnTouchListener(new View.OnTouchListener() {
            @Override public boolean onTouch(View v, MotionEvent event) {
                int a=event.getActionMasked();
                if(a==MotionEvent.ACTION_DOWN){action(0,1);v.setBackgroundColor(0xe0504a20);return true;}
                if(a==MotionEvent.ACTION_UP||a==MotionEvent.ACTION_CANCEL){action(0,0);v.setBackgroundColor(0xcf101820);return true;}
                return true;
            }
        });gameButtons[0]=fire;
        gameButtons[1]=button("R",55,43,Gravity.BOTTOM|Gravity.RIGHT,125,20,1,0);
        gameButtons[2]=button("ALT",55,43,Gravity.BOTTOM|Gravity.RIGHT,125,72,4,0);
        gameButtons[3]=button("JUMP",65,43,Gravity.BOTTOM|Gravity.RIGHT,20,102,5,0);
        TextView next=button("⇄",55,43,Gravity.BOTTOM|Gravity.RIGHT,125,124,0,0);
        next.setOnClickListener(new View.OnClickListener(){@Override public void onClick(View v){for(int i=1;i<=owned.length;i++){int n=(current+i)%owned.length;if(owned[n]){action(3,n);break;}}}});gameButtons[4]=next;

        panel = new LinearLayout(activity);panel.setOrientation(LinearLayout.VERTICAL);panel.setBackgroundColor(0xf0101820);
        p=new FrameLayout.LayoutParams(dp(320),-1,Gravity.LEFT|Gravity.TOP);p.leftMargin=dp(8);p.topMargin=dp(80);p.bottomMargin=dp(8);root.addView(panel,p);panel.setVisibility(View.GONE);
        TextView title=text("АРСЕНАЛ · $16000 на старте",13);panel.addView(title,new LinearLayout.LayoutParams(-1,dp(37)));
        LinearLayout controls=new LinearLayout(activity);
        TextView bots=text("4 БОТА",12),armor=text("БРОНЯ $1000",12);
        controls.addView(bots,new LinearLayout.LayoutParams(0,dp(38),1));controls.addView(armor,new LinearLayout.LayoutParams(0,dp(38),1));panel.addView(controls);
        bots.setOnClickListener(new View.OnClickListener(){@Override public void onClick(View v){action(6,0);}});
        armor.setOnClickListener(new View.OnClickListener(){@Override public void onClick(View v){action(7,0);}});
        ScrollView scroll=new ScrollView(activity);LinearLayout list=new LinearLayout(activity);list.setOrientation(LinearLayout.VERTICAL);scroll.addView(list);panel.addView(scroll,new LinearLayout.LayoutParams(-1,0,1));
        String group="";
        for(int i=0;i<Weapons.NAMES.length;i++){
            if(!group.equals(Weapons.GROUPS[i])){group=Weapons.GROUPS[i];TextView header=text(group.toUpperCase(),12);header.setTextColor(0xff8ac978);list.addView(header,new LinearLayout.LayoutParams(-1,dp(30)));}
            final int id=i;TextView row=text(Weapons.NAMES[i]+" · $"+Weapons.PRICES[i],13);row.setGravity(Gravity.CENTER_VERTICAL|Gravity.LEFT);entries[i]=row;
            LinearLayout.LayoutParams rowParams=new LinearLayout.LayoutParams(-1,dp(40));rowParams.bottomMargin=dp(2);list.addView(row,rowParams);
            row.setOnClickListener(new View.OnClickListener(){@Override public void onClick(View v){if(owned[id]){action(3,id);}else{action(2,id);}shop=false;panel.setVisibility(View.GONE);}});
            row.setOnLongClickListener(new View.OnLongClickListener(){@Override public boolean onLongClick(View v){action(2,id);return true;}});
        }
        activity.addContentView(root,new ViewGroup.LayoutParams(-1,-1));
        writeJavaLog("Iteration 2 overlay attached; native mod remains disabled until requested");
        sounds=new SoundPool.Builder().setMaxStreams(10).setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME).setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build()).build();
        for(int i=0;i<8;i++){try{soundIds[i]=sounds.load(activity.getAssets().openFd("granny_csgo/shot"+i+".wav"),1);}catch(Exception e){android.util.Log.w("GrannyCSGO","Sound loading",e);}}
        handler.postDelayed(refresh,250);
    }
    private void update(){
        try{
            JSONObject data=new JSONObject(nativeLoaded ? nativeHud() : "{\"game\":false,\"status\":\"Мод выключен. Проверь меню и Practice, затем нажми ВКЛ. МОД.\"}");game=data.optBoolean("game");current=data.optInt("weapon");money=data.optInt("money");
            if(current<0||current>=Weapons.NAMES.length)current=0;
            String status=data.optString("status","Подключение…");
            if(game){
                String ammo=data.optBoolean("reload")?"ПЕРЕЗАРЯДКА":data.optInt("ammo")+" / "+data.optInt("reserve");
                info.setText("HP "+data.optInt("health")+" · ARM "+data.optInt("armor")+" · $"+money+" · "+Weapons.NAMES[current]+" · "+ammo);
                String feed=data.optString("killfeed");message.setText(feed.isEmpty()?data.optString("message"):feed);
            }else{info.setText("TACTICAL LAB · ITERATION 2");message.setText(status);}
            JSONArray inventory=data.optJSONArray("owned");if(inventory!=null)for(int i=0;i<owned.length;i++)owned[i]=inventory.optBoolean(i);
            for(int i=0;i<entries.length;i++){
                entries[i].setText((i==current?"▶ ":"")+Weapons.NAMES[i]+(owned[i]?" · В ИНВЕНТАРЕ":" · $"+Weapons.PRICES[i]));
                entries[i].setTextColor(owned[i]?0xffdcf8d3:(money>=Weapons.PRICES[i]?0xffeeeeee:0xff818a8c));
            }
            for(View button:gameButtons)button.setVisibility(game&&!shop?View.VISIBLE:View.GONE);
            reticle.game=game;reticle.scope=data.optBoolean("scope");reticle.flash=(float)data.optDouble("flash");reticle.invalidate();
        }catch(Exception failure){message.setText("Ошибка HUD: "+failure.getClass().getSimpleName());}
    }
    private void showLog(){
        action(0,0);String value;
        try{
            File f=new File(activity.getFilesDir(),"granny-csgo.log");FileInputStream in=new FileInputStream(f);ByteArrayOutputStream out=new ByteArrayOutputStream();byte[] buffer=new byte[4096];int n;
            while((n=in.read(buffer))>0&&out.size()<64000)out.write(buffer,0,n);in.close();value=out.toString("UTF-8");
        }catch(Exception e){value="Журнал пока пуст: "+e.getClass().getSimpleName();}
        final String content="Iteration 2 · "+android.os.Build.MODEL+" · Android "+android.os.Build.VERSION.RELEASE+"\n"+value+previousExits();
        new AlertDialog.Builder(activity).setTitle("Журнал мода").setMessage(content)
            .setPositiveButton("Закрыть",null).setNeutralButton("Копировать",new android.content.DialogInterface.OnClickListener(){@Override public void onClick(android.content.DialogInterface dialog,int which){ClipboardManager clipboard=(ClipboardManager)activity.getSystemService(Context.CLIPBOARD_SERVICE);clipboard.setPrimaryClip(ClipData.newPlainText("Granny mod log",content));}}).show();
    }
    private static final class Root extends FrameLayout {
        Root(Context c){super(c);}
        @Override public void onWindowFocusChanged(boolean focused){super.onWindowFocusChanged(focused);action(8,focused?0:1);}
        @Override protected void onDetachedFromWindow(){action(0,0);action(8,1);super.onDetachedFromWindow();}
    }
    private static final class Reticle extends View {
        boolean game,scope;float flash;final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
        Reticle(Context c){super(c);}
        @Override protected void onDraw(Canvas canvas){
            super.onDraw(canvas);float x=getWidth()*.5f,y=getHeight()*.5f,d=getResources().getDisplayMetrics().density;
            if(game){
                if(scope){Path path=new Path();path.setFillType(Path.FillType.EVEN_ODD);path.addRect(0,0,getWidth(),getHeight(),Path.Direction.CW);path.addCircle(x,y,getHeight()*.44f,Path.Direction.CW);paint.setColor(0xe8000000);paint.setStyle(Paint.Style.FILL);canvas.drawPath(path,paint);paint.setColor(Color.BLACK);paint.setStrokeWidth(d);canvas.drawLine(x,0,x,getHeight(),paint);canvas.drawLine(0,y,getWidth(),y,paint);}
                else{paint.setColor(0xffb6ff68);paint.setStrokeWidth(1.6f*d);float gap=4*d,arm=10*d;canvas.drawLine(x-arm,y,x-gap,y,paint);canvas.drawLine(x+gap,y,x+arm,y,paint);canvas.drawLine(x,y-arm,x,y-gap,paint);canvas.drawLine(x,y+gap,x,y+arm,paint);}
            }
            if(flash>0){paint.setColor(Color.argb((int)(Math.min(flash,1)*235),255,255,255));canvas.drawRect(0,0,getWidth(),getHeight(),paint);}
        }
    }
}
