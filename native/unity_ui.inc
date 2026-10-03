// In-game interface: the mod's Canvas nested in Granny's gameplay overlay canvas.
// It does not depend on the player camera or scene materials.
// Default layout keeps clear of Granny's controls (CanvasScaler 800x600, match 0.5):
//   joystick bottom-left, crouch upper-left, pick-up right-middle (~0.69 H),
//   shotgun/place buttons bottom-right (<0.24 H), drop bottom-centre, hide buttons top-right.
// HUD buttons are textured circles that can be moved, resized and faded in the layout editor.
#include "ui_icons.h"

struct TouchData {int finger;V2 position,raw,delta;float dt;int taps,phase,type;float pressure,maxPressure,radius,variance,altitude,azimuth;};
static_assert(sizeof(TouchData)==68,"Unity 6000 Touch layout");
struct UIRect {float x0,y0,x1,y1;bool contains(V2 p)const{return p.x>=x0&&p.x<=x1&&p.y>=y0&&p.y<=y1;}};
struct UILabel {Obj text=nullptr;std::string value;Color color{1,1,1,1};};
struct UIWidget {
    Obj root=nullptr,image=nullptr,icon=nullptr;UILabel label;UIRect hit{};int action=-1,value=0,layer=0;
    Color base{},shown{-1,-1,-1,-1},iconShown{-1,-1,-1,-1};bool visible=true;float pressed=-10;
    // Movable HUD buttons: centre in screen fractions, diameter as a fraction of screen height.
    const char *key=nullptr;float nx=0,ny=0,size=0,defaultX=0,defaultY=0,defaultSize=0;
};
enum {LayerHud=0,LayerWheel=1,LayerCategories=2,LayerItems=3,LayerEdit=4};
enum {ActFire=0,ActReload,ActAim,ActJump,ActNext,ActShop,ActCategory,ActItem,ActBack,ActBuy,ActBots,ActFps,ActClose,
      ActEdit,ActSmaller,ActBigger,ActFainter,ActStronger,ActResetLayout,ActEditDone};
static constexpr int GearVest=100,GearHelmet=101;
struct BuyCategory {const char *title;std::vector<int> items;};
// CS:GO buy wheel order: 1 Pistols, 2 Heavy, 3 SMGs, 4 Rifles, 5 Gear, 6 Grenades.
static const BuyCategory buyMenu[6]={
    {"ПИСТОЛЕТЫ",{0,1,2,3,4,5,6,7,8,9}},
    {"ТЯЖЁЛОЕ",{28,29,30,31,32,33}},
    {"ПП",{10,11,12,13,14,15,16}},
    {"ВИНТОВКИ",{21,20,17,18,19,24,23,22,25,26,27}},
    {"СНАРЯЖЕНИЕ",{GearVest,GearHelmet,35,34}},
    {"ГРАНАТЫ",{39,40,41,37,36,38}},
};
static const Color panelColor{1,1,1,.95f},tileColor{.75f,.82f,.9f,.9f},ownedColor{.45f,1,.6f,.95f},selectColor{1,.72f,.25f,1};
static const Color gold{.98f,.80f,.30f,1},white{.93f,.95f,.96f,1},muted{.62f,.67f,.70f,1},red{1,.38f,.32f,1},green{.45f,.95f,.55f,1};
static std::string filesDir;

struct NativeUI {
    Klass *canvas=nullptr,*rect=nullptr,*text=nullptr,*image=nullptr,*rawImage=nullptr,*shadow=nullptr,*raycaster=nullptr,*input=nullptr,*resources=nullptr,*fontClass=nullptr,*systemType=nullptr,*screen=nullptr,*texture2D=nullptr,*byteClass=nullptr;
    const Method *ctor=nullptr,*anchorMin=nullptr,*anchorMax=nullptr,*sizeDelta=nullptr,*anchorPos=nullptr,*pivotSet=nullptr;
    const Method *textValue=nullptr,*textFont=nullptr,*fontSize=nullptr,*alignment=nullptr,*fontStyle=nullptr,*color=nullptr,*rayTarget=nullptr,*overflowH=nullptr,*overflowV=nullptr;
    const Method *allFonts=nullptr,*builtinFont=nullptr,*renderMode=nullptr,*sortingOrder=nullptr,*touchCount=nullptr,*getTouch=nullptr,*width=nullptr,*height=nullptr;
    const Method *effectColor=nullptr,*effectDistance=nullptr,*overrideSorting=nullptr,*scaleFactor=nullptr,*isRoot=nullptr;
    const Method *texCtor=nullptr,*loadRaw=nullptr,*applyTex=nullptr,*filterMode=nullptr,*rawTexture=nullptr;
    Obj canvasGO=nullptr,root=nullptr,hud=nullptr,wheel=nullptr,editPanel=nullptr,raycast=nullptr,font=nullptr;
    Obj hurtImage=nullptr,flashImage=nullptr,scopeRoot=nullptr,crossRoot=nullptr,cross[5]{};
    Obj texButton=nullptr,texAccent=nullptr,texPanel=nullptr,texIcons[icons::Count]{};
    UILabel hp,money,weaponName,ammo,notice,status,wheelTitle,centerTitle,centerPrice,centerInfo,centerHint,statNames[4],editTitle;
    Obj statFill[4]{};float statValue[4]{-1,-1,-1,-1};
    std::vector<UIWidget> widgets;
    std::unordered_map<int,int> fingerWidget;
    struct Drag {int widget;V2 offset;};std::unordered_map<int,Drag> drags;
    float unit=1; // canvas units per screen pixel (1 / host canvas scaleFactor)
    float hudAlpha=.85f;
    bool bound=false,failed=false,shopOpen=false,editMode=false;int category=-1,selected=-1,editSelected=-1,lastFrame=-1,screenW=0,screenH=0,itemStart=0;
    float retry=0,crossGap=-1;Color hurtShown{0,0,0,-1},flashShown{0,0,0,-1},crossShown{0,0,0,-1};bool canvasVisible=true,wheelVisible=false,scopeVisible=false,crossVisible=true,editVisible=false;
    bool bind(){
        if(bound)return true;
        canvas=R.klass("UnityEngine","Canvas");rect=R.klass("UnityEngine","RectTransform");
        text=R.klass("UnityEngine.UI","Text");image=R.klass("UnityEngine.UI","Image");rawImage=R.klass("UnityEngine.UI","RawImage");shadow=R.klass("UnityEngine.UI","Shadow");
        raycaster=R.klass("UnityEngine.UI","GraphicRaycaster");texture2D=R.klass("UnityEngine","Texture2D");byteClass=R.klass("System","Byte");
        input=R.klass("UnityEngine","Input");resources=R.klass("UnityEngine","Resources");
        fontClass=R.klass("UnityEngine","Font");systemType=R.klass("System","Type");screen=R.klass("UnityEngine","Screen");
        if(!canvas||!rect||!text||!image||!input||!fontClass||!systemType||!screen){log("Unity UI class is unavailable");return false;}
        ctor=R.method(U.gameObject,".ctor",{"System.String","System.Type[]"});
        anchorMin=R.method(rect,"set_anchorMin",{"UnityEngine.Vector2"});anchorMax=R.method(rect,"set_anchorMax",{"UnityEngine.Vector2"});
        sizeDelta=R.method(rect,"set_sizeDelta",{"UnityEngine.Vector2"});anchorPos=R.method(rect,"set_anchoredPosition",{"UnityEngine.Vector2"});
        pivotSet=R.method(rect,"set_pivot",{"UnityEngine.Vector2"});
        textValue=R.method(text,"set_text",{"System.String"});textFont=R.method(text,"set_font",{"UnityEngine.Font"});
        fontSize=R.method(text,"set_fontSize",{"System.Int32"});alignment=R.method(text,"set_alignment",{"UnityEngine.TextAnchor"});
        fontStyle=R.method(text,"set_fontStyle",{"UnityEngine.FontStyle"});
        overflowH=R.method(text,"set_horizontalOverflow",{"UnityEngine.HorizontalWrapMode"});overflowV=R.method(text,"set_verticalOverflow",{"UnityEngine.VerticalWrapMode"});
        color=R.method(image,"set_color",{"UnityEngine.Color"});rayTarget=R.method(image,"set_raycastTarget",{"System.Boolean"});
        allFonts=R.method(resources,"FindObjectsOfTypeAll",{"System.Type"});builtinFont=R.method(resources,"GetBuiltinResource",{"System.Type","System.String"});
        renderMode=R.method(canvas,"get_renderMode",{});sortingOrder=R.method(canvas,"set_sortingOrder",{"System.Int32"});overrideSorting=R.method(canvas,"set_overrideSorting",{"System.Boolean"});scaleFactor=R.method(canvas,"get_scaleFactor",{});isRoot=R.method(canvas,"get_isRootCanvas",{});
        touchCount=R.method(input,"get_touchCount",{});getTouch=R.method(input,"GetTouch",{"System.Int32"});
        width=R.method(screen,"get_width",{});height=R.method(screen,"get_height",{});
        if(shadow){effectColor=R.method(shadow,"set_effectColor",{"UnityEngine.Color"});effectDistance=R.method(shadow,"set_effectDistance",{"UnityEngine.Vector2"});}
        // Sprite.Create is stripped; textured UI uses RawImage with runtime Texture2D objects.
        if(texture2D&&rawImage&&byteClass){
            texCtor=R.method(texture2D,".ctor",{"System.Int32","System.Int32","UnityEngine.TextureFormat","System.Boolean"});
            loadRaw=R.method(texture2D,"LoadRawTextureData",{"System.Byte[]"});applyTex=R.method(texture2D,"Apply",{});
            filterMode=R.method(texture2D,"set_filterMode",{"UnityEngine.FilterMode"});rawTexture=R.method(rawImage,"set_texture",{"UnityEngine.Texture"});
        }
        const Method *required[]={ctor,anchorMin,anchorMax,sizeDelta,anchorPos,pivotSet,textValue,textFont,fontSize,alignment,color,rayTarget,touchCount,getTouch,width,height};
        for(auto method:required)if(!method){log("Required Unity UI method is unavailable");return false;}
        bound=true;return true;
    }
    // ---- textures
    // Rows are uploaded bottom-up because Unity's texture row 0 is the bottom of the image.
    Obj makeTexture(const std::vector<uint8_t> &rgba,int w,int h){
        if(!texCtor||!loadRaw||!applyTex)return nullptr;
        Obj tex=R.pin(R.object_new(texture2D));int format=4;bool mipmaps=false;  // TextureFormat.RGBA32
        R.call(texCtor,tex,{&w,&h,&format,&mipmaps});
        Array *bytes=(Array*)R.array_new(byteClass,rgba.size());if(!bytes)return nullptr;
        for(int y=0;y<h;y++)memcpy(bytes->data+size_t(y)*w*4,rgba.data()+size_t(h-1-y)*w*4,size_t(w)*4);
        R.call(loadRaw,tex,{bytes});int bilinear=1;if(filterMode)R.call(filterMode,tex,{&bilinear});R.call(applyTex,tex);
        return tex;
    }
    void makeTextures(){
        texButton=makeTexture(icons::renderButton(128,false),128,128);
        texAccent=makeTexture(icons::renderButton(128,true),128,128);
        texPanel=makeTexture(icons::renderPanel(256,96,22),256,96);
        for(int i=0;i<icons::Count;i++)texIcons[i]=makeTexture(icons::renderIcon(i,128),128,128);
        log(std::string("HUD textures ")+(texButton&&texPanel?"created":"unavailable; using flat panels"));
    }
    // ---- construction helpers
    Obj node(Obj parent,const char *name){
        Obj go=R.pin(R.object_new(U.gameObject));Array *types=(Array*)R.pin(R.array_new(systemType,1));
        if(!go||!types)return nullptr;
        Obj type=R.type(rect);R.gc_write(types,(void**)types->data,type);
        R.call(ctor,go,{R.string_new(name),types});
        if(parent){Obj t=U.trans(go);U.parent(t,parent);}
        return go;
    }
    void place(Obj go,UIRect anchors){
        Obj t=U.trans(go);V2 min{anchors.x0,anchors.y0},max{anchors.x1,anchors.y1},zero{},center{.5f,.5f};
        R.call(anchorMin,t,{&min});R.call(anchorMax,t,{&max});R.call(sizeDelta,t,{&zero});R.call(anchorPos,t,{&zero});R.call(pivotSet,t,{&center});
    }
    // Pixel placement relative to the screen centre.
    void placePx(Obj go,float cx,float cy,float w,float h){
        Obj t=U.trans(go);V2 c{.5f,.5f},size{w*unit,h*unit},position{cx*unit,cy*unit};
        R.call(anchorMin,t,{&c});R.call(anchorMax,t,{&c});R.call(pivotSet,t,{&c});R.call(sizeDelta,t,{&size});R.call(anchorPos,t,{&position});
    }
    // A square of `side` screen pixels centred at fractional screen position (nx, ny).
    void placeAt(Obj go,float nx,float ny,float side){
        Obj t=U.trans(go);V2 a{nx,ny},size{side*unit,side*unit},zero{},c{.5f,.5f};
        R.call(anchorMin,t,{&a});R.call(anchorMax,t,{&a});R.call(pivotSet,t,{&c});R.call(sizeDelta,t,{&size});R.call(anchorPos,t,{&zero});
    }
    void move(Obj go,float cx,float cy){Obj t=U.trans(go);V2 position{cx*unit,cy*unit};R.call(anchorPos,t,{&position});}
    UIRect pxRect(float cx,float cy,float w,float h){return {screenW*.5f+cx-w*.5f,screenH*.5f+cy-h*.5f,screenW*.5f+cx+w*.5f,screenH*.5f+cy+h*.5f};}
    UIRect anchorRect(UIRect a){return {a.x0*screenW,a.y0*screenH,a.x1*screenW,a.y1*screenH};}
    Obj paint(Obj go,Color tint,bool blocks){
        Obj graphic=U.add(go,image);R.call(color,graphic,{&tint});R.call(rayTarget,graphic,{&blocks});return graphic;
    }
    // Textured graphic; falls back to a flat Image when runtime textures are unavailable.
    Obj paintTex(Obj go,Obj tex,Color tint,bool blocks){
        if(!tex||!rawTexture)return paint(go,{tint.r*.12f,tint.g*.14f,tint.b*.16f,tint.a*.85f},blocks);
        Obj graphic=U.add(go,rawImage);R.call(rawTexture,graphic,{tex});R.call(color,graphic,{&tint});R.call(rayTarget,graphic,{&blocks});return graphic;
    }
    void setColor(Obj graphic,Color c){if(graphic)R.call(color,graphic,{&c});}
    UILabel label(Obj parent,const char *name,UIRect anchors,const std::string &value,int align,float size,Color tint,bool bold=false){
        Obj go=node(parent,name);place(go,anchors);
        UILabel result;result.text=U.add(go,text);result.color=tint;result.value=value;
        R.call(textFont,result.text,{font});R.call(textValue,result.text,{R.string_new(value.c_str())});
        int pixels=std::max(1,(int)std::lround(std::max(12.f,screenH*size)*unit));R.call(fontSize,result.text,{&pixels});R.call(alignment,result.text,{&align});
        if(bold&&fontStyle){int style=1;R.call(fontStyle,result.text,{&style});}
        bool no=false;R.call(rayTarget,result.text,{&no});int over=1;if(overflowH)R.call(overflowH,result.text,{&over});if(overflowV)R.call(overflowV,result.text,{&over});
        R.call(color,result.text,{&tint});
        if(shadow&&effectColor){Obj s=U.add(go,shadow);Color dark{0,0,0,.85f};V2 offset{std::max(1.f,screenH*.002f)*unit,-std::max(1.f,screenH*.002f)*unit};R.call(effectColor,s,{&dark});if(effectDistance)R.call(effectDistance,s,{&offset});}
        return result;
    }
    void setText(UILabel &l,const std::string &value){if(l.text&&l.value!=value){l.value=value;R.call(textValue,l.text,{R.string_new(value.c_str())});}}
    void setTextColor(UILabel &l,Color c){if(l.text&&memcmp(&l.color,&c,sizeof(c))){l.color=c;R.call(color,l.text,{&c});}}
    // Rectangular text button on a rounded panel (buy wheel, editor).
    int button(Obj parent,int layer,const std::string &value,int action,int argument,float size,Color tint,bool px,UIRect anchors,float cx=0,float cy=0,float w=0,float h=0){
        Obj go=node(parent,"TacticalControl");
        UIWidget widget;widget.root=go;widget.layer=layer;widget.action=action;widget.value=argument;widget.base=tint;
        if(px){placePx(go,cx,cy,w,h);widget.hit=pxRect(cx,cy,w,h);}else{place(go,anchors);widget.hit=anchorRect(anchors);}
        widget.image=paintTex(go,texPanel,tint,true);
        widget.label=label(U.trans(go),"Label",{0,0,1,1},value,4,size,white,true);
        widgets.push_back(widget);return (int)widgets.size()-1;
    }
    // Round HUD button with an icon; position and size come from the saved layout.
    int roundButton(Obj parent,const char *key,int icon,const char *caption,int action,bool accent,float nx,float ny,float size){
        Obj go=node(parent,key);
        UIWidget widget;widget.root=go;widget.layer=LayerHud;widget.action=action;widget.base=accent?Color{1,.75f,.6f,1}:Color{1,1,1,1};
        widget.key=key;widget.nx=widget.defaultX=nx;widget.ny=widget.defaultY=ny;widget.size=widget.defaultSize=size;
        widget.image=paintTex(go,accent?texAccent:texButton,widget.base,true);
        if(icon>=0&&texIcons[icon]){Obj ic=node(U.trans(go),"Icon");place(ic,{.2f,.2f,.8f,.8f});widget.icon=paintTex(ic,texIcons[icon],{1,1,1,1},false);}
        else widget.label=label(U.trans(go),"Label",{0,0,1,1},caption,4,size*.3f,white,true);
        widgets.push_back(widget);applyPlacement(widgets.back());return (int)widgets.size()-1;
    }
    void applyPlacement(UIWidget &w){
        float side=w.size*screenH;placeAt(w.root,w.nx,w.ny,side);
        float cx=w.nx*screenW,cy=w.ny*screenH;w.hit={cx-side*.5f,cy-side*.5f,cx+side*.5f,cy+side*.5f};
    }
    // ---- layout persistence (filesDir/hud_layout.txt: "key x y size" lines and "alpha a")
    std::string layoutPath(){return filesDir.empty()?std::string():filesDir+"/hud_layout.txt";}
    void loadLayout(){
        std::string path=layoutPath();if(path.empty())return;
        FILE *f=fopen(path.c_str(),"r");if(!f)return;
        char key[64];float x,y,size;int loaded=0;
        while(fscanf(f,"%63s",key)==1){
            if(!strcmp(key,"alpha")){if(fscanf(f,"%f",&x)==1)hudAlpha=clamp(x,.25f,1);continue;}
            if(fscanf(f,"%f %f %f",&x,&y,&size)!=3)break;
            for(auto &w:widgets)if(w.key&&!strcmp(w.key,key)){w.nx=clamp(x,.02f,.98f);w.ny=clamp(y,.02f,.98f);w.size=clamp(size,.05f,.4f);applyPlacement(w);++loaded;}
        }
        fclose(f);log("HUD layout loaded: "+std::to_string(loaded)+" buttons");
    }
    void saveLayout(){
        std::string path=layoutPath();if(path.empty())return;
        FILE *f=fopen(path.c_str(),"w");if(!f){log("HUD layout could not be saved");return;}
        fprintf(f,"alpha %.3f\n",hudAlpha);
        for(auto &w:widgets)if(w.key)fprintf(f,"%s %.4f %.4f %.4f\n",w.key,w.nx,w.ny,w.size);
        fclose(f);log("HUD layout saved");
    }
    void resetLayout(){for(auto &w:widgets)if(w.key){w.nx=w.defaultX;w.ny=w.defaultY;w.size=w.defaultSize;applyPlacement(w);}hudAlpha=.85f;}
    // ---- lifecycle
    bool alive(){return canvasGO&&U.alive(canvasGO);}
    void ensure(){
        if(failed)return;
        if(!bound){if(now<retry)return;retry=now+1;if(!bind()){failed=true;return;}}
        int w=R.value<int>(R.call(width)),h=R.value<int>(R.call(height));
        if(alive()&&w==screenW&&h==screenH)return;
        if(now<retry)return;retry=now+1;
        if(alive())U.remove(canvasGO);
        U.remove(texButton);U.remove(texAccent);U.remove(texPanel);for(auto t:texIcons)U.remove(t);
        reset();
        PinScope scope(uiPins);
        if(!create(w,h)){if(alive())U.remove(canvasGO);reset();}
    }
    void reset(){
        R.release(uiPins);canvasGO=root=hud=wheel=editPanel=raycast=font=hurtImage=flashImage=scopeRoot=crossRoot=nullptr;
        texButton=texAccent=texPanel=nullptr;for(auto &t:texIcons)t=nullptr;
        for(auto &c:cross)c=nullptr;for(auto &f:statFill)f=nullptr;for(auto &v:statValue)v=-1;
        widgets.clear();fingerWidget.clear();drags.clear();shopOpen=editMode=false;category=-1;selected=-1;editSelected=-1;lastFrame=-1;crossGap=-1;
        hurtShown=flashShown=crossShown=Color{0,0,0,-1};canvasVisible=crossVisible=true;wheelVisible=scopeVisible=editVisible=false;
    }
    Obj findFont(){
        Obj result=nullptr;
        // LegacyRuntime.ttf is a dynamic font with Cyrillic glyphs; the game's fonts may be bitmap-only.
        if(builtinFont)result=R.call(builtinFont,nullptr,{R.type(fontClass),R.string_new("LegacyRuntime.ttf")});
        if(U.alive(result))return result;
        Array *available=allFonts?(Array*)R.call(allFonts,nullptr,{R.type(fontClass)}):nullptr;
        if(available&&available->length<1000){auto objects=(Obj*)available->data;for(size_t i=0;i<available->length;i++)if(U.alive(objects[i]))return objects[i];}
        return nullptr;
    }
    bool create(int w,int h){
        TraceScope trace("create mod canvas");
        if(w<=0||h<=0)return false;
        screenW=w;screenH=h;unit=1;
        font=R.pin(findFont());if(!font){log("UI waits for a font");return false;}
        canvasGO=node(nullptr,"GrannyTacticalCanvas");if(!canvasGO)return false;
        Obj canvasComponent=U.add(canvasGO,canvas);
        if(!canvasComponent){log("Canvas component could not be added");return false;}
        // Above Granny's buttons (orders 0-5), below her pause menu (8), black screens (10) and ads (50).
        int order=6;if(sortingOrder)R.call(sortingOrder,canvasComponent,{&order});
        int mode=renderMode?R.value<int>(R.call(renderMode,canvasComponent)):-1;
        // Canvas.set_renderMode is stripped and a new Canvas starts in world space (mode 2 on device),
        // so the mod canvas is nested under one of Granny's gameplay overlay canvases.
        if(mode!=0&&!attachToGameCanvas(canvasComponent))return false;
        if(raycaster){raycast=U.add(canvasGO,raycaster);U.enable(raycast,false);}
        root=U.trans(canvasGO);
        makeTextures();
        scopeRoot=node(root,"ScopeOverlay");place(scopeRoot,{0,0,1,1});
        {
            float half=screenH*.5f/screenW;Color black{0,0,0,.94f};
            Obj left=node(U.trans(scopeRoot),"ScopeLeft");place(left,{0,0,.5f-half,1});paint(left,black,false);
            Obj right=node(U.trans(scopeRoot),"ScopeRight");place(right,{.5f+half,0,1,1});paint(right,black,false);
            Obj lineH=node(U.trans(scopeRoot),"ScopeLineH");placePx(lineH,0,0,screenH,std::max(1.f,screenH*.002f));paint(lineH,{0,0,0,1},false);
            Obj lineV=node(U.trans(scopeRoot),"ScopeLineV");placePx(lineV,0,0,std::max(1.f,screenH*.002f),screenH);paint(lineV,{0,0,0,1},false);
        }
        Obj hurtGO=node(root,"DamageFlash");place(hurtGO,{0,0,1,1});hurtImage=paint(hurtGO,{.8f,0,0,0},false);
        hud=node(root,"Hud");place(hud,{0,0,1,1});Obj hudT=U.trans(hud);
        crossRoot=node(hudT,"Crosshair");placePx(crossRoot,0,0,1,1);
        for(int i=0;i<5;i++){cross[i]=paint(node(U.trans(crossRoot),"CrossBar"),{.35f,1,.45f,.95f},false);}
        // Readouts on rounded panels.
        Obj hpPanel=node(hudT,"HealthPanel");place(hpPanel,{.008f,.895f,.205f,.993f});paintTex(hpPanel,texPanel,{1,1,1,.8f},false);
        hp=label(hudT,"Health",{.02f,.9f,.2f,.99f},"",3,.048f,white,true);
        Obj moneyPanel=node(hudT,"MoneyPanel");place(moneyPanel,{.39f,.895f,.53f,.993f});paintTex(moneyPanel,texPanel,{1,1,1,.8f},false);
        money=label(hudT,"Money",{.4f,.9f,.525f,.99f},"",4,.046f,gold,true);
        weaponName=label(hudT,"Weapon",{.535f,.9f,.83f,.99f},"",3,.036f,white,true);
        Obj ammoPanel=node(hudT,"AmmoPanel");place(ammoPanel,{.82f,.247f,.995f,.327f});paintTex(ammoPanel,texPanel,{1,1,1,.8f},false);
        ammo=label(hudT,"Ammo",{.83f,.25f,.987f,.324f},"",5,.058f,white,true);
        notice=label(hudT,"Notice",{.2f,.835f,.8f,.895f},"",4,.036f,white,true);
        status=label(hudT,"Status",{.2f,.79f,.8f,.835f},"",4,.026f,muted);
        roundButton(hudT,"fire",icons::Fire,"FIRE",ActFire,true,.915f,.43f,.23f);
        roundButton(hudT,"aim",icons::Aim,"AIM",ActAim,false,.785f,.53f,.125f);
        roundButton(hudT,"reload",icons::Reload,"R",ActReload,false,.785f,.37f,.125f);
        roundButton(hudT,"jump",icons::Jump,"JUMP",ActJump,false,.672f,.37f,.125f);
        roundButton(hudT,"next",icons::Swap,"NEXT",ActNext,false,.672f,.53f,.125f);
        roundButton(hudT,"lfire",icons::Fire,"FIRE",ActFire,true,.055f,.60f,.13f);
        roundButton(root,"buy",icons::Cart,"BUY",ActShop,false,.335f,.945f,.095f);
        int gear=roundButton(root,"edit",icons::Gear,"",ActEdit,false,.965f,.945f,.085f);widgets[gear].key=nullptr;
        createWheel();
        createEditor();
        Obj flashGO=node(root,"FlashBang");place(flashGO,{0,0,1,1});flashImage=paint(flashGO,{1,1,1,0},false);
        U.active(wheel,false);U.active(scopeRoot,false);U.active(editPanel,false);wheelVisible=scopeVisible=editVisible=false;
        loadLayout();
        std::ostringstream s;s<<"Mod canvas created: "<<w<<"x"<<h<<", renderMode="<<mode<<", sortingOrder=6, font="<<U.name(font)<<", controls="<<widgets.size();log(s.str());
        notify("Шестерёнка справа вверху — настройка кнопок",5);
        return true;
    }
    // A new Canvas is expected to be ScreenSpaceOverlay. If the engine ever creates it in world
    // space, nest it under one of Granny's overlay canvases with its own sorting instead.
    bool attachToGameCanvas(Obj canvasComponent){
        // Prefer Granny's always-active gameplay canvases from level2; never the ad SDK canvases.
        const char *preferred[]={"AllaTexter","DifferentSpritesAndButtons","All days texter","TheEndTexts"};
        Array *list=U.objects(canvas);Obj host=nullptr;int rank=99;
        if(list&&list->length<1000){
            auto items=(Obj*)list->data;
            for(size_t i=0;i<list->length;i++){
                Obj c=items[i];if(c==canvasComponent||!U.alive(c))continue;
                if(R.value<int>(R.call(renderMode,c))!=0)continue;
                if(isRoot&&!R.value<bool>(R.call(isRoot,c)))continue;
                std::string path=U.path(U.trans(c,true));
                if(path.find("Applovin")!=std::string::npos||path.find("Ads")!=std::string::npos||path.find("Black")!=std::string::npos)continue;
                std::string name=U.name(c);int r=50;
                for(int k=0;k<4;k++)if(name==preferred[k])r=k;
                if(r<rank){rank=r;host=c;}
            }
        }
        if(!host){log("Mod canvas waits for a gameplay overlay canvas");return false;}
        U.parent(U.trans(canvasGO),U.trans(host,true));place(canvasGO,{0,0,1,1});
        bool yes=true;if(overrideSorting)R.call(overrideSorting,canvasComponent,{&yes});
        int order=6;if(sortingOrder)R.call(sortingOrder,canvasComponent,{&order});
        float scale=scaleFactor?R.value<float>(R.call(scaleFactor,host)):1;
        unit=scale>.05f?1/scale:1;
        std::ostringstream s;s<<"Mod canvas nested under "<<U.path(U.trans(host,true))<<", scaleFactor="<<scale;log(s.str());
        return true;
    }
    void createWheel(){
        wheel=node(root,"BuyWheel");place(wheel,{0,0,1,1});Obj t=U.trans(wheel);
        Obj dim=node(t,"Dim");place(dim,{0,0,1,1});paint(dim,{0,0,0,.6f},true);
        float H=screenH;
        wheelTitle=label(t,"WheelTitle",{.25f,.9f,.75f,.985f},"",4,.042f,white,true);
        Obj center=node(t,"Center");placePx(center,0,0,.52f*H,.38f*H);paintTex(center,texPanel,panelColor,true);
        Obj c=U.trans(center);
        centerTitle=label(c,"Title",{.05f,.80f,.95f,.98f},"",4,.042f,white,true);
        centerPrice=label(c,"Price",{.05f,.66f,.95f,.80f},"",4,.032f,gold,true);
        const char *names[4]={"Урон","Скорострельность","Точность","Мобильность"};
        for(int i=0;i<4;i++){
            float y=.56f-i*.105f;
            statNames[i]=label(c,"StatName",{.05f,y,.45f,y+.09f},names[i],3,.024f,muted);
            Obj bar=node(c,"StatBar");place(bar,{.47f,y+.025f,.95f,y+.065f});paint(bar,{.2f,.23f,.26f,.9f},false);
            Obj fill=node(U.trans(bar),"StatFill");place(fill,{0,0,.5f,1});statFill[i]=paint(fill,gold,false);
        }
        centerInfo=label(c,"Info",{.05f,.14f,.95f,.24f},"",4,.025f,muted);
        centerHint=label(c,"Hint",{.05f,.02f,.95f,.13f},"",4,.024f,muted);
        // Category ring: hexagon around the centre panel.
        const float angles[6]={150,90,30,-30,-90,-150};
        float rx=std::min(.52f*H,screenW*.5f-.17f*H),ry=.30f*H;
        for(int i=0;i<6;i++){
            float a=angles[i]*3.14159265f/180;
            std::string caption=std::to_string(i+1)+"  "+buyMenu[i].title;
            button(t,LayerCategories,caption,ActCategory,i,.032f,tileColor,true,{},rx*std::cos(a),ry*std::sin(a),.30f*H,.12f*H);
        }
        itemStart=(int)widgets.size();
        for(int i=0;i<12;i++)button(t,LayerItems,"",ActItem,-1,.026f,tileColor,true,{},0,0,.25f*H,.105f*H);
        button(t,LayerItems,"< НАЗАД",ActBack,0,.028f,tileColor,true,{},-.13f*H,-.235f*H,.24f*H,.075f*H);
        button(t,LayerItems,"КУПИТЬ",ActBuy,0,.028f,{.5f,1,.55f,.95f},true,{},.13f*H,-.235f*H,.24f*H,.075f*H);
        button(t,LayerWheel,"X",ActClose,0,.045f,{1,.45f,.4f,.95f},false,{.90f,.88f,.985f,.985f});
        button(t,LayerWheel,"",ActBots,0,.026f,tileColor,false,{.015f,.02f,.17f,.10f});
        button(t,LayerWheel,"",ActFps,0,.026f,tileColor,false,{.83f,.02f,.985f,.10f});
    }
    void createEditor(){
        editPanel=node(root,"LayoutEditor");place(editPanel,{0,0,1,1});Obj t=U.trans(editPanel);
        Obj back=node(t,"EditorPanel");place(back,{.29f,.62f,.71f,.89f});paintTex(back,texPanel,{1,1,1,.92f},false);
        editTitle=label(t,"EditorTitle",{.3f,.81f,.7f,.885f},"НАСТРОЙКА КНОПОК",4,.034f,gold,true);
        label(t,"EditorHint",{.3f,.765f,.7f,.815f},"Перетаскивай кнопки пальцем, выбранную меняй ниже",4,.022f,muted);
        button(t,LayerEdit,"РАЗМЕР -",ActSmaller,0,.024f,tileColor,false,{.30f,.69f,.395f,.755f});
        button(t,LayerEdit,"РАЗМЕР +",ActBigger,0,.024f,tileColor,false,{.405f,.69f,.5f,.755f});
        button(t,LayerEdit,"ПРОЗР. -",ActFainter,0,.024f,tileColor,false,{.51f,.69f,.595f,.755f});
        button(t,LayerEdit,"ПРОЗР. +",ActStronger,0,.024f,tileColor,false,{.605f,.69f,.7f,.755f});
        button(t,LayerEdit,"СБРОС",ActResetLayout,0,.026f,{1,.6f,.5f,.95f},false,{.30f,.628f,.495f,.68f});
        button(t,LayerEdit,"ГОТОВО",ActEditDone,0,.026f,{.5f,1,.55f,.95f},false,{.505f,.628f,.7f,.68f});
    }
    // ---- buy wheel logic
    static std::string itemName(int id){return id==GearVest?"Kevlar Vest":id==GearHelmet?"Kevlar + Helmet":weapons[id].name;}
    static int itemPrice(int id){return id==GearVest?650:id==GearHelmet?(combat.armor>=100?350:1000):weapons[id].price;}
    static bool itemOwned(int id){
        if(id==GearVest)return combat.armor>=100;
        if(id==GearHelmet)return combat.armor>=100&&combat.helmet;
        return combat.owned[id]&&(weapons[id].kind<8||combat.ammo[id]>0);
    }
    void openCategory(int index){
        category=index;selected=-1;
        const auto &items=buyMenu[index].items;int n=(int)items.size();
        float H=screenH,rx=std::min(.62f*H,screenW*.5f-.14f*H),ry=.34f*H+std::max(0.f,.62f*H-rx)*.4f;
        for(int i=0;i<12;i++){
            UIWidget &w=widgets[itemStart+i];
            if(i>=n){w.value=-1;continue;}
            float a=(90-i*360.f/n)*3.14159265f/180,cx=rx*std::cos(a),cy=ry*std::sin(a);
            w.value=items[i];move(w.root,cx,cy);w.hit=pxRect(cx,cy,.25f*H,.105f*H);
        }
    }
    void resize(float factor){if(editSelected>=0){UIWidget &w=widgets[editSelected];w.size=clamp(w.size*factor,.05f,.4f);applyPlacement(w);}else notify("Сначала выбери кнопку",1.2f);}
    void activate(UIWidget &widget){
        widget.pressed=now;
        switch(widget.action){
            case ActReload:reloadInput=1;break;
            case ActAim:altInput=1;break;
            case ActJump:jumpInput=1;break;
            case ActNext:for(int i=1;i<=WEAPON_COUNT;i++){int id=(combat.weapon+i)%WEAPON_COUNT;if(combat.owned[id]&&(weapons[id].kind<8||combat.ammo[id]>0)){selectInput=id;break;}}break;
            case ActShop:if(shopOpen)closeShop();else openShop();break;
            case ActClose:closeShop();break;
            case ActCategory:openCategory(widget.value);break;
            case ActBack:category=-1;selected=-1;break;
            case ActItem:if(widget.value<0)break;if(selected==widget.value)purchase(selected);else selected=widget.value;break;
            case ActBuy:if(selected>=0)purchase(selected);break;
            case ActBots:spawnInput=botsEnabled?2:1;break;
            case ActFps:frameRate=frameRate==60?120:60;applyFrameRate(true);notify("Частота: "+std::to_string(frameRate)+" FPS",1.5f);break;
            case ActEdit:if(editMode)finishEditing();else startEditing();break;
            case ActSmaller:resize(1/1.12f);break;
            case ActBigger:resize(1.12f);break;
            case ActFainter:hudAlpha=clamp(hudAlpha-.1f,.25f,1);break;
            case ActStronger:hudAlpha=clamp(hudAlpha+.1f,.25f,1);break;
            case ActResetLayout:resetLayout();editSelected=-1;notify("Раскладка сброшена",1.5f);break;
            case ActEditDone:finishEditing();break;
        }
    }
    void purchase(int id){
        if(id==GearVest)armorInput=1;else if(id==GearHelmet)armorInput=2;else buyInput=id;
    }
    void openShop(){if(editMode)return;shopOpen=true;category=-1;selected=-1;fireInput=0;U.enable(raycast,true);}
    void closeShop(){shopOpen=false;category=-1;selected=-1;if(!editMode)U.enable(raycast,false);}
    void startEditing(){closeShop();editMode=true;editSelected=-1;fireInput=0;drags.clear();U.enable(raycast,true);notify("Перетаскивай кнопки, затем ГОТОВО",3);}
    void finishEditing(){editMode=false;editSelected=-1;drags.clear();U.enable(raycast,false);saveLayout();notify("Раскладка сохранена",1.5f);}
    bool interactive(const UIWidget &w)const{
        if(editMode)return w.layer==LayerEdit||w.action==ActEdit;
        if(w.action==ActShop||w.action==ActEdit)return w.action==ActShop||!shopOpen;
        if(!shopOpen)return w.layer==LayerHud;
        if(w.layer==LayerWheel)return true;
        if(w.layer==LayerCategories)return category<0;
        if(w.layer==LayerItems)return category>=0&&(w.action!=ActItem||w.value>=0)&&(w.action!=ActBuy||selected>=0);
        return false;
    }
    void readInput(){
        if(!alive())return;
        int frame=R.value<int>(R.call(U.frameCount));if(frame==lastFrame)return;lastFrame=frame;
        int count=std::min(10,R.value<int>(R.call(touchCount)));std::vector<int> down;bool firing=false;
        for(int i=0;i<count;i++){
            TouchData touch=R.value<TouchData>(R.call(getTouch,nullptr,{&i}));if(touch.phase>=3)continue;
            down.push_back(touch.finger);
            if(editMode){editTouch(touch);continue;}
            auto found=fingerWidget.find(touch.finger);
            if(found==fingerWidget.end()){
                int chosen=-1;
                for(size_t j=0;j<widgets.size();j++)if(interactive(widgets[j])&&widgets[j].hit.contains(touch.position)){chosen=(int)j;break;}
                fingerWidget[touch.finger]=chosen;
                if(chosen>=0)activate(widgets[chosen]);
                found=fingerWidget.find(touch.finger);
            }
            // A finger that started on FIRE keeps shooting while it drags to aim.
            if(found->second>=0&&!shopOpen&&!editMode&&widgets[found->second].action==ActFire)firing=true;
        }
        for(auto it=fingerWidget.begin();it!=fingerWidget.end();)if(std::find(down.begin(),down.end(),it->first)==down.end())it=fingerWidget.erase(it);else++it;
        for(auto it=drags.begin();it!=drags.end();)if(std::find(down.begin(),down.end(),it->first)==down.end())it=drags.erase(it);else++it;
        fireInput=firing&&!editMode;
    }
    // Layout editor: editor buttons act on tap; any HUD button can be dragged.
    void editTouch(const TouchData &touch){
        auto drag=drags.find(touch.finger);
        if(drag!=drags.end()){
            if(drag->second.widget<0)return;
            UIWidget &w=widgets[drag->second.widget];
            w.nx=clamp((touch.position.x+drag->second.offset.x)/screenW,.02f,.98f);
            w.ny=clamp((touch.position.y+drag->second.offset.y)/screenH,.02f,.98f);
            applyPlacement(w);return;
        }
        int chosen=-1;
        for(size_t j=0;j<widgets.size();j++)if(widgets[j].layer==LayerEdit&&widgets[j].hit.contains(touch.position)){chosen=(int)j;break;}
        if(chosen>=0){activate(widgets[chosen]);drags[touch.finger]={-1,{}};return;}
        for(size_t j=0;j<widgets.size();j++){
            UIWidget &w=widgets[j];
            if(!w.key||!w.hit.contains(touch.position))continue;
            editSelected=(int)j;drags[touch.finger]={(int)j,{w.nx*screenW-touch.position.x,w.ny*screenH-touch.position.y}};return;
        }
        for(size_t j=0;j<widgets.size();j++)if(widgets[j].action==ActEdit&&widgets[j].hit.contains(touch.position)){activate(widgets[j]);break;}
        drags[touch.finger]={-1,{}};
    }
    // ---- per-tick presentation
    void show(Obj go,bool &shown,bool value){if(go&&shown!=value){shown=value;U.active(go,value);}}
    void tint(Obj graphic,Color &shown,Color value){
        if(!graphic)return;
        if(std::fabs(shown.r-value.r)+std::fabs(shown.g-value.g)+std::fabs(shown.b-value.b)+std::fabs(shown.a-value.a)<.01f)return;
        shown=value;R.call(color,graphic,{&value});
    }
    void stat(int i,float value){
        value=clamp(value,.03f,1);if(std::fabs(statValue[i]-value)<.005f)return;statValue[i]=value;
        Obj t=U.trans(R.call(U.componentGO,statFill[i]));V2 max{value,1};R.call(anchorMax,t,{&max});
    }
    void updateWheel(){
        int money=combat.money;
        setText(wheelTitle,category<0?"МЕНЮ ЗАКУПКИ  ·  $"+std::to_string(money):std::to_string(category+1)+"  "+buyMenu[category].title+"  ·  $"+std::to_string(money));
        for(size_t j=0;j<widgets.size();j++){
            UIWidget &w=widgets[j];
            if(w.layer==LayerCategories||w.layer==LayerItems){
                bool on=interactive(w);
                if(w.visible!=on){w.visible=on;U.active(w.root,on);}
                if(!on)continue;
            }
            if(w.layer==LayerHud||w.layer==LayerEdit)continue;
            Color base=w.base;
            if(w.action==ActItem){
                int id=w.value;bool owned=itemOwned(id),afford=money>=itemPrice(id);
                std::string state=owned?(id>=100?"ЕСТЬ":"ВЗЯТЬ"):"$"+std::to_string(itemPrice(id));
                setText(w.label,itemName(id)+"\n"+state);
                setTextColor(w.label,owned?green:(afford?white:red));
                base=id==selected?selectColor:(owned?ownedColor:tileColor);
                if(id<100&&id==combat.weapon)base={1,.8f,.35f,1};
            }
            if(w.action==ActBots)setText(w.label,botsEnabled?"БОТЫ: ВКЛ":"БОТЫ: ВЫКЛ");
            if(w.action==ActFps)setText(w.label,std::to_string(frameRate)+" FPS");
            if(w.action==ActBuy&&selected>=0)setText(w.label,itemOwned(selected)?(selected>=100?"ЕСТЬ":"ВЗЯТЬ"):"КУПИТЬ $"+std::to_string(itemPrice(selected)));
            if(now-w.pressed<.12f)base={std::min(1.f,base.r+.25f),std::min(1.f,base.g+.25f),std::min(1.f,base.b+.25f),base.a};
            tint(w.image,w.shown,base);
        }
        if(category<0){
            setText(centerTitle,"$"+std::to_string(money));
            setText(centerPrice,std::string("Броня ")+std::to_string(combat.armor)+(combat.helmet?" + шлем":""));
            setText(centerInfo,std::string("В руках: ")+weapons[combat.weapon].name);
            setText(centerHint,"Выбери категорию 1–6");
            for(int i=0;i<4;i++)stat(i,0);
            return;
        }
        if(selected<0){
            setText(centerTitle,buyMenu[category].title);setText(centerPrice,"");setText(centerInfo,"");
            setText(centerHint,"Нажми на предмет — характеристики,\nещё раз — покупка");
            for(int i=0;i<4;i++)stat(i,0);
            return;
        }
        setText(centerTitle,itemName(selected));
        int price=itemPrice(selected);
        setText(centerPrice,"$"+std::to_string(price)+(selected<100&&weapons[selected].kind<6?"   ·   награда $300":""));
        if(selected>=100){
            setText(centerInfo,selected==GearHelmet?"Броня 100 и шлем: меньше урона в голову":"Броня 100: поглощает часть урона");
            stat(0,0);stat(1,0);stat(2,0);stat(3,1);
        }else{
            const Weapon &w=weapons[selected];
            stat(0,w.damage*w.pellets/120.f);stat(1,w.rpm/1000.f);stat(2,1-w.spread/1.2f);stat(3,(w.speed-2.5f)/2.5f);
            std::string info=w.kind>=8?"Граната":(w.kind==6?"Ближний бой":"Магазин "+std::to_string(w.magazine)+" / "+std::to_string(w.reserve));
            if(w.automatic)info+="  ·  авто";if(w.scoped)info+="  ·  прицел";if(w.burst)info+="  ·  очередь";
            setText(centerInfo,info);
        }
        setText(centerHint,itemOwned(selected)?"Уже есть — нажми ещё раз, чтобы взять":(combat.money>=price?"Нажми ещё раз или КУПИТЬ":"Недостаточно денег"));
    }
    void update(bool inGame){
        if(!alive())return;
        show(canvasGO,canvasVisible,inGame);
        if(!inGame)return;
        show(wheel,wheelVisible,shopOpen);
        show(editPanel,editVisible,editMode);
        const Weapon &w=weapons[combat.weapon];
        bool sniper=scoped&&w.kind==3;
        show(scopeRoot,scopeVisible,sniper&&!shopOpen);
        // HUD text
        setText(hp,"+ "+std::to_string(combat.health)+"   AR "+std::to_string(combat.armor)+(combat.helmet&&combat.armor>0?"+":""));
        setTextColor(hp,combat.health<=25?red:white);
        setText(money,"$"+std::to_string(combat.money));
        setText(weaponName,w.name);
        std::string ammoText=w.kind>=6?"—":(combat.reload_left>0?"ПЕРЕЗАРЯДКА":std::to_string(combat.ammo[combat.weapon])+" / "+std::to_string(combat.reserve[combat.weapon]));
        setText(ammo,ammoText);
        setTextColor(ammo,w.kind<6&&combat.ammo[combat.weapon]<=std::max(1,w.magazine/5)?red:white);
        std::string message=now<noticeUntil?(killfeed.empty()?notification:killfeed):"";
        if(now>=noticeUntil)killfeed.clear();
        setText(notice,message);
        std::string state;
        if(!worldReady)state=U.alive(pivot)?"Мод: ищу камеру игрока…":"Мод: ожидание игрока…";
        else if(!visualsReady)state="Мод: модели загружаются…";
        else if(!cameraObject)state="Мод: камера не найдена, прицел от головы";
        setText(status,state);
        for(size_t j=0;j<widgets.size();j++){
            UIWidget &widget=widgets[j];
            if(widget.layer==LayerEdit){Color base=widget.base;if(now-widget.pressed<.12f)base.a=1;tint(widget.image,widget.shown,base);continue;}
            if(widget.layer!=LayerHud)continue;
            Color base=widget.base;base.a*=hudAlpha;
            if(widget.action==ActAim&&!w.scoped&&!w.burst)base.a*=.55f;
            if(widget.action==ActFire&&fireInput.load())base={1,.45f,.3f,1};
            if(widget.action==ActShop&&shopOpen)base={1,.8f,.3f,1};
            if(widget.action==ActEdit&&editMode)base={1,.8f,.3f,1};
            if(editMode&&widget.key)base=(int)j==editSelected?Color{1,.85f,.2f,1}:Color{.75f,.9f,1,.9f};
            if(now-widget.pressed<.12f)base={std::min(1.f,base.r+.25f),std::min(1.f,base.g+.25f),std::min(1.f,base.b+.25f),base.a};
            tint(widget.image,widget.shown,base);
            tint(widget.icon,widget.iconShown,{1,1,1,std::min(1.f,hudAlpha+.1f)});
            bool visible=widget.action==ActShop||widget.action==ActEdit?!(shopOpen&&widget.action==ActEdit):!shopOpen;
            if(widget.visible!=visible){widget.visible=visible;U.active(widget.root,visible);}
        }
        if(shopOpen)updateWheel();
        // Crosshair: CS-style four bars whose gap follows movement, airtime and recoil.
        float moving=length(flat(mv.velocity));
        float gap=screenH*(.012f+.006f*clamp(moving/4,0,1)+(mv.grounded?0:.012f)+recoilPitch*.0015f);
        float len=screenH*.018f,th=std::max(2.f,screenH*.0035f);
        if(std::fabs(gap-crossGap)>.5f){
            crossGap=gap;
            const float pos[5][4]={{0,gap+len*.5f,th,len},{0,-gap-len*.5f,th,len},{gap+len*.5f,0,len,th},{-gap-len*.5f,0,len,th},{0,0,th,th}};
            for(int i=0;i<5;i++)placePx(R.call(U.componentGO,cross[i]),pos[i][0],pos[i][1],pos[i][2],pos[i][3]);
        }
        show(crossRoot,crossVisible,!shopOpen&&!sniper&&!editMode);
        Color crossColor=hitMarker>0?Color{1,.25f,.2f,1}:Color{.35f,1,.45f,.95f};
        if(memcmp(&crossColor,&crossShown,sizeof(Color))){crossShown=crossColor;for(auto c:cross)setColor(c,crossColor);}
        tint(hurtImage,hurtShown,{.75f,0,0,clamp(hurt,0,1)*.45f});
        tint(flashImage,flashShown,{1,1,1,clamp(flash,0,1)*.95f});
    }
} nativeUI;
