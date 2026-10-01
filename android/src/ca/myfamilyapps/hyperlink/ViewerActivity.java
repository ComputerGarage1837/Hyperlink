package ca.myfamilyapps.hyperlink;

import android.app.*;
import android.os.*;
import android.graphics.*;
import android.view.*;
import android.view.inputmethod.*;
import android.widget.*;
import android.text.InputType;
import org.json.*;

/** Attended, pinned desktop viewer. No credentials or desktop frames are persisted. */
public final class ViewerActivity extends Activity implements Remote.Listener {
    private Remote remote;
    private SessionTools tools;
    private volatile boolean selectingDocument;
    private Screen screen;
    private TextView status;
    private LinearLayout controls;
    private volatile boolean stopped;
    private volatile int frameWidth, frameHeight;
    private boolean control, textInput, direct;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Object frames = new Object();
    private Bitmap pending;
    private boolean presentationQueued;
    private long received;

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_SECURE);
        LinearLayout root = MainActivity.root(this);
        status = new TextView(this); status.setTextColor(Color.WHITE);
        status.setText("Connecting · owner approval required"); root.addView(status);
        LinearLayout toolbar = new LinearLayout(this); root.addView(toolbar);
        button(toolbar,"Disconnect",() -> finish());
        button(toolbar,"Tools",() -> {
            if(remote==null || !remote.extensions){status.setText("Session tools require a connected Hyperlink 0.4 host");return;}
            if(tools==null || tools.isClosed())tools=new SessionTools(this,remote);
            tools.show();
        });
        button(toolbar,"Touchpad",() -> { direct=!direct; status.setText(direct?"Direct touch":"Touchpad"); });
        button(toolbar,"Keyboard",() -> {
            if(!control)return;
            screen.requestFocus();
            ((InputMethodManager)getSystemService(INPUT_METHOD_SERVICE)).showSoftInput(screen,InputMethodManager.SHOW_IMPLICIT);
        });
        HorizontalScrollView scroll = new HorizontalScrollView(this);
        controls = new LinearLayout(this); scroll.addView(controls); root.addView(scroll);
        button(controls,"Left",() -> {if(remote!=null)remote.click(0);});
        button(controls,"Right",() -> {if(remote!=null)remote.click(1);});
        for(String label:new String[]{"Ctrl","Alt","Shift","Win"}) {
            final int key=label.equals("Ctrl")?17:label.equals("Alt")?18:label.equals("Shift")?16:91;
            ToggleButton toggle=new ToggleButton(this); toggle.setTextOn(label+" ✓"); toggle.setTextOff(label); toggle.setChecked(false);
            toggle.setOnCheckedChangeListener((b,on) -> {if(remote!=null)remote.key(key,on);}); controls.addView(toggle);
        }
        button(controls,"Tab",() -> {if(remote!=null)remote.stroke(9);});
        button(controls,"Esc",() -> {if(remote!=null)remote.stroke(27);});
        screen=new Screen(); root.addView(screen,new LinearLayout.LayoutParams(-1,0,1)); setContentView(root);
        enableControls(false);
        final String peer=getIntent().getStringExtra("peer");
        Thread init=new Thread(() -> {
            try {
                Identity identity=new Identity(getApplicationContext());
                JSONObject parsed=Json.parse(peer.getBytes(java.nio.charset.StandardCharsets.UTF_8),8192);
                main.post(() -> {if(stopped)return; remote=new Remote(identity,this); remote.connect(parsed);});
            } catch(Exception e) {ended("Could not open this saved computer");}
        },"Hyperlink viewer identity"); init.setDaemon(true); init.start();
    }
    private void button(LinearLayout parent,String label,Runnable action) {
        Button button=new Button(this); button.setText(label); button.setOnClickListener(v -> action.run()); parent.addView(button);
    }
    private void enableControls(boolean enabled) {for(int i=0;i<controls.getChildCount();i++)controls.getChildAt(i).setEnabled(enabled);}
    public void ready(int width,int height,boolean allowed,boolean text) {
        frameWidth=width; frameHeight=height;
        main.post(() -> {if(stopped)return;control=allowed;textInput=text;enableControls(allowed);
            screen.pointerX=width/2f;screen.pointerY=height/2f;
            status.setText((allowed?"Control":"View only")+" · encrypted · 30 fps draft");});
    }
    public void frame(byte[] jpeg) throws Exception { if(selectingDocument)return;
        BitmapFactory.Options bounds=new BitmapFactory.Options(); bounds.inJustDecodeBounds=true;
        BitmapFactory.decodeByteArray(jpeg,0,jpeg.length,bounds);
        if(bounds.outWidth!=frameWidth||bounds.outHeight!=frameHeight||bounds.outWidth<1||bounds.outHeight<1||bounds.outWidth>1600||bounds.outHeight>1000)
            throw new java.io.IOException("Invalid desktop frame dimensions");
        Bitmap decoded=BitmapFactory.decodeByteArray(jpeg,0,jpeg.length);
        if(decoded==null)throw new java.io.IOException("Invalid desktop frame");
        synchronized(frames) {
            if(stopped){decoded.recycle();return;}
            if(pending!=null)pending.recycle(); pending=decoded; received++;
            if(presentationQueued)return; presentationQueued=true;
        }
        main.post(() -> {Bitmap next; synchronized(frames){next=pending;pending=null;presentationQueued=false;}
            if(next!=null){if(stopped)next.recycle();else screen.present(next);}});
    }
    public void ended(String reason) {main.post(() -> {if(stopped)return;control=false;enableControls(false);status.setText(reason);});}
    @Override protected void onStop() {super.onStop();
        if(tools!=null && tools.isPicking() && !isFinishing()){
            selectingDocument=true;if(remote!=null)remote.release();
            synchronized(frames){if(pending!=null){pending.recycle();pending=null;}}screen.clear();return;
        }
        stopped=true;if(tools!=null)tools.close();if(remote!=null)remote.close();
        synchronized(frames){if(pending!=null){pending.recycle();pending=null;}}screen.clear();}
    @Override protected void onActivityResult(int request,int result,android.content.Intent data){
        super.onActivityResult(request,result,data);selectingDocument=false;
        if(tools!=null && (request==SessionTools.UPLOAD || request==SessionTools.DOWNLOAD_FOLDER))tools.picked(request,result,data);
    }
    @Override protected void onDestroy(){if(tools!=null)tools.close();if(remote!=null)remote.close();super.onDestroy();}

    private final class Screen extends View {
        private Bitmap bitmap;
        private final Paint paint=new Paint(Paint.FILTER_BITMAP_FLAG);
        private float zoom=1, pointerX,pointerY,lastX,lastY,downX,downY,lastScrollY;
        private boolean dragging,multiple;
        private long downAt;
        private final ScaleGestureDetector scale;
        Screen() {
            super(ViewerActivity.this);setFocusable(true);setFocusableInTouchMode(true);
            scale=new ScaleGestureDetector(ViewerActivity.this,new ScaleGestureDetector.SimpleOnScaleGestureListener(){
                @Override public boolean onScale(ScaleGestureDetector detector){zoom=Math.max(1,Math.min(4,zoom*detector.getScaleFactor()));invalidate();return true;}});
        }
        void present(Bitmap next){Bitmap old=bitmap;bitmap=next;if(old!=null)old.recycle();invalidate();}
        void clear(){if(bitmap!=null){bitmap.recycle();bitmap=null;}invalidate();}
        private float ratio(){return frameWidth>0&&frameHeight>0?Math.min(getWidth()/(float)frameWidth,getHeight()/(float)frameHeight)*zoom:1;}
        @Override protected void onDraw(Canvas canvas){canvas.drawColor(Color.rgb(11,17,24));if(bitmap==null)return;
            float r=ratio();canvas.drawBitmap(bitmap,null,new RectF((getWidth()-frameWidth*r)/2,(getHeight()-frameHeight*r)/2,(getWidth()+frameWidth*r)/2,(getHeight()+frameHeight*r)/2),paint);}
        private void move(float x,float y){pointerX=Math.max(0,Math.min(frameWidth-1,x));pointerY=Math.max(0,Math.min(frameHeight-1,y));
            if(remote!=null)remote.input("type","move","x",(int)pointerX,"y",(int)pointerY);}
        @Override public boolean onTouchEvent(MotionEvent event){scale.onTouchEvent(event);if(!control||remote==null)return true;
            float x=event.getX(),y=event.getY();int action=event.getActionMasked();
            if(action==MotionEvent.ACTION_DOWN){downAt=event.getEventTime();downX=lastX=x;downY=lastY=y;multiple=false;dragging=false;
                if(direct){float r=ratio();move((x-(getWidth()-frameWidth*r)/2)/r,(y-(getHeight()-frameHeight*r)/2)/r);}}
            else if(action==MotionEvent.ACTION_POINTER_DOWN){multiple=true;lastScrollY=y;if(dragging){remote.input("type","button","button",0,"down",0);dragging=false;}}
            else if(action==MotionEvent.ACTION_MOVE){
                if(event.getPointerCount()>1){float delta=y-lastScrollY;if(Math.abs(delta)>12&&!scale.isInProgress()){remote.input("type","wheel","delta",delta>0?120:-120);lastScrollY=y;}}
                else if(!multiple){if(!dragging&&event.getEventTime()-downAt>450&&Math.hypot(x-downX,y-downY)<20){remote.input("type","button","button",0,"down",1);dragging=true;}
                    float r=ratio();if(direct)move((x-(getWidth()-frameWidth*r)/2)/r,(y-(getHeight()-frameHeight*r)/2)/r);else move(pointerX+(x-lastX)/r,pointerY+(y-lastY)/r);}
            } else if(action==MotionEvent.ACTION_UP){if(dragging)remote.input("type","button","button",0,"down",0);
                else if(!multiple&&Math.hypot(x-downX,y-downY)<20)remote.click(0);dragging=false;}
            else if(action==MotionEvent.ACTION_CANCEL){remote.release();dragging=false;}
            lastX=x;lastY=y;return true;
        }
        @Override public boolean onCheckIsTextEditor(){return control;}
        @Override public InputConnection onCreateInputConnection(EditorInfo info){info.inputType=InputType.TYPE_CLASS_TEXT|InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;info.imeOptions=EditorInfo.IME_FLAG_NO_EXTRACT_UI;
            return new BaseInputConnection(this,false){
                @Override public boolean commitText(CharSequence value,int position){if(!control||remote==null)return false;
                    String text=value.toString();if(!textInput){status.setText("Update the Windows host for keyboard text input");return false;}
                    if(text.length()>0&&text.length()<=1024&&text.indexOf('\0')<0)remote.input("type","text","text",text);return true;}
                @Override public boolean deleteSurroundingText(int before,int after){if(before==1&&after==0&&remote!=null){remote.stroke(8);return true;}return false;}
                @Override public boolean sendKeyEvent(KeyEvent event){return Screen.this.dispatchKeyEvent(event);}
            };}
        @Override public boolean onKeyDown(int code,KeyEvent event){int vk=windowsKey(code);if(control&&remote!=null&&vk!=0){if(event.getRepeatCount()==0)remote.key(vk,true);return true;}return super.onKeyDown(code,event);}
        @Override public boolean onKeyUp(int code,KeyEvent event){int vk=windowsKey(code);if(control&&remote!=null&&vk!=0){remote.key(vk,false);return true;}return super.onKeyUp(code,event);}
    }
    private static int windowsKey(int code){if(code>=29&&code<=54)return code+36;if(code>=7&&code<=16)return code+41;if(code>=131&&code<=142)return code-19;
        switch(code){case 66:return 13;case 67:return 8;case 61:return 9;case 111:return 27;case 112:return 46;case 62:return 32;
            case 19:return 38;case 20:return 40;case 21:return 37;case 22:return 39;case 113:case 114:return 17;
            case 57:case 58:return 18;case 59:case 60:return 16;case 117:return 91;case 118:return 92;
            case 122:return 36;case 123:return 35;case 92:return 33;case 93:return 34;case 124:return 45;default:return 0;}}
}
