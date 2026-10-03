// Game controls are Unity UI objects under the game's existing screen Canvas.
struct UIRect {float x0,y0,x1,y1;bool contains(V2 p)const{return p.x>=x0&&p.x<=x1&&p.y>=y0&&p.y<=y1;}};
struct TouchData {int finger;V2 position,raw,delta;float dt;int taps,phase,type;float pressure,maxPressure,radius,variance,altitude,azimuth;};
static_assert(sizeof(TouchData)==68,"Unity 6000 Touch layout");
struct CanvasRect {float x,y,width,height;};
struct UIWidget {Obj root=nullptr,text=nullptr;UIRect rect{};int action=-1,value=0;bool shop=false;};
struct NativeUI {
    Klass *canvas=nullptr,*rect=nullptr,*text=nullptr,*image=nullptr,*button=nullptr,*input=nullptr,*resources=nullptr,*fontClass=nullptr,*systemType=nullptr,*screen=nullptr;
    const Method *ctor=nullptr,*anchorMin=nullptr,*anchorMax=nullptr,*sizeDelta=nullptr,*anchorPos=nullptr,*pivot=nullptr,*rectValue=nullptr;
    const Method *textValue=nullptr,*textFont=nullptr,*fontSize=nullptr,*alignment=nullptr,*color=nullptr,*rayTarget=nullptr,*overflow=nullptr;
    const Method *fonts=nullptr,*builtinFont=nullptr,*renderMode=nullptr,*touchCount=nullptr,*getTouch=nullptr,*frameCount=nullptr,*width=nullptr,*height=nullptr;
    Obj root=nullptr,shopRoot=nullptr,status=nullptr,notice=nullptr,reticle=nullptr,flashImage=nullptr,font=nullptr;
    std::vector<UIWidget> widgets;
    std::unordered_map<int,int> fingerWidget;
    bool bound=false,shopOpen=false;int page=0,lastFrame=-1;float retry=0,canvasHeight=720;
    bool bind(){
        if(bound)return true;
        canvas=R.klass("UnityEngine","Canvas");rect=R.klass("UnityEngine","RectTransform");
        text=R.klass("UnityEngine.UI","Text");image=R.klass("UnityEngine.UI","Image");button=R.klass("UnityEngine.UI","Button");
        input=R.klass("UnityEngine","Input");resources=R.klass("UnityEngine","Resources");
        fontClass=R.klass("UnityEngine","Font");systemType=R.klass("System","Type");screen=R.klass("UnityEngine","Screen");
        if(!canvas||!rect||!text||!image||!button||!input||!fontClass||!systemType){log("Unity UI class is unavailable");return false;}
        ctor=R.method(U.gameObject,".ctor",{"System.String","System.Type[]"});
        anchorMin=R.method(rect,"set_anchorMin",{"UnityEngine.Vector2"});anchorMax=R.method(rect,"set_anchorMax",{"UnityEngine.Vector2"});
        sizeDelta=R.method(rect,"set_sizeDelta",{"UnityEngine.Vector2"});anchorPos=R.method(rect,"set_anchoredPosition",{"UnityEngine.Vector2"});
        pivot=R.method(rect,"set_pivot",{"UnityEngine.Vector2"});rectValue=R.method(rect,"get_rect",{});
        textValue=R.method(text,"set_text",{"System.String"});textFont=R.method(text,"set_font",{"UnityEngine.Font"});
        fontSize=R.method(text,"set_fontSize",{"System.Int32"});alignment=R.method(text,"set_alignment",{"UnityEngine.TextAnchor"});
        color=R.method(image,"set_color",{"UnityEngine.Color"});rayTarget=R.method(image,"set_raycastTarget",{"System.Boolean"});
        overflow=R.method(text,"set_horizontalOverflow",{"UnityEngine.HorizontalWrapMode"});
        fonts=R.method(resources,"FindObjectsOfTypeAll",{"System.Type"});renderMode=R.method(canvas,"get_renderMode",{});
        builtinFont=R.method(resources,"GetBuiltinResource",{"System.Type","System.String"});
        touchCount=R.method(input,"get_touchCount",{});getTouch=R.method(input,"GetTouch",{"System.Int32"});
        frameCount=R.method(U.time,"get_frameCount",{});width=R.method(screen,"get_width",{});height=R.method(screen,"get_height",{});
        const Method *required[]={ctor,anchorMin,anchorMax,sizeDelta,anchorPos,pivot,textValue,textFont,fontSize,alignment,color,rayTarget,fonts,renderMode,touchCount,getTouch,frameCount,width,height};
        for(auto method:required)if(!method){log("Required Unity UI method is unavailable");return false;}
        bound=true;return true;
    }
    Obj node(Obj parent,const char *name,UIRect bounds){
        Obj go=R.pin(R.object_new(U.gameObject));Array *types=(Array*)R.pin(R.array_new(systemType,1));
        if(!go||!types)return nullptr;
        Obj type=R.type(rect);R.gc_write(types,(void**)types->data,type);
        R.call(ctor,go,{R.string_new(name),types});Obj t=R.pin(U.trans(go));U.parent(t,parent);
        V2 min{bounds.x0,bounds.y0},max{bounds.x1,bounds.y1},zero{},center{.5f,.5f};
        R.call(anchorMin,t,{&min});R.call(anchorMax,t,{&max});R.call(sizeDelta,t,{&zero});R.call(anchorPos,t,{&zero});R.call(pivot,t,{&center});
        return go;
    }
    Obj paint(Obj go,Color tint,bool clickable){
        Obj graphic=U.add(go,image);R.call(color,graphic,{&tint});R.call(rayTarget,graphic,{&clickable});return graphic;
    }
    Obj label(Obj parent,const char *name,UIRect bounds,const std::string &value,int align=4,float relativeSize=.03f){
        Obj go=node(parent,name,bounds);Obj label=U.add(go,text);
        R.call(textFont,label,{font});R.call(textValue,label,{R.string_new(value.c_str())});
        int size=std::max(14,(int)(canvasHeight*relativeSize));R.call(fontSize,label,{&size});R.call(alignment,label,{&align});
        bool no=false;R.call(rayTarget,label,{&no});int over=1;R.call(overflow,label,{&over});
        Color light{.9f,.96f,.85f,1};R.call(color,label,{&light});return label;
    }
    void widget(Obj parent,UIRect bounds,const std::string &value,int action,int argument=0,bool shop=false,UIRect hit={}){
        Obj go=node(parent,"TacticalControl",bounds);paint(go,{.07f,.11f,.14f,.84f},true);U.add(go,button);
        Obj text=label(U.trans(go),"Label",{0,0,1,1},value,4,shop?.026f:.033f);
        widgets.push_back({go,text,hit.x1>hit.x0?hit:bounds,action,argument,shop});
    }
    bool create(){
        TraceScope trace("create integrated Unity controls");
        if(!bind())return false;
        Array *canvases=U.objects(canvas);Obj parent=nullptr;
        if(canvases&&canvases->length<1000){auto objects=(Obj*)canvases->data;for(size_t i=0;i<canvases->length;i++){
            if(R.value<int>(R.call(renderMode,objects[i]))!=2){parent=R.pin(U.trans(objects[i],true));break;}
        }}
        if(!parent){log("Unity controls waiting for the game's Canvas");return false;}
        Array *available=(Array*)R.call(fonts,nullptr,{R.type(fontClass)});
        if(available&&available->length>0&&available->length<1000){auto objects=(Obj*)available->data;for(size_t i=0;i<available->length;i++)if(U.alive(objects[i])){font=R.pin(objects[i]);break;}}
        if(!font&&builtinFont)font=R.pin(R.call(builtinFont,nullptr,{R.type(fontClass),R.string_new("LegacyRuntime.ttf")}));
        if(!font){log("Unity controls waiting for a loaded game font");return false;}
        CanvasRect bounds=R.value<CanvasRect>(R.call(rectValue,parent));if(bounds.height>100)canvasHeight=bounds.height;
        root=node(parent,"GrannyTacticalUI",{0,0,1,1});worldRoots.push_back(root);Obj t=U.trans(root);
        status=label(t,"TacticalStatus",{.06f,.91f,.78f,.99f},"AK-47",3,.028f);
        notice=label(t,"TacticalNotice",{.25f,.845f,.75f,.91f},"",4,.024f);
        reticle=label(t,"Crosshair",{.47f,.45f,.53f,.55f},"+",4,.045f);
        widget(t,{.84f,.11f,.975f,.29f},"FIRE",0);
        widget(t,{.73f,.12f,.815f,.205f},"AIM",4);
        widget(t,{.73f,.23f,.815f,.315f},"R",1);
        widget(t,{.63f,.12f,.705f,.205f},"JUMP",5);
        widget(t,{.63f,.23f,.705f,.315f},"NEXT",3,-1);
        widget(t,{.82f,.91f,.975f,.99f},"BUY",9);
        shopRoot=node(t,"WeaponShop",{.15f,.15f,.85f,.85f});Obj shopT=U.trans(shopRoot);paint(shopRoot,{.04f,.06f,.08f,.97f},true);
        label(shopT,"ArsenalTitle",{.04f,.87f,.77f,.99f},"ARSENAL",3,.037f);
        widget(shopT,{.81f,.88f,.97f,.98f},"CLOSE",9,0,true,{.717f,.766f,.829f,.836f});
        for(int i=0;i<12;i++){
            float x=i%2?.52f:.04f,y=.75f-(i/2)*.105f;
            UIRect row{x,y,x+.44f,y+.09f};UIRect hit{.15f+.7f*row.x0,.15f+.7f*row.y0,.15f+.7f*row.x1,.15f+.7f*row.y1};
            widget(shopT,row,"",2,i,true,hit);
        }
        widget(shopT,{.04f,.02f,.2f,.105f},"<",10,-1,true,{.178f,.164f,.29f,.2235f});
        widget(shopT,{.8f,.02f,.96f,.105f},">",10,1,true,{.71f,.164f,.822f,.2235f});
        widget(shopT,{.24f,.02f,.75f,.105f},"ARMOR $1000",7,0,true,{.318f,.164f,.675f,.2235f});
        Obj flashGO=node(t,"FlashEffect",{0,0,1,1});flashImage=paint(flashGO,{1,1,1,0},false);
        shopOpen=false;page=0;lastFrame=-1;fingerWidget.clear();U.active(shopRoot,false);
        log("Integrated Unity UI created; Android gameplay overlay removed");return true;
    }
    void activate(const UIWidget &widget){
        switch(widget.action){
            case 1:reloadInput=1;break;
            case 2:{int id=page*12+widget.value;if(id<WEAPON_COUNT){if(combat.owned[id])selectInput=id;else buyInput=id;shopOpen=false;}break;}
            case 3:for(int i=1;i<=WEAPON_COUNT;i++){int id=(combat.weapon+i)%WEAPON_COUNT;if(combat.owned[id]){selectInput=id;break;}}break;
            case 4:altInput=1;break;
            case 5:jumpInput=1;break;
            case 7:armorInput=1;break;
            case 9:shopOpen=!shopOpen;fireInput=0;break;
            case 10:page=(page+widget.value+(WEAPON_COUNT+11)/12)%((WEAPON_COUNT+11)/12);break;
        }
    }
    void readInput(){
        if(!root)return;
        int frame=R.value<int>(R.call(frameCount));if(frame==lastFrame)return;lastFrame=frame;
        int w=R.value<int>(R.call(width)),h=R.value<int>(R.call(height));if(w<=0||h<=0)return;
        int count=std::min(12,R.value<int>(R.call(touchCount)));std::vector<int> alive;bool firing=false;
        for(int i=0;i<count;i++){
            TouchData touch=R.value<TouchData>(R.call(getTouch,nullptr,{&i}));if(touch.phase>=3)continue;
            alive.push_back(touch.finger);V2 p{touch.position.x/w,touch.position.y/h};
            auto found=fingerWidget.find(touch.finger);
            if(found==fingerWidget.end()){
                int selected=-1;for(size_t j=0;j<widgets.size();j++){
                    const UIWidget &widget=widgets[j];if(widget.shop!=shopOpen&&widget.action!=9)continue;
                    if(widget.shop&&!shopOpen)continue;
                    if(widget.rect.contains(p)){selected=(int)j;break;}
                }
                fingerWidget[touch.finger]=selected;
                if(selected>=0)activate(widgets[selected]);
                found=fingerWidget.find(touch.finger);
            }
            if(found->second>=0&&!shopOpen){const UIWidget &widget=widgets[found->second];if(widget.action==0&&widget.rect.contains(p))firing=true;}
        }
        for(auto it=fingerWidget.begin();it!=fingerWidget.end();)if(std::find(alive.begin(),alive.end(),it->first)==alive.end())it=fingerWidget.erase(it);else++it;
        fireInput=firing;
    }
    void update(bool inGame){
        if(!root){if(!inGame||now<retry)return;retry=now+1;if(!create())return;}
        U.active(root,inGame);if(!inGame)return;U.active(shopRoot,shopOpen);
        std::ostringstream line;line<<"HP "<<combat.health<<"  ARM "<<combat.armor<<"  $"<<combat.money<<"  "<<weapons[combat.weapon].name<<"  ";
        if(combat.reload_left>0)line<<"RELOADING";else line<<combat.ammo[combat.weapon]<<" / "<<combat.reserve[combat.weapon];
        R.call(textValue,status,{R.string_new(line.str().c_str())});R.call(textValue,notice,{R.string_new((killfeed.empty()?notification:killfeed).c_str())});
        U.active(R.call(U.componentGO,reticle),!shopOpen);
        for(const UIWidget &widget:widgets){
            if(!widget.shop){if(widget.action!=9)U.active(widget.root,!shopOpen);continue;}
            if(widget.action==2){int id=page*12+widget.value;U.active(widget.root,id<WEAPON_COUNT);if(id<WEAPON_COUNT){
                std::string text=std::string(weapons[id].name)+(combat.owned[id]?"  OWNED":"  $"+std::to_string(weapons[id].price));R.call(textValue,widget.text,{R.string_new(text.c_str())});
            }}
        }
        Color tint{1,1,1,clamp(flash,0,1)*.9f};R.call(color,flashImage,{&tint});
    }
} nativeUI;
static void resetNativeUI(){nativeUI.root=nativeUI.shopRoot=nativeUI.status=nativeUI.notice=nativeUI.reticle=nativeUI.flashImage=nativeUI.font=nullptr;nativeUI.widgets.clear();nativeUI.fingerWidget.clear();nativeUI.shopOpen=false;nativeUI.retry=0;nativeUI.lastFrame=-1;}
static void updateNativeUI(bool inGame){nativeUI.update(inGame);}
static void inputNativeUI(){nativeUI.readInput();}
