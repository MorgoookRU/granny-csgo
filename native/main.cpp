#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <unistd.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "shadowhook.h"
#include "combat.h"
#include "weapons_generated.h"
#include "target_layout.h"

#define MOD_ITERATION "9"

using Obj = void *;
struct Klass; struct Method; struct Field; struct Type;
struct Array { void *klass, *monitor, *bounds; uintptr_t length; char data[0]; };
struct V3 { float x=0, y=0, z=0; };
struct V2 { float x=0,y=0; };
struct Color { float r, g, b, a; };
struct Hit { V3 point, normal; uint32_t face; float distance; float uv[2]; int collider; };
struct NavHit { V3 point, normal; float distance; int mask, hit; };
static_assert(sizeof(Hit)==44 && sizeof(NavHit)==36, "Unity 6000 value layouts");
static V3 operator+(V3 a,V3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
static V3 operator-(V3 a,V3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
static V3 operator*(V3 a,float n){return {a.x*n,a.y*n,a.z*n};}
static float dot(V3 a,V3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static float length(V3 a){return std::sqrt(dot(a,a));}
static V3 normal(V3 a){float n=length(a);return n>0.0001f?a*(1.f/n):V3{};}
static V3 cross(V3 a,V3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
static V3 flat(V3 a){return {a.x,0,a.z};}
static float clamp(float v,float a,float b){return std::max(a,std::min(v,b));}

static JavaVM *jvm;
static jclass overlay;
static jmethodID soundCallback;
static std::mutex logMutex;
static FILE *logFile;
static FILE *startupFile;
static size_t startupBytes=0;
static std::string readyFile;
static std::atomic<int> fireInput{0}, reloadInput{0}, buyInput{-1}, selectInput{-1};
static std::atomic<int> altInput{0}, jumpInput{0}, spawnInput{0}, armorInput{0};
static std::atomic<bool> started{false}, paused{false};
static uint32_t rng=0xC56018;
static thread_local int traceDepth=0;
struct TraceScope { explicit TraceScope(const char *stage); ~TraceScope(){--traceDepth;} };
static float random01(){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return (rng&0xffffff)/16777216.f;}
static void log(const std::string &s){
    std::lock_guard<std::mutex> lock(logMutex);
    __android_log_print(ANDROID_LOG_INFO,"GrannyCSGO","%s",s.c_str());
    if(logFile){auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();fprintf(logFile,"[%lld pid=%d] %s\n",(long long)ms,getpid(),s.c_str());fflush(logFile);}
    // Preserve the beginning of this session separately from the rolling journal.
    if(startupFile&&startupBytes<32000&&s.compare(0,5,"CALL ")){
        int bytes=fprintf(startupFile,"[pid=%d] %s\n",getpid(),s.c_str());
        if(bytes>0)startupBytes+=bytes;fflush(startupFile);
    }
}
TraceScope::TraceScope(const char *stage){++traceDepth;log(std::string("STAGE ")+stage);}
static std::string bootStatus="Подключение мода…";
static void setStatus(const std::string &s){bootStatus=s;log(s);}

// GC handles are grouped so that rebuilding the world never releases live UI objects.
struct Pins { std::vector<uint32_t> handles; };
static Pins worldPins, uiPins;

struct Runtime {
    void *handle=nullptr;
    void *(*domain_get)();
    const void **(*domain_get_assemblies)(void *,size_t *);
    const void *(*assembly_get_image)(const void *);
    Klass *(*class_from_name)(const void *,const char *,const char *);
    Klass *(*class_get_parent)(Klass *);
    Klass *(*object_get_class)(Obj);
    const char *(*class_get_name)(Klass *);
    const char *(*class_get_namespace)(Klass *);
    bool (*class_is_valuetype)(Klass *);
    bool (*class_is_assignable_from)(Klass *,Klass *);
    int32_t (*class_value_size)(Klass *,uint32_t *);
    Klass *(*method_get_class)(const Method *);
    uint32_t (*method_get_flags)(const Method *,uint32_t *);
    void (*format_exception)(Obj,char *,int);
    const Method *(*class_get_methods)(Klass *,void **);
    const char *(*method_get_name)(const Method *);
    uint32_t (*method_get_param_count)(const Method *);
    const Type *(*method_get_param)(const Method *,uint32_t);
    char *(*type_get_name)(const Type *);
    const Type *(*class_get_type)(Klass *);
    Obj (*type_get_object)(const Type *);
    Field *(*class_get_field_from_name)(Klass *,const char *);
    size_t (*field_get_offset)(Field *);
    void (*field_get_value)(Obj,Field *,void *);
    void (*field_set_value)(Obj,Field *,void *);
    Obj (*runtime_invoke)(const Method *,Obj,void **,Obj *);
    void *(*object_unbox)(Obj);
    Obj (*object_new)(Klass *);
    Array *(*array_new)(Klass *,uintptr_t);
    Obj (*string_new)(const char *);
    void (*gc_write)(Obj,void **,Obj);
    uint32_t (*gchandle_new)(Obj,bool);
    void (*gchandle_free)(uint32_t);
    void (*free_mem)(void *);
    void *(*resolve_icall)(const char *)=nullptr;
    std::unordered_map<std::string,Klass *> classes;
    std::unordered_map<std::string,const Method *> methods;
    std::unordered_map<std::string,Field *> fields;
    Pins *pins=&worldPins;
    Klass *unityObject=nullptr;
    Field *cachedPointer=nullptr;
    std::unordered_set<std::string> failures;

    bool init(){
#define BIND(member,symbol) member=reinterpret_cast<decltype(member)>(dlsym(handle,symbol));if(!member){setStatus(std::string("Нет API: ")+symbol);return false;}
        BIND(domain_get,"il2cpp_domain_get")
        BIND(domain_get_assemblies,"il2cpp_domain_get_assemblies")
        BIND(assembly_get_image,"il2cpp_assembly_get_image")
        BIND(class_from_name,"il2cpp_class_from_name")
        BIND(class_get_parent,"il2cpp_class_get_parent")
        BIND(object_get_class,"il2cpp_object_get_class")
        BIND(class_get_name,"il2cpp_class_get_name")
        BIND(class_get_namespace,"il2cpp_class_get_namespace")
        BIND(class_is_valuetype,"il2cpp_class_is_valuetype")
        BIND(class_is_assignable_from,"il2cpp_class_is_assignable_from")
        BIND(class_value_size,"il2cpp_class_value_size")
        BIND(method_get_class,"il2cpp_method_get_class")
        BIND(method_get_flags,"il2cpp_method_get_flags")
        BIND(format_exception,"il2cpp_format_exception")
        BIND(class_get_methods,"il2cpp_class_get_methods")
        BIND(method_get_name,"il2cpp_method_get_name")
        BIND(method_get_param_count,"il2cpp_method_get_param_count")
        BIND(method_get_param,"il2cpp_method_get_param")
        BIND(type_get_name,"il2cpp_type_get_name")
        BIND(class_get_type,"il2cpp_class_get_type")
        BIND(type_get_object,"il2cpp_type_get_object")
        BIND(class_get_field_from_name,"il2cpp_class_get_field_from_name")
        BIND(field_get_offset,"il2cpp_field_get_offset")
        BIND(field_get_value,"il2cpp_field_get_value")
        BIND(field_set_value,"il2cpp_field_set_value")
        BIND(runtime_invoke,"il2cpp_runtime_invoke")
        BIND(object_unbox,"il2cpp_object_unbox")
        BIND(object_new,"il2cpp_object_new")
        BIND(array_new,"il2cpp_array_new")
        BIND(string_new,"il2cpp_string_new")
        BIND(gc_write,"il2cpp_gc_wbarrier_set_field")
        BIND(gchandle_new,"il2cpp_gchandle_new")
        BIND(gchandle_free,"il2cpp_gchandle_free")
        BIND(free_mem,"il2cpp_free")
#undef BIND
        resolve_icall=reinterpret_cast<decltype(resolve_icall)>(dlsym(handle,"il2cpp_resolve_icall"));
        return true;
    }
    std::string name(Klass *c){return c?std::string(class_get_namespace(c))+"."+class_get_name(c):"null";}
    void failure(const std::string &key,const std::string &message){
        if(failures.size()<128&&failures.insert(key).second)log(message);
    }
    uintptr_t nativePointer(Obj o){
        uintptr_t pointer=0;
        if(o&&cachedPointer&&class_is_assignable_from(unityObject,object_get_class(o)))field_get_value(o,cachedPointer,&pointer);
        return pointer;
    }
    bool alive(Obj o){return nativePointer(o)!=0;}
    Klass *klass(const char *ns,const char *name){
        std::string key=std::string(ns)+"."+name;
        auto f=classes.find(key);if(f!=classes.end())return f->second;
        size_t count=0;auto assemblies=domain_get_assemblies(domain_get(),&count);
        for(size_t i=0;i<count;i++){
            Klass *c=class_from_name(assembly_get_image(assemblies[i]),ns,name);
            if(c){classes[key]=c;return c;}
        }
        classes[key]=nullptr;log("Unavailable class: "+key);return nullptr;
    }
    const Method *method(Klass *c,const char *name,std::initializer_list<const char *> signature){
        if(!c)return nullptr;
        std::string key=this->name(c)+"::"+name;
        for(auto s:signature)key+=std::string(";")+s;
        auto f=methods.find(key);if(f!=methods.end())return f->second;
        for(auto k=c;k;k=class_get_parent(k)){
            void *iter=nullptr;const Method *m;
            while((m=class_get_methods(k,&iter))){
                if(strcmp(method_get_name(m),name)||method_get_param_count(m)!=signature.size())continue;
                bool ok=true;int index=0;
                for(auto s:signature){char *type=type_get_name(method_get_param(m,index++));bool same=type&&!strcmp(type,s);if(type)free_mem(type);if(!same){ok=false;break;}}
                if(ok){methods[key]=m;return m;}
            }
        }
        methods[key]=nullptr;log("Unavailable method: "+key);return nullptr;
    }
    Field *fieldOf(Klass *c,const char *name){
        if(!c)return nullptr;
        std::string key=this->name(c)+"."+name;
        auto f=fields.find(key);if(f!=fields.end())return f->second;
        Field *result=class_get_field_from_name(c,name);fields[key]=result;
        if(!result)log("Unavailable field: "+key);
        return result;
    }
    Obj call(const Method *m,Obj self=nullptr,std::initializer_list<void *> args={}){
        if(!m)return nullptr;
        uint32_t impl=0;bool instance=(method_get_flags(m,&impl)&0x10)==0;
        if(instance&&(!self||!class_is_assignable_from(method_get_class(m),object_get_class(self)))){
            std::string method=method_get_name(m);failure("receiver:"+method,"Skipped invalid receiver: "+method+" expected="+name(method_get_class(m))+" actual="+name(self?object_get_class(self):nullptr));return nullptr;
        }
        if(instance&&cachedPointer&&unityObject&&strcmp(method_get_name(m),".ctor")&&class_is_assignable_from(unityObject,object_get_class(self))){
            if(!alive(self)){
                std::string method=method_get_name(m);failure("destroyed:"+method,"Skipped destroyed Unity object: "+method);return nullptr;
            }
        }
        if(traceDepth)log(std::string("CALL ")+method_get_name(m));
        std::vector<void *> params(args);
        if(params.size()!=method_get_param_count(m)){
            std::string method=method_get_name(m);failure("arguments:"+method,"Skipped wrong argument count: "+method);return nullptr;
        }
        Obj exception=nullptr;Obj result=runtime_invoke(m,self,params.empty()?nullptr:params.data(),&exception);
        if(exception){char message[2048]{};format_exception(exception,message,sizeof(message));std::string method=method_get_name(m);failure("exception:"+method+":"+message,"Unity exception in "+method+": "+message);return nullptr;}
        return result;
    }
    template<class T>T value(Obj o){
        T v{};if(!o)return v;Klass *c=object_get_class(o);uint32_t alignment=0;
        int size=c&&class_is_valuetype(c)?class_value_size(c,&alignment):-1;
        if(size!=(int)sizeof(v)){
            std::string type=name(c);failure("value:"+type+":"+std::to_string(sizeof(v)),"Skipped boxed value: type="+type+" size="+std::to_string(size)+" expected="+std::to_string(sizeof(v)));return v;
        }
        void *raw=object_unbox(o);if(raw)memcpy(&v,raw,sizeof(v));return v;
    }
    template<class T>T field(Obj o,Klass *c,const char *name){T v{};Field *f=fieldOf(c,name);if(o&&f)field_get_value(o,f,&v);return v;}
    template<class T>void set(Obj o,Klass *c,const char *name,T v){Field *f=fieldOf(c,name);if(o&&f)field_set_value(o,f,&v);}
    Obj type(Klass *c){return c?type_get_object(class_get_type(c)):nullptr;}
    Obj pin(Obj o){if(o&&pins)pins->handles.push_back(gchandle_new(o,false));return o;}
    void release(Pins &p){for(auto h:p.handles)gchandle_free(h);p.handles.clear();}
    // System.String: length at 0x10, UTF-16 characters at 0x14 (verified in the metadata dump).
    std::string text(Obj s){
        std::string out;if(!s)return out;
        int32_t n=*reinterpret_cast<int32_t*>(static_cast<char*>(s)+0x10);
        auto chars=reinterpret_cast<const uint16_t*>(static_cast<char*>(s)+0x14);
        for(int i=0;i<n&&i<128;i++){
            uint32_t c=chars[i];
            if(c<0x80)out+=char(c);
            else if(c<0x800){out+=char(0xc0|(c>>6));out+=char(0x80|(c&0x3f));}
            else{out+=char(0xe0|(c>>12));out+=char(0x80|((c>>6)&0x3f));out+=char(0x80|(c&0x3f));}
        }
        return out;
    }
} R;
struct PinScope { Pins *previous; explicit PinScope(Pins &p):previous(R.pins){R.pins=&p;} ~PinScope(){R.pins=previous;} };

struct Unity {
    Klass *object,*gameObject,*component,*behaviour,*transform,*renderer,*material,*shader,*mesh,*meshFilter,*meshRenderer;
    Klass *camera,*time,*physics,*hit,*nav,*agent,*capsule,*controller,*vec,*integer,*fps,*granny,*application,*joystick;
    const Method *destroy,*findObjects,*objectName,*goCtor,*getTransform,*componentTransform,*componentGO,*componentsInChildren;
    const Method *addComponent,*getComponent,*setActive,*setLayer,*getPosition,*setPosition,*getForward,*getRight;
    const Method *getParent,*setParent,*setLocalPosition,*setLocalScale,*setEuler,*rotate,*lookAt;
    const Method *meshCtor,*setVertices,*setNormals,*setTriangles,*setMesh,*getMaterial,*setMaterial,*matCtor,*matShaderCtor,*matColor,*matTexture,*matHasProperty,*shaderFind;
    const Method *cameraMain,*camerasCount,*allCameras,*getFov,*setFov,*getEnabled,*setEnabled;
    const Method *fixedDelta,*frameCount,*targetFrameRate,*raycast,*findById,*sampleNav,*setDestination,*onNav,*agentSpeed,*agentStop;
    const Method *capsuleHeight,*capsuleRadius,*capsuleCenter,*getVelocity,*playerDeath,*grannyShot;
    bool bind(){
#define CLASS(member,ns,name) member=R.klass(ns,name);if(!member){setStatus("Нет класса " name);return false;}
        CLASS(object,"UnityEngine","Object") CLASS(gameObject,"UnityEngine","GameObject")
        CLASS(component,"UnityEngine","Component") CLASS(behaviour,"UnityEngine","Behaviour")
        CLASS(transform,"UnityEngine","Transform") CLASS(renderer,"UnityEngine","Renderer")
        CLASS(material,"UnityEngine","Material") CLASS(shader,"UnityEngine","Shader")
        CLASS(mesh,"UnityEngine","Mesh") CLASS(meshFilter,"UnityEngine","MeshFilter")
        CLASS(meshRenderer,"UnityEngine","MeshRenderer") CLASS(camera,"UnityEngine","Camera")
        CLASS(time,"UnityEngine","Time") CLASS(physics,"UnityEngine","Physics")
        CLASS(hit,"UnityEngine","RaycastHit") CLASS(nav,"UnityEngine.AI","NavMesh")
        CLASS(agent,"UnityEngine.AI","NavMeshAgent") CLASS(capsule,"UnityEngine","CapsuleCollider")
        CLASS(controller,"UnityEngine","CharacterController") CLASS(application,"UnityEngine","Application")
        CLASS(vec,"UnityEngine","Vector3") CLASS(integer,"System","Int32")
        CLASS(fps,"","FPSControllerNEW") CLASS(granny,"","EnemyAIGranny")
#undef CLASS
        joystick=R.klass("","NewJoystickScript");
#define M(dst,cl,n,...) dst=R.method(cl,n,{__VA_ARGS__})
        M(destroy,object,"Destroy","UnityEngine.Object","System.Single");
        M(findObjects,object,"FindObjectsOfType","System.Type");M(objectName,object,"get_name");
        M(goCtor,gameObject,".ctor","System.String");
        M(getTransform,gameObject,"get_transform");M(componentTransform,component,"get_transform");M(componentGO,component,"get_gameObject");
        // GameObject.GetComponentInChildren(Type) is stripped in this build; the array variant survives.
        M(componentsInChildren,component,"GetComponentsInChildren","System.Type");
        M(addComponent,gameObject,"AddComponent","System.Type");M(getComponent,gameObject,"GetComponent","System.Type");
        M(setActive,gameObject,"SetActive","System.Boolean");M(setLayer,gameObject,"set_layer","System.Int32");
        M(getPosition,transform,"get_position");M(setPosition,transform,"set_position","UnityEngine.Vector3");
        M(getForward,transform,"get_forward");M(getRight,transform,"get_right");M(getParent,transform,"get_parent");
        M(setParent,transform,"SetParent","UnityEngine.Transform","System.Boolean");
        M(setLocalPosition,transform,"set_localPosition","UnityEngine.Vector3");M(setLocalScale,transform,"set_localScale","UnityEngine.Vector3");
        M(setEuler,transform,"set_localEulerAngles","UnityEngine.Vector3");M(rotate,transform,"Rotate","UnityEngine.Vector3");
        M(lookAt,transform,"LookAt","UnityEngine.Vector3");
        M(meshCtor,mesh,".ctor");M(setVertices,mesh,"set_vertices","UnityEngine.Vector3[]");M(setNormals,mesh,"set_normals","UnityEngine.Vector3[]");M(setTriangles,mesh,"set_triangles","System.Int32[]");
        M(setMesh,meshFilter,"set_sharedMesh","UnityEngine.Mesh");M(getMaterial,renderer,"get_sharedMaterial");M(setMaterial,renderer,"set_sharedMaterial","UnityEngine.Material");
        M(matCtor,material,".ctor","UnityEngine.Material");M(matShaderCtor,material,".ctor","UnityEngine.Shader");
        M(matColor,material,"set_color","UnityEngine.Color");M(matTexture,material,"set_mainTexture","UnityEngine.Texture");
        M(matHasProperty,material,"HasProperty","System.String");M(shaderFind,shader,"Find","System.String");
        M(cameraMain,camera,"get_main");M(camerasCount,camera,"get_allCamerasCount");M(allCameras,camera,"GetAllCameras","UnityEngine.Camera[]");
        M(getFov,camera,"get_fieldOfView");M(setFov,camera,"set_fieldOfView","System.Single");
        M(getEnabled,behaviour,"get_enabled");M(setEnabled,behaviour,"set_enabled","System.Boolean");
        M(fixedDelta,time,"get_fixedDeltaTime");M(frameCount,time,"get_frameCount");
        M(targetFrameRate,application,"set_targetFrameRate","System.Int32");
        M(raycast,physics,"Raycast","UnityEngine.Vector3","UnityEngine.Vector3","UnityEngine.RaycastHit&","System.Single","System.Int32","UnityEngine.QueryTriggerInteraction");
        M(sampleNav,nav,"SamplePosition","UnityEngine.Vector3","UnityEngine.AI.NavMeshHit&","System.Single","System.Int32");
        M(findById,object,"FindObjectFromInstanceID","System.Int32");
        M(setDestination,agent,"SetDestination","UnityEngine.Vector3");M(onNav,agent,"get_isOnNavMesh");M(agentSpeed,agent,"set_speed","System.Single");M(agentStop,agent,"set_stoppingDistance","System.Single");
        M(capsuleHeight,capsule,"set_height","System.Single");M(capsuleRadius,capsule,"set_radius","System.Single");M(capsuleCenter,capsule,"set_center","UnityEngine.Vector3");
        M(getVelocity,controller,"get_velocity");M(playerDeath,fps,"PlayerGetsCaught");M(grannyShot,granny,"grannyHitByGun");
#undef M
        const Method *required[]={destroy,findObjects,goCtor,getTransform,componentTransform,componentGO,addComponent,getComponent,setActive,getPosition,setPosition,getForward,getRight,getParent,setParent,setLocalPosition,setLocalScale,setEuler,rotate,meshCtor,setVertices,setNormals,setTriangles,setMesh,setMaterial,matColor,raycast,fixedDelta,frameCount};
        for(auto m:required)if(!m){setStatus("Ошибка подключения Unity. Открой журнал мода.");return false;}
        R.unityObject=object;
        R.cachedPointer=R.class_get_field_from_name(object,"m_CachedPtr");
        if(!R.cachedPointer||R.field_get_offset(R.cachedPointer)!=0x10){setStatus("Мод не подключён: изменилось поле нативного объекта Unity");return false;}
        log("Unity object guard uses verified m_CachedPtr offset 0x10");
        return true;
    }
    bool alive(Obj o){return R.alive(o);}
    Obj trans(Obj o,bool component=false){return o?R.call(component?componentTransform:getTransform,o):nullptr;}
    V3 pos(Obj t){return R.value<V3>(R.call(getPosition,t));}
    V3 forward(Obj t){return R.value<V3>(R.call(getForward,t));}
    V3 right(Obj t){return R.value<V3>(R.call(getRight,t));}
    void position(Obj t,V3 p){R.call(setPosition,t,{&p});}
    void local(Obj t,V3 p){R.call(setLocalPosition,t,{&p});}
    void scale(Obj t,V3 p){R.call(setLocalScale,t,{&p});}
    void euler(Obj t,V3 p){R.call(setEuler,t,{&p});}
    void active(Obj o,bool b){if(o)R.call(setActive,o,{&b});}
    bool enabled(Obj behaviourObject){return getEnabled&&R.value<bool>(R.call(getEnabled,behaviourObject));}
    void enable(Obj behaviourObject,bool b){if(behaviourObject&&setEnabled)R.call(setEnabled,behaviourObject,{&b});}
    void remove(Obj o){if(alive(o)){float delay=0;R.call(destroy,nullptr,{o,&delay});}}
    Obj newGO(const char *name){Obj o=R.pin(R.object_new(gameObject));R.call(goCtor,o,{R.string_new(name)});return o;}
    Obj add(Obj go,Klass *k){return k?R.pin(R.call(addComponent,go,{R.type(k)})):nullptr;}
    Obj get(Obj go,Klass *k){return R.call(getComponent,go,{R.type(k)});}
    void parent(Obj t,Obj p){bool world=false;R.call(setParent,t,{p,&world});}
    Array *objects(Klass *k){return (Array *)R.call(findObjects,nullptr,{R.type(k)});}
    std::string name(Obj o){return objectName&&alive(o)?R.text(R.call(objectName,o)):std::string("?");}
    std::string path(Obj t){
        std::string result;
        for(int i=0;t&&i<12;i++){result=name(t)+(result.empty()?"":"/")+result;t=R.call(getParent,t);}
        return result;
    }
    bool ray(V3 start,V3 dir,float distance,Hit &h){
        dir=normal(dir);int mask=-1,ignore=1;h={};
        return R.value<bool>(R.call(raycast,nullptr,{&start,&dir,&h,&distance,&mask,&ignore}));
    }
    Obj hitTrans(Hit &h){
        if(!h.collider||!findById)return nullptr;
        Obj collider=R.call(findById,nullptr,{&h.collider});return collider?trans(collider,true):nullptr;
    }
    V3 navPoint(V3 p){NavHit h{};float range=3;int mask=-1;if(sampleNav&&R.value<bool>(R.call(sampleNav,nullptr,{&p,&h,&range,&mask})))return h.point;return p;}
    bool childOf(Obj t,Obj root){for(int i=0;t&&i<16;i++){if(t==root)return true;t=R.call(getParent,t);}return false;}
} U;

// ---------------------------------------------------------------- game state
static Combat combat;static bool combatReady=false;
static Obj player,playerTransform,character,pivot,cameraObject,cameraTransform,aimTransform,gun,gunTransform,muzzle,cubeMesh;
static Obj mats[10];
static std::vector<Obj> worldRoots;
static V3 waypoints[16];static int waypointCount=0;
static float now=0,stepDt=1/60.f,recoilPitch=0,recoilYaw=0,lastPitch=0,lastYaw=0,flash=0,hurt=0,hitMarker=0,grannyHP=100,grannyReset=0;
static float cameraSearchStart=0,nextCameraProbe=0,nextVisualProbe=0,nextFrameRate=0,muzzleUntil=0,baseFov=65;
static bool scoped=false,burstMode=false,wasCaught=false,inTick=false,botsEnabled=true,worldReady=false,visualsReady=false,deathHandled=false;
static int burstLeft=0,lastGun=17,frameRate=60;
static std::string notification="BUY — магазин, FIRE — огонь",killfeed;
static float noticeUntil=0;
struct Bot {Obj root=nullptr,transform=nullptr,agent=nullptr,head=nullptr,legs[2]{};float hp=100,armor=50,nextShot=0,respawn=0,blind=0,lastSeen=-100,firstSeen=-100,repath=0;V3 target{},routed{1e9f,0,0};int weapon=17;};
static Bot bots[4];
struct Effect {Obj root=nullptr,transform=nullptr;float expires=0;};static Effect effects[12];static int effectIndex=0;
struct Grenade {Obj root=nullptr,transform=nullptr,renderer=nullptr;V3 velocity{},position{};int kind=8;float age=0,remaining=0,tick=0;bool exploded=false;};static Grenade grenades[8];

static void notify(const std::string &text,float seconds=2.5f){notification=text;noticeUntil=now+seconds;}
static void sound(int kind,float volume=1.f){
    if(!jvm||!overlay||!soundCallback)return;
    JNIEnv *env=nullptr;bool detach=false;
    if(jvm->GetEnv((void **)&env,JNI_VERSION_1_6)!=JNI_OK){if(jvm->AttachCurrentThread(&env,nullptr)!=JNI_OK)return;detach=true;}
    env->CallStaticVoidMethod(overlay,soundCallback,kind,volume);
    if(env->ExceptionCheck())env->ExceptionClear();
    if(detach)jvm->DetachCurrentThread();
}

// ---------------------------------------------------------------- frame pacing
// Granny runs physics at 1/75 s while rendering at the platform default rate; movement is
// applied in FixedUpdate, so 5 physics steps land on 4 frames and the camera judders.
// Time.fixedDeltaTime's setter is stripped, so the native TimeManager value is changed after
// verifying the libunity getter instructions and the original rational value.
static int64_t *fixedCount=nullptr;
static bool timeManagerChecked=false;
static bool findTimeManager(){
    if(fixedCount||timeManagerChecked)return fixedCount;
    timeManagerChecked=true;
    if(!R.resolve_icall){log("Frame pacing: il2cpp_resolve_icall unavailable");return false;}
    auto code=reinterpret_cast<const uint32_t*>(R.resolve_icall("UnityEngine.Time::get_fixedDeltaTime"));
    if(!code){log("Frame pacing: fixedDeltaTime icall not registered");return false;}
    if(code[0]!=target::fixedGetter[0]||(code[1]>>26)!=0x25||code[2]!=target::fixedGetter[2]||code[3]!=target::fixedGetter[3]){
        log("Frame pacing: unexpected fixedDeltaTime getter code; physics rate unchanged");return false;
    }
    int64_t offset=int64_t(code[1]&0x3ffffff);if(offset&0x2000000)offset-=0x4000000;
    auto getter=reinterpret_cast<const uint32_t*>(reinterpret_cast<const char*>(code+1)+offset*4);
    if(getter[0]!=target::timeManagerGetter){log("Frame pacing: unexpected GetTimeManager code");return false;}
    auto manager=reinterpret_cast<char*(*)()>(const_cast<uint32_t*>(getter))();
    if(!manager)return false;
    auto count=reinterpret_cast<int64_t*>(manager+0x50);auto rate=reinterpret_cast<uint32_t*>(manager+0x58);
    if(rate[0]!=target::fixedRate||rate[1]!=1||count[0]<=0||count[0]>target::fixedRate){
        std::ostringstream s;s<<"Frame pacing: unexpected fixed step "<<count[0]<<"/"<<rate[0]<<"/"<<rate[1];log(s.str());return false;
    }
    fixedCount=count;
    std::ostringstream s;s<<"Frame pacing: TimeManager fixed step "<<count[0]<<"/"<<rate[0]<<" ("<<(count[0]==target::originalFixedCount?"original 1/75 s":"already changed")<<")";log(s.str());
    return true;
}
static void applyFrameRate(bool force){
    if(!force&&now<nextFrameRate)return;
    nextFrameRate=now+4;
    if(U.targetFrameRate){int fps=frameRate;R.call(U.targetFrameRate,nullptr,{&fps});}
    if(findTimeManager()){
        int64_t wanted=target::fixedRate/frameRate;
        if(*fixedCount!=wanted){
            *fixedCount=wanted;
            float check=R.value<float>(R.call(U.fixedDelta));
            std::ostringstream s;s<<"Frame pacing: "<<frameRate<<" FPS, physics step now "<<check<<" s";log(s.str());
        }
    }
}

// ---------------------------------------------------------------- materials and models
static Obj makeMesh(){
    TraceScope trace("mesh allocation");
    static const V3 v[]={
      {-0.5f,-0.5f,0.5f},{0.5f,-0.5f,0.5f},{0.5f,0.5f,0.5f},{-0.5f,0.5f,0.5f},
      {0.5f,-0.5f,-0.5f},{-0.5f,-0.5f,-0.5f},{-0.5f,0.5f,-0.5f},{0.5f,0.5f,-0.5f},
      {0.5f,-0.5f,0.5f},{0.5f,-0.5f,-0.5f},{0.5f,0.5f,-0.5f},{0.5f,0.5f,0.5f},
      {-0.5f,-0.5f,-0.5f},{-0.5f,-0.5f,0.5f},{-0.5f,0.5f,0.5f},{-0.5f,0.5f,-0.5f},
      {-0.5f,0.5f,0.5f},{0.5f,0.5f,0.5f},{0.5f,0.5f,-0.5f},{-0.5f,0.5f,-0.5f},
      {-0.5f,-0.5f,-0.5f},{0.5f,-0.5f,-0.5f},{0.5f,-0.5f,0.5f},{-0.5f,-0.5f,0.5f}};
    const V3 faceNormals[]={{0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
    V3 normals[24];int triangles[36];
    for(int f=0;f<6;f++){for(int j=0;j<4;j++)normals[f*4+j]=faceNormals[f];int a=f*4;int t[]={a,a+1,a+2,a,a+2,a+3};memcpy(triangles+f*6,t,sizeof(t));}
    auto vertices=(Array*)R.pin(R.array_new(U.vec,24)),ns=(Array*)R.pin(R.array_new(U.vec,24)),ts=(Array*)R.pin(R.array_new(U.integer,36));
    if(!vertices||!ns||!ts){log("Model data allocation failed");return nullptr;}
    memcpy(vertices->data,v,sizeof(v));memcpy(ns->data,normals,sizeof(normals));memcpy(ts->data,triangles,sizeof(triangles));
    Obj mesh=R.pin(R.object_new(U.mesh));R.call(U.meshCtor,mesh);
    R.call(U.setVertices,mesh,{vertices});R.call(U.setNormals,mesh,{ns});R.call(U.setTriangles,mesh,{ts});return mesh;
}
static bool makeMaterials(){
    TraceScope trace("scene materials");
    // Granny's house is dark and most scene materials use Mobile/Diffuse, which has no _Color.
    // These shaders are present in this build; self-illumination keeps models readable at night.
    const char *shaders[]={"Legacy Shaders/Self-Illumin/Diffuse","Legacy Shaders/Diffuse","Sprites/Default","Hidden/Internal-Colored"};
    Obj chosen=nullptr;std::string chosenName;
    for(auto name:shaders){
        if(!U.shaderFind||!U.matShaderCtor)break;
        Obj shader=R.call(U.shaderFind,nullptr,{R.string_new(name)});
        if(!U.alive(shader))continue;
        Obj probe=R.object_new(U.material);R.call(U.matShaderCtor,probe,{shader});
        bool hasColor=!U.matHasProperty||R.value<bool>(R.call(U.matHasProperty,probe,{R.string_new("_Color")}));
        if(hasColor){chosen=shader;chosenName=name;break;}
    }
    const Color colors[]={{.10f,.11f,.12f,1},{.45f,.23f,.09f,1},{.58f,.61f,.65f,1},{.16f,.30f,.58f,1},{.60f,.47f,.27f,1},{1,.82f,.28f,1},{.62f,.62f,.64f,1},{1,.36f,.05f,1},{.82f,.62f,.48f,1},{.24f,.28f,.15f,1}};
    Obj source=nullptr;
    if(!chosen){
        auto renderers=U.objects(U.renderer);
        if(renderers&&renderers->length<10000){auto objects=(Obj *)renderers->data;for(size_t i=0;i<renderers->length;i++){source=R.call(U.getMaterial,objects[i]);if(U.alive(source))break;}}
        if(!U.alive(source)||!U.matCtor){log("No material source for mod models");return false;}
        chosenName="copy of scene material";
    }
    for(int i=0;i<10;i++){
        mats[i]=R.pin(R.object_new(U.material));
        if(chosen)R.call(U.matShaderCtor,mats[i],{chosen});else{R.call(U.matCtor,mats[i],{source});R.call(U.matTexture,mats[i],{nullptr});}
        Color c=colors[i];R.call(U.matColor,mats[i],{&c});
    }
    log("Model material: "+chosenName);
    cubeMesh=makeMesh();return cubeMesh;
}
static Obj box(Obj parent,const char *name,V3 position,V3 size,int material,int layer=0){
    if(!cubeMesh)return nullptr;
    Obj root=U.newGO(name),t=R.pin(U.trans(root));if(parent)U.parent(t,parent);
    U.local(t,position);U.scale(t,size);R.call(U.setLayer,root,{&layer});
    Obj filter=U.add(root,U.meshFilter),renderer=U.add(root,U.meshRenderer);
    R.call(U.setMesh,filter,{cubeMesh});R.call(U.setMaterial,renderer,{mats[material]});return root;
}
static void weaponModel(){
    TraceScope trace("weapon model");
    if(!cubeMesh||!U.alive(aimTransform)){log("Weapon model deferred: no aim transform");return;}
    if(U.alive(gun))U.remove(gun);
    worldRoots.erase(std::remove(worldRoots.begin(),worldRoots.end(),gun),worldRoots.end());
    gun=U.newGO("CSGO_Weapon");worldRoots.push_back(gun);gunTransform=R.pin(U.trans(gun));U.parent(gunTransform,aimTransform);
    if(!U.alive(gun)||!U.alive(gunTransform)){gun=gunTransform=nullptr;notify("Не удалось создать оружие");return;}
    U.local(gunTransform,{.22f,-.2f,.42f});
    const Weapon &w=weapons[combat.weapon];
    int furniture=combat.weapon==17||combat.weapon==21?1:(w.kind==3&&combat.weapon==25?9:0);
    float body=w.kind==0?.2f:(w.kind==3?.5f:.36f);
    // Sleeve and hands make the view model read as held rather than floating.
    box(gunTransform,"Sleeve",{.02f,-.10f,-.16f},{.09f,.09f,.26f},0,2);
    box(gunTransform,"Hand",{0,-.075f,-body*.22f},{.06f,.07f,.08f},8,2);
    if(w.kind>=8){box(gunTransform,"Grenade",{0,-.015f,.05f},{.07f,.11f,.07f},w.kind==11?7:(w.kind==9?2:9),2);box(gunTransform,"Spoon",{.03f,.04f,.05f},{.012f,.05f,.03f},2,2);muzzle=nullptr;return;}
    if(w.kind==6){box(gunTransform,"Blade",{0,.01f,.16f},{.012f,.045f,.26f},2,2);box(gunTransform,"Handle",{0,0,-.03f},{.03f,.04f,.11f},0,2);muzzle=nullptr;return;}
    if(w.kind==7){box(gunTransform,"Taser",{0,0,.04f},{.05f,.07f,.16f},5,2);muzzle=box(gunTransform,"Spark",{0,0,.14f},{.03f,.03f,.03f},5,2);return;}
    box(gunTransform,"Receiver",{0,0,body*.15f},{.06f,.07f,body},0,2);
    box(gunTransform,"Barrel",{0,.02f,body*.72f},{.022f,.022f,w.silenced?.24f:.16f},0,2);
    box(gunTransform,"Grip",{0,-.07f,-body*.2f},{.04f,.12f,.055f},furniture,2);
    box(gunTransform,"Magazine",{0,-.07f,body*.1f},{.035f,w.kind==5?.09f:.13f,w.kind==5?.13f:.05f},0,2);
    if(w.kind>0&&w.kind<6){
        box(gunTransform,"Stock",{0,-.015f,-body*.62f},{.045f,.075f,.17f},furniture,2);
        box(gunTransform,"Handguard",{0,-.005f,body*.48f},{.055f,.06f,.15f},furniture,2);
        box(gunTransform,"LeftHand",{-.02f,-.05f,body*.5f},{.06f,.06f,.08f},8,2);
    }
    if(w.scoped){box(gunTransform,"Scope",{0,.07f,.03f},{.05f,.05f,w.kind==3?.22f:.09f},0,2);box(gunTransform,"Lens",{0,.07f,w.kind==3?.14f:.08f},{.04f,.04f,.006f},3,2);}
    else box(gunTransform,"FrontSight",{0,.045f,body*.62f},{.01f,.025f,.01f},0,2);
    if(combat.weapon==9){box(gunTransform,"SecondPistol",{-.34f,0,.025f},{.06f,.07f,.2f},0,2);box(gunTransform,"SecondHand",{-.34f,-.075f,-.03f},{.06f,.07f,.08f},8,2);}
    muzzle=box(gunTransform,"MuzzleFlash",{0,.02f,body*.72f+(w.silenced?.13f:.1f)},{.06f,.06f,.04f},5,2);
    if(muzzle)U.active(muzzle,false);
}
static V3 spawnPosition(int i){
    V3 p=waypointCount?waypoints[(i*3+2)%waypointCount]:U.pos(playerTransform)+V3{float(4+i),0,3};
    return U.navPoint(p);
}
static void spawnBots(){
    TraceScope trace("spawn four bots");
    for(int i=0;i<4;i++){
        Bot &b=bots[i];if(U.alive(b.root))U.remove(b.root);b={};b.weapon=(int[]){17,18,11,7}[i];
        b.root=U.newGO("CSGO_Bot");U.active(b.root,false);worldRoots.push_back(b.root);b.transform=R.pin(U.trans(b.root));U.position(b.transform,spawnPosition(i));
        int team=i%2?4:3;
        box(b.transform,"Torso",{0,1.05f,0},{.46f,.6f,.28f},team);
        box(b.transform,"Face",{0,1.55f,.02f},{.24f,.26f,.24f},8);
        b.head=R.pin(U.trans(box(b.transform,"Helmet",{0,1.66f,0},{.3f,.14f,.3f},team==3?0:9)));
        for(int j=0;j<2;j++){b.legs[j]=R.pin(U.trans(box(b.transform,"Leg",{j?.13f:-.13f,.43f,0},{.16f,.78f,.19f},0)));box(b.transform,"Arm",{j?.31f:-.31f,1.09f,.14f},{.13f,.45f,.16f},team);}
        box(b.transform,"Rifle",{.2f,1.11f,.33f},{.07f,.075f,.5f},0);
        Obj collider=U.add(b.root,U.capsule);float height=1.85f,radius=.32f;V3 center{0,.925f,0};R.call(U.capsuleHeight,collider,{&height});R.call(U.capsuleRadius,collider,{&radius});R.call(U.capsuleCenter,collider,{&center});
        b.agent=U.add(b.root,U.agent);float speed=2.6f,stop=4;R.call(U.agentSpeed,b.agent,{&speed});R.call(U.agentStop,b.agent,{&stop});
        b.hp=100;b.armor=50;b.nextShot=now+6+i*.5f;b.target=spawnPosition(i+1);
        U.active(b.root,true);
    }
    notify("Боты: 4 противника на карте");
}
static void removeBots(){for(auto &b:bots){if(U.alive(b.root))U.remove(b.root);b={};}}

// ---------------------------------------------------------------- scene discovery
static Obj findPlayerCamera(std::string &how){
    // Granny 1.8.12 renders through Player/CameraShakeAnim/CameraPivot/Main Camera/Camera.
    // The MainCamera-tagged parent camera is disabled, so Camera.main is null during play.
    if(U.alive(pivot)&&U.componentsInChildren){
        auto list=(Array*)R.call(U.componentsInChildren,pivot,{R.type(U.camera)});
        if(list&&list->length<64){auto items=(Obj*)list->data;for(size_t i=0;i<list->length;i++)if(U.alive(items[i])&&U.enabled(items[i])){how="enabled camera under cameraPivot";return items[i];}}
    }
    if(U.camerasCount&&U.allCameras){
        int count=R.value<int>(R.call(U.camerasCount));
        if(count>0&&count<64){
            auto list=(Array*)R.array_new(U.camera,count);
            if(list){R.call(U.allCameras,nullptr,{list});auto items=(Obj*)list->data;
                for(size_t i=0;i<list->length;i++)if(U.alive(items[i])&&U.childOf(U.trans(items[i],true),playerTransform)){how="enabled camera inside player";return items[i];}}
        }
    }
    Obj main=U.cameraMain?R.call(U.cameraMain):nullptr;
    if(U.alive(main)){how="Camera.main";return main;}
    return nullptr;
}
static void releaseWorld(){
    for(auto root:worldRoots)U.remove(root);worldRoots.clear();
    R.release(worldPins);
    player=playerTransform=character=pivot=cameraObject=cameraTransform=aimTransform=gun=gunTransform=muzzle=cubeMesh=nullptr;
    for(auto &m:mats)m=nullptr;for(auto &b:bots)b={};for(auto &e:effects)e={};for(auto &g:grenades)g={};
    worldReady=visualsReady=false;
}
struct MoveState {V3 velocity{};float vy=0,air=0,budget=0;bool grounded=true,jumping=false;float jumpBuffer=-1;int frames=0;};
static MoveState mv;
static void resetMovement(){mv=MoveState{};}
static void adoptController(Obj self){
    TraceScope trace("adopt gameplay controller");
    releaseWorld();
    player=R.pin(self);playerTransform=R.pin(U.trans(self,true));
    character=R.field<Obj>(self,U.fps,"character");
    Obj playerGO=R.call(U.componentGO,self);
    if(!U.alive(character)&&U.alive(playerGO))character=U.get(playerGO,U.controller);
    character=R.pin(character);pivot=R.pin(R.field<Obj>(self,U.fps,"cameraPivot"));
    cameraSearchStart=now;nextCameraProbe=0;nextVisualProbe=0;
    waypointCount=0;
    Obj grannyGO=R.field<Obj>(self,U.fps,"granny");Obj granny=U.alive(grannyGO)?U.get(grannyGO,U.granny):nullptr;
    if(granny){for(int i=1;i<=16;i++){std::string n="nav"+std::to_string(i);Obj t=R.field<Obj>(granny,U.granny,n.c_str());if(U.alive(t))waypoints[waypointCount++]=U.pos(t);}}
    if(!combatReady){combat_init(&combat,weapons,WEAPON_COUNT);combat.owned[17]=1;combat.weapon=17;combat.helmet=1;combatReady=true;}
    else{
        // A new Granny day keeps the bought inventory, like the next round in CS:GO.
        combat.health=100;combat.armor=std::max(combat.armor,0);combat.reload_left=0;combat.cooldown=0;combat.trigger_down=0;
        for(int i=0;i<WEAPON_COUNT;i++)if(combat.owned[i]&&weapons[i].kind<8){combat.ammo[i]=weapons[i].magazine;combat.reserve[i]=weapons[i].reserve;}
    }
    scoped=burstMode=wasCaught=deathHandled=false;burstLeft=0;recoilPitch=recoilYaw=lastPitch=lastYaw=0;flash=hurt=hitMarker=0;grannyHP=100;grannyReset=0;
    resetMovement();
    std::ostringstream s;s<<"Controller adopted: player="<<U.path(playerTransform)<<"; character="<<(U.alive(character)?"ok":"missing")
        <<"; cameraPivot="<<(U.alive(pivot)?U.path(pivot):"missing")<<"; waypoints="<<waypointCount;
    log(s.str());
    applyFrameRate(true);
}
static void prepareWorld(){
    if(now<nextVisualProbe)return;
    nextVisualProbe=now+.5f;
    if(!U.alive(cameraObject)&&now>=nextCameraProbe){
        nextCameraProbe=now+1;std::string how;Obj cam=findPlayerCamera(how);
        if(cam){
            cameraObject=R.pin(cam);cameraTransform=R.pin(U.trans(cam,true));aimTransform=cameraTransform;
            if(U.getFov)baseFov=clamp(R.value<float>(R.call(U.getFov,cam)),30,100);
            log("Camera: "+U.path(cameraTransform)+" via "+how);
            if(visualsReady)weaponModel();
        }
    }
    if(!U.alive(aimTransform)&&now-cameraSearchStart>2.5f&&U.alive(pivot)){
        aimTransform=pivot;log("Camera not found yet; aiming from cameraPivot "+U.path(pivot));
    }
    if(!U.alive(aimTransform))return;
    if(!worldReady){worldReady=true;log("Gameplay ready; weapon "+std::string(weapons[combat.weapon].name));}
    if(!visualsReady){
        visualsReady=makeMaterials();
        if(visualsReady){weaponModel();if(botsEnabled)spawnBots();log("Scene models ready");}
    }
}

// ---------------------------------------------------------------- combat
static bool smokeBetween(V3 a,V3 b){
    V3 delta=b-a;float n=dot(delta,delta);if(n<.01f)return false;
    for(auto &g:grenades)if(g.root&&g.exploded&&g.kind==10&&g.remaining>0){float t=clamp(dot(g.position-a,delta)/n,0,1);if(length(a+delta*t-g.position)<2)return true;}
    return false;
}
static bool visible(V3 from,V3 to,Obj target){
    if(smokeBetween(from,to))return false;
    V3 d=to-from;float distance=length(d);Hit h;
    if(!U.ray(from,d,distance,h))return true;
    Obj t=U.hitTrans(h);return target&&U.childOf(t,target);
}
static void botDamage(Bot &b,float damage,bool head){
    if(b.hp<=0)return;
    if(head)damage*=4;
    if(b.armor>0){float absorb=std::min(b.armor,damage*.35f);b.armor-=absorb;damage-=absorb;}
    b.hp-=damage;hitMarker=.18f;
    if(b.hp<=0){U.active(b.root,false);b.respawn=now+8;combat_reward(&combat,300);killfeed=std::string(weapons[combat.weapon].name)+(head?"  >  HEADSHOT":"  >  BOT");noticeUntil=now+3;}
}
static Obj grannyObject(){Obj go=R.field<Obj>(player,U.fps,"granny");return U.alive(go)?U.get(go,U.granny):nullptr;}
static void grannyDamage(Obj ai,float damage){
    grannyHP-=damage;hitMarker=.18f;
    if(grannyHP<=0&&now>grannyReset){R.call(U.grannyShot,ai);combat_reward(&combat,300);grannyReset=now+10;grannyHP=100;killfeed="GRANNY  >  +$300";noticeUntil=now+3;}
}
static void hitDamage(Hit &h,float damage){
    Obj t=U.hitTrans(h);
    for(auto &b:bots)if(b.hp>0&&U.childOf(t,b.transform)){botDamage(b,damage,h.point.y-U.pos(b.transform).y>1.45f);return;}
    Obj ai=grannyObject();if(ai){Obj gt=U.trans(ai,true);if(U.childOf(t,gt))grannyDamage(ai,damage*(h.point.y-U.pos(gt).y>1.5f?4:1));}
}
static void tracer(V3 from,V3 to){
    Effect &e=effects[(effectIndex++)%12];
    if(!U.alive(e.root)){e.root=box(nullptr,"CSGO_Tracer",{},V3{.012f,.012f,1},5,2);if(!e.root)return;e.transform=R.pin(U.trans(e.root));worldRoots.push_back(e.root);}
    U.active(e.root,true);U.position(e.transform,(from+to)*.5f);U.scale(e.transform,{.012f,.012f,length(to-from)});R.call(U.lookAt,e.transform,{&to});e.expires=now+.045f;
}
static void throwGrenade(int kind){
    Grenade *slot=nullptr;for(auto &g:grenades)if(!g.root){slot=&g;break;}
    if(!slot){notify("На карте уже 8 гранат");return;}
    *slot={};slot->kind=kind;slot->position=U.pos(aimTransform)+U.forward(aimTransform)*.45f;
    slot->velocity=U.forward(aimTransform)*9+V3{0,2.5f,0}+mv.velocity*.5f;
    slot->root=box(nullptr,"CSGO_Grenade",slot->position,{.1f,.15f,.1f},kind==11?7:9,2);
    if(!slot->root){*slot={};return;}
    slot->transform=R.pin(U.trans(slot->root));slot->renderer=U.get(slot->root,U.meshRenderer);worldRoots.push_back(slot->root);
}
static void selectWeapon(int id){
    if(!combat_select(&combat,WEAPON_COUNT,id))return;
    if(weapons[id].kind<8)lastGun=id;
    scoped=burstMode=false;burstLeft=0;weaponModel();
}
static void shot(){
    const Weapon &w=weapons[combat.weapon];sound(std::min(w.kind,7),w.silenced?.28f:1.f);
    if(w.kind>=8){
        throwGrenade(w.kind);
        // Thrown grenades leave the inventory and the previous gun comes back, as in CS:GO.
        combat.owned[combat.weapon]=0;selectWeapon(combat.owned[lastGun]?lastGun:34);
        return;
    }
    V3 origin=U.pos(aimTransform),forward=U.forward(aimTransform),right=U.right(aimTransform),up=cross(forward,right);
    float moving=length(flat(mv.velocity));
    float spread=w.spread+(moving>1?1.8f*clamp(moving/5,0,1):0.f)+std::min(combat.shots*.12f,2.0f);
    if(!mv.grounded)spread+=3;
    if(R.field<bool>(player,U.fps,"playerCrouch"))spread*=.6f;
    if(scoped)spread*=.25f;else if(w.kind==3)spread+=3;
    float range=w.kind==6?2.2f:(w.kind==7?3.4f:150.f);
    for(int i=0;i<w.pellets;i++){
        V3 dir=normal(forward+right*((random01()-.5f)*spread*.0349f)+up*((random01()-.5f)*spread*.0349f));
        Hit h;V3 end=origin+dir*range;
        if(U.ray(origin,dir,range,h)){end=h.point;float damage=w.damage;if(w.kind==4)damage*=clamp(1-h.distance/22.f,.15f,1);hitDamage(h,damage);}
        if(w.kind<6)tracer(origin+forward*.3f-up*.08f+right*.08f,end);
    }
    if(muzzle){U.active(muzzle,true);muzzleUntil=now+.04f;}
    recoilPitch=std::min(recoilPitch+w.kick,12.f);
    recoilYaw+=w.kick*.45f*std::sin(combat.shots*1.7f);
}
static void radiusDamage(V3 p,float radius,float damage){
    for(auto &b:bots)if(b.hp>0){V3 target=U.pos(b.transform)+V3{0,1,0};float d=length(target-p);if(d<radius&&visible(p+V3{0,.15f,0},target,b.transform))botDamage(b,damage*(1-d/radius),false);}
    Obj ai=grannyObject();if(ai){V3 target=U.pos(U.trans(ai,true))+V3{0,1,0};if(length(target-p)<radius)grannyDamage(ai,damage);}
    float self=length(U.pos(playerTransform)+V3{0,1,0}-p);if(self<radius*.7f){combat_damage(&combat,int(damage*.5f*(1-self/radius)));hurt=.5f;}
}
static void updateGrenades(float dt){
    for(auto &g:grenades)if(g.root){
        if(!U.alive(g.root)){g={};continue;}
        g.age+=dt;
        if(!g.exploded){
            g.velocity.y-=9.81f*dt;V3 step=g.velocity*dt;Hit hit;
            bool collision=U.ray(g.position,step,length(step)+.06f,hit);
            if(collision){g.position=hit.point+hit.normal*.08f;g.velocity=(g.velocity-hit.normal*(2*dot(g.velocity,hit.normal)))*.45f;}else g.position=g.position+step;
            U.position(g.transform,g.position);
            if(g.age<1.6f&&!(g.kind==11&&collision&&g.age>.2f))continue;
            g.exploded=true;
            if(g.kind==8){radiusDamage(g.position,6,140);U.scale(g.transform,{1.2f,1.2f,1.2f});R.call(U.setMaterial,g.renderer,{mats[7]});g.remaining=.12f;sound(3,.9f);}
            if(g.kind==9){
                for(auto &b:bots)if(b.hp>0&&length(U.pos(b.transform)-g.position)<15&&visible(g.position,U.pos(b.transform)+V3{0,1.5f,0},b.transform))b.blind=now+4;
                V3 d=g.position-U.pos(aimTransform);if(length(d)<15&&visible(U.pos(aimTransform),g.position,nullptr))flash=clamp(dot(normal(d),U.forward(aimTransform))*.8f+.2f,0,1);
                U.scale(g.transform,{.8f,.8f,.8f});R.call(U.setMaterial,g.renderer,{mats[2]});g.remaining=.08f;
            }
            if(g.kind==10){U.position(g.transform,g.position+V3{0,1,0});U.scale(g.transform,{3.8f,2.7f,3.8f});R.call(U.setMaterial,g.renderer,{mats[6]});g.remaining=18;}
            if(g.kind==11){U.scale(g.transform,{4,.14f,4});R.call(U.setMaterial,g.renderer,{mats[7]});g.remaining=7;}
            if(g.kind==12){g.remaining=12;}
        }else{
            g.remaining-=dt;g.tick-=dt;
            if(g.kind==11&&g.tick<=0){radiusDamage(g.position,2.5f,9);if(length(U.pos(playerTransform)-g.position)<2.5f){combat_damage(&combat,6);hurt=.4f;}g.tick=.3f;}
            if(g.kind==12&&g.tick<=0){sound(2,.4f);g.tick=.4f;for(auto &b:bots)if(now-b.lastSeen>3)b.target=g.position;}
            if(g.remaining<=0){U.remove(g.root);g.root=nullptr;}
        }
    }
}
static void updateBots(){
    if(!botsEnabled)return;
    V3 target=U.pos(aimTransform),ground=U.pos(playerTransform);
    for(int i=0;i<4;i++){
        Bot &b=bots[i];if(!U.alive(b.root)&&b.hp>0)continue;
        if(b.hp<=0){if(b.root&&now>=b.respawn){U.position(b.transform,spawnPosition(i));U.active(b.root,true);b.hp=100;b.armor=50;b.nextShot=now+1.5f;}continue;}
        V3 p=U.pos(b.transform),eye=p+V3{0,1.5f,0};float dist=length(target-eye);
        bool seen=now>b.blind&&dist<25&&visible(eye,target,playerTransform);
        if(seen){
            if(now-b.lastSeen>.3f){b.firstSeen=now;b.nextShot=std::max(b.nextShot,now+.55f);} // reaction time
            b.lastSeen=now;b.target=ground;V3 look=ground;look.y=p.y;R.call(U.lookAt,b.transform,{&look});
        }
        else if(now-b.lastSeen>6&&length(b.target-p)<2)b.target=spawnPosition(i+(int)now/10);
        // Re-plan only when the goal moved or every half second; SetDestination recomputes a path.
        if((length(b.target-b.routed)>.5f||now>=b.repath)&&R.value<bool>(R.call(U.onNav,b.agent))){R.call(U.setDestination,b.agent,{&b.target});b.routed=b.target;b.repath=now+.5f;}
        float stride=seen&&dist<5?0:std::sin(now*8+i)*16;for(int j=0;j<2;j++)U.euler(b.legs[j],{j?stride:-stride,0,0});
        if(seen&&now>=b.nextShot&&combat.health>0){
            const Weapon &w=weapons[b.weapon];b.nextShot=now+std::max(.3f,60.f/w.rpm*2.2f);
            tracer(eye,target);sound(std::min(w.kind,5),.16f);
            float moving=length(flat(mv.velocity));
            float chance=clamp(.7f-dist*.02f-moving*.05f-(mv.grounded?0:.15f),.1f,.75f);
            if(random01()<chance){
                int damage=(int)(w.damage*.35f);
                if(random01()<(combat.helmet?.03f:.1f))damage*=2;
                combat_damage(&combat,damage);hurt=std::max(hurt,.45f);
            }
        }
    }
}
#include "unity_ui.inc"

// ---------------------------------------------------------------- movement
// Granny's FirstPersonControl-style update sets the horizontal speed directly from the
// joystick, zeroes it in the air (inAirMultiplier=0) and counts any airtime above 0.36 s as a
// hard landing and above 0.7 s as a deadly fall. CharacterController.Move is intercepted for the
// player only: ground acceleration/friction, air momentum and a jump with its own gravity.
static constexpr float groundAccel=42,groundFriction=32,airAccel=9,jumpSpeed=5.1f,jumpGravity=14.5f;
static Obj moveOwner=nullptr;
static int32_t (*originalMove)(Obj,V3,const Method*);
static V3 approach(V3 v,V3 target,float delta){V3 d=target-v;float l=length(d);return l<=delta||l<1e-5f?target:v+d*(delta/l);}
static float weaponSpeed(){const Weapon &w=weapons[combat.weapon];return clamp(w.speed/5.f,.6f,1.f)*(scoped?.6f:1.f);}
static V3 joystickVelocity(){
    if(!U.joystick)return {};
    Obj stick=R.field<Obj>(player,U.fps,"joystick");if(!stick)return {};
    V2 input=R.field<V2>(stick,U.joystick,"<inputDir>k__BackingField");
    V3 forward=normal(flat(U.forward(playerTransform))),right=normal(flat(U.right(playerTransform)));
    float ax=std::fabs(input.x),ay=std::fabs(input.y);if(ax<.02f&&ay<.02f)return {};
    float speed=ay>ax?R.field<float>(player,U.fps,input.y>=0?"forwardSpeed":"backwardSpeed")*ay:R.field<float>(player,U.fps,"sidestepSpeed")*ax;
    return normal(right*input.x+forward*input.y)*speed;
}
static int32_t hookedMove(Obj self,V3 motion,const Method *method){
    if(!moveOwner||self!=character||!worldReady||paused)return originalMove(self,motion,method);
    float dt=stepDt;
    V3 desired=mv.grounded?V3{motion.x/dt,0,motion.z/dt}:joystickVelocity();
    desired=desired*weaponSpeed();
    bool input=length(desired)>.05f;
    if(mv.grounded)mv.velocity=approach(mv.velocity,input?desired:V3{},(input?groundAccel:groundFriction)*dt);
    else if(input)mv.velocity=approach(mv.velocity,desired,airAccel*dt);
    if(mv.jumpBuffer>=0&&now-mv.jumpBuffer>.2f)mv.jumpBuffer=-1;
    if(mv.jumpBuffer>=0&&mv.grounded&&!mv.jumping){
        mv.jumping=true;mv.vy=jumpSpeed;mv.air=0;mv.jumpBuffer=-1;mv.budget=2*jumpSpeed/jumpGravity+.12f;
    }
    V3 result{mv.velocity.x*dt,motion.y,mv.velocity.z*dt};
    if(mv.jumping){mv.vy-=jumpGravity*dt;result.y=mv.vy*dt;mv.air+=dt;}
    int32_t flags=originalMove(self,result,method);
    bool below=flags&4,above=flags&2;
    if(mv.jumping){if(above&&mv.vy>0)mv.vy=0;if(below&&mv.vy<=0)mv.jumping=false;}
    mv.grounded=below;
    // Hitting a wall removes the matching momentum instead of sliding forever into it.
    if(flags&1){V3 actual=R.value<V3>(R.call(U.getVelocity,self));if(length(flat(actual))<length(mv.velocity)*.5f)mv.velocity=flat(actual);}
    return flags;
}
static void afterMovement(Obj self){
    // The mod's own jump must not be read by Granny as a fall.
    if(mv.jumping){float credited=std::max(0.f,mv.air-mv.budget);R.set(self,U.fps,"timeInAir",credited);}
}

// ---------------------------------------------------------------- tick and hooks
static void handleMenuActions(){
    int id=buyInput.exchange(-1);
    if(id>=0&&id<WEAPON_COUNT){
        if(combat.owned[id]&&(weapons[id].kind<8||combat.ammo[id]>0)){selectWeapon(id);notify(std::string("Выбрано: ")+weapons[id].name,1.5f);}
        else if(combat_buy(&combat,weapons,WEAPON_COUNT,id)){if(weapons[id].kind<8)lastGun=id;scoped=burstMode=false;burstLeft=0;weaponModel();notify(std::string("Куплено: ")+weapons[id].name);sound(6,.5f);}
        else notify("Недостаточно денег");
    }
    id=selectInput.exchange(-1);if(id>=0)selectWeapon(id);
    int armor=armorInput.exchange(0);
    if(armor){
        int price=armor==2?(combat.armor>=100?350:1000):650;
        if(combat.armor>=100&&(armor==1||combat.helmet))notify("Броня уже полная");
        else if(combat.money>=price){combat.money-=price;combat.armor=100;if(armor==2)combat.helmet=1;notify(armor==2?"Kevlar + Helmet":"Kevlar Vest");sound(6,.5f);}
        else notify("Недостаточно денег");
    }
    int bots=spawnInput.exchange(0);
    if(bots==1){botsEnabled=true;if(visualsReady)spawnBots();}
    if(bots==2){botsEnabled=false;removeBots();notify("Боты убраны");}
}
static void tick(Obj self){
    if(paused){fireInput=0;return;}
    if(player&&self!=player&&U.alive(player))return;
    now+=stepDt;
    if(self!=player)adoptController(self);
    applyFrameRate(false);
    nativeUI.ensure();
    nativeUI.readInput();
    handleMenuActions();
    prepareWorld();
    bool caught=R.field<bool>(self,U.fps,"playerCaught");
    if(caught){
        if(!wasCaught){wasCaught=true;fireInput=0;nativeUI.closeShop();resetMovement();}
        nativeUI.update(false);return;
    }
    if(wasCaught){wasCaught=false;combat.health=100;deathHandled=false;}
    if(!worldReady){nativeUI.update(true);return;}
    afterMovement(self);
    const Weapon &w=weapons[combat.weapon];
    if(altInput.exchange(0)){if(w.scoped)scoped=!scoped;else if(w.burst){burstMode=!burstMode;notify(burstMode?"Режим: очередь":"Режим: одиночный",1.2f);}else notify("У этого оружия нет прицела",1.2f);}
    if(cameraObject&&U.setFov){float fov=scoped?(w.kind==3?baseFov*.35f:baseFov*.6f):baseFov;R.call(U.setFov,cameraObject,{&fov});}
    if(jumpInput.exchange(0))mv.jumpBuffer=now;
    if(reloadInput.exchange(0))combat_reload(&combat,weapons);
    combat_tick(&combat,weapons,stepDt);
    bool held=fireInput.load()&&!nativeUI.shopOpen;
    if(burstMode&&w.burst){
        if(held&&!combat.trigger_down&&burstLeft==0)burstLeft=3;
        if(burstLeft>0&&combat.cooldown<=0&&combat.reload_left<=0){combat.trigger_down=0;if(combat_fire(&combat,weapons,1)){shot();--burstLeft;combat.cooldown=burstLeft?0.085f:.3f;}else burstLeft=0;}
        combat.trigger_down=held;
    }else if(combat_fire(&combat,weapons,held))shot();
    if(visualsReady){updateBots();updateGrenades(stepDt);}
    for(auto &e:effects)if(e.root&&now>e.expires)U.active(e.root,false);
    if(muzzle&&muzzleUntil>0&&now>muzzleUntil){U.active(muzzle,false);muzzleUntil=0;}
    recoilPitch=std::max(0.f,recoilPitch-stepDt*7);recoilYaw*=std::max(0.f,1-stepDt*7);
    if(U.alive(aimTransform)){V3 kick{-recoilPitch,recoilYaw,0};R.call(U.rotate,aimTransform,{&kick});lastPitch=recoilPitch;lastYaw=recoilYaw;}
    if(U.alive(gunTransform)){
        float speed=length(flat(mv.velocity)),bob=std::sin(now*9)*.006f*clamp(speed/4,0,1),sway=std::cos(now*4.5f)*.004f*clamp(speed/4,0,1);
        float drop=mv.grounded?0:clamp(mv.vy*.006f,-.02f,.02f);
        U.local(gunTransform,{(scoped?.06f:.22f)+sway,-.2f+bob-drop,.42f-recoilPitch*.003f});
        U.euler(gunTransform,{combat.reload_left>0?25.f:-recoilPitch*.5f,0,combat.reload_left>0?-15.f:0});
        if(scoped&&w.kind==3)U.active(gun,false);else U.active(gun,true);
    }
    flash=std::max(0.f,flash-stepDt*.35f);hurt=std::max(0.f,hurt-stepDt*1.4f);hitMarker=std::max(0.f,hitMarker-stepDt);
    if(combat.health<=0&&!deathHandled){deathHandled=true;fireInput=0;notify("Ты убит. Новый день — новый раунд.",4);if(U.playerDeath)R.call(U.playerDeath,self);}
    nativeUI.update(true);
}
static void unityReady(){
    static bool recorded=false;if(recorded)return;recorded=true;
    if(!readyFile.empty()){FILE *file=fopen(readyFile.c_str(),"w");if(file){fprintf(file,"Unity scene callback pid=%d\n",getpid());fclose(file);}}
    log("Unity managed scene callback reached; automatic startup acknowledged");
}
using FixedUpdate=void(*)(Obj,const Method *);static FixedUpdate originalFixed,originalMenu;
static bool runtimeReady=false,runtimeAttempted=false;
static void hookedMenu(Obj self,const Method *method){
    originalMenu(self,method);unityReady();
    log("Main menu initialized; native mod waits for gameplay FixedUpdate");
}
static void hookedFixed(Obj self,const Method *method){
    static bool first=true;if(first){first=false;log("First FPSControllerNEW.FixedUpdate callback");}
    if(inTick){originalFixed(self,method);return;}
    inTick=true;
    bool ours=runtimeReady&&self==player;
    if(ours&&U.alive(aimTransform)&&(lastPitch!=0||lastYaw!=0)){V3 undo{lastPitch,-lastYaw,0};R.call(U.rotate,aimTransform,{&undo});lastPitch=lastYaw=0;}
    if(runtimeReady)stepDt=clamp(R.value<float>(R.call(U.fixedDelta)),.001f,.1f);
    moveOwner=ours?self:nullptr;
    originalFixed(self,method);
    moveOwner=nullptr;
    unityReady();
    if(!runtimeAttempted){
        runtimeAttempted=true;TraceScope trace("bind managed API on Unity gameplay thread");
        runtimeReady=R.init()&&U.bind();
        if(runtimeReady)log("Managed API bound after original gameplay callback");
    }
    if(runtimeReady)tick(self);
    inTick=false;
}
struct LibraryTarget {uintptr_t base=0;bool buildMatches=false,fixedExecutable=false,menuExecutable=false,moveExecutable=false;};
static int inspectLibrary(dl_phdr_info *info,size_t,void *context){
    if(!info->dlpi_name)return 0;
    const char *name=strrchr(info->dlpi_name,'/');name=name?name+1:info->dlpi_name;
    if(strcmp(name,"libil2cpp.so"))return 0;
    auto &found=*static_cast<LibraryTarget*>(context);found.base=info->dlpi_addr;
    for(int i=0;i<info->dlpi_phnum;i++){
        const auto &header=info->dlpi_phdr[i];
        if(header.p_type==PT_LOAD&&(header.p_flags&PF_X)){
            auto inside=[&](uintptr_t rva){return rva>=header.p_vaddr&&rva+16<=header.p_vaddr+header.p_memsz;};
            found.fixedExecutable|=inside(target::fixedUpdate);found.menuExecutable|=inside(target::menuStart);found.moveExecutable|=inside(target::characterMove);
        }
        if(header.p_type!=PT_NOTE||header.p_memsz>1024*1024)continue;
        const uint8_t *notes=(const uint8_t*)(info->dlpi_addr+header.p_vaddr);size_t offset=0;
        while(offset+sizeof(ElfW(Nhdr))<=header.p_memsz){
            ElfW(Nhdr) note;memcpy(&note,notes+offset,sizeof(note));offset+=sizeof(note);
            size_t names=(size_t(note.n_namesz)+3)&~size_t(3),desc=(size_t(note.n_descsz)+3)&~size_t(3);
            if(names>header.p_memsz-offset||desc>header.p_memsz-offset-names)break;
            if(note.n_type==NT_GNU_BUILD_ID&&note.n_namesz==4&&!memcmp(notes+offset,"GNU",4)&&note.n_descsz==sizeof(target::buildId))
                found.buildMatches=!memcmp(notes+offset+names,target::buildId,sizeof(target::buildId));
            offset+=names+desc;
        }
    }
    return 1;
}
static void boot(){
    log("BOOT waiting for IL2CPP ELF; managed APIs are not called here");
    for(int i=0;i<240;i++){
        R.handle=dlopen("libil2cpp.so",RTLD_NOW|RTLD_NOLOAD);
        if(R.handle)break;usleep(250000);
    }
    if(!R.handle){setStatus("Игра не загрузила Unity за 60 секунд");return;}
    TraceScope trace("install verified gameplay hooks without managed VM access");
    LibraryTarget library;dl_iterate_phdr(inspectLibrary,&library);
    if(!library.base||!library.buildMatches||!library.fixedExecutable||!library.menuExecutable){setStatus("Мод не подключён: версия IL2CPP отличается от Granny 1.8.12");return;}
    void *address=(void*)(library.base+target::fixedUpdate),*menuAddress=(void*)(library.base+target::menuStart),*moveAddress=(void*)(library.base+target::characterMove);
    if(memcmp(address,target::fixedBytes,sizeof(target::fixedBytes))||memcmp(menuAddress,target::menuBytes,sizeof(target::menuBytes))){setStatus("Мод не подключён: код контроллера отличается от проверенного");return;}
    log("BOOT original ELF build ID and gameplay entry points verified");
    int hookInit=shadowhook_init(SHADOWHOOK_MODE_UNIQUE,false);
    if(hookInit!=0){setStatus(std::string("Ошибка подключения: ")+shadowhook_to_errmsg(hookInit));return;}
    if(!shadowhook_hook_func_addr(address,(void *)hookedFixed,(void **)&originalFixed)){
        setStatus(std::string("Не удалось подключить игровой цикл: ")+shadowhook_to_errmsg(shadowhook_get_errno()));return;
    }
    if(!shadowhook_hook_func_addr(menuAddress,(void*)hookedMenu,(void**)&originalMenu))log("Menu startup acknowledgement hook unavailable");
    if(library.moveExecutable&&!memcmp(moveAddress,target::moveBytes,sizeof(target::moveBytes))){
        if(shadowhook_hook_func_addr(moveAddress,(void*)hookedMove,(void**)&originalMove))log("BOOT movement hook installed on CharacterController.Move");
        else log(std::string("Movement hook unavailable: ")+shadowhook_to_errmsg(shadowhook_get_errno()));
    }else log("Movement hook skipped: CharacterController.Move differs from the verified build");
    setStatus("Подключение установлено. Оружие и боты включатся при начале игры.");
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm,void *){jvm=vm;return JNI_VERSION_1_6;}
extern "C" JNIEXPORT void JNICALL Java_org_modlab_granny_ModOverlay_nativeStart(JNIEnv *env,jclass cls,jstring path,jint mode,jstring token){
    if(started.exchange(true))return;
    const char *p=env->GetStringUTFChars(path,nullptr);std::string directory=p;env->ReleaseStringUTFChars(path,p);
    std::string filename=directory+"/granny-csgo.log";
    if(token){const char *value=env->GetStringUTFChars(token,nullptr);std::string id=value;env->ReleaseStringUTFChars(token,value);if(id.size()==36&&id.find_first_not_of("0123456789abcdef-")==std::string::npos)readyFile=directory+"/unity-ready-"+id;}
    logFile=fopen(filename.c_str(),"a");startupFile=fopen((directory+"/granny-csgo-startup.log").c_str(),"w");
    overlay=(jclass)env->NewGlobalRef(cls);soundCallback=env->GetStaticMethodID(cls,"playShot","(IF)V");
    botsEnabled=mode>=2;
    log("Granny Tactical Lab iteration " MOD_ITERATION " / Granny 1.8.12 / arm64-v8a / automatic mode="+std::to_string(mode));
    std::thread(boot).detach();
}
extern "C" JNIEXPORT void JNICALL Java_org_modlab_granny_ModOverlay_nativeAction(JNIEnv *,jclass,jint action,jint value){
    switch(action){case 0:fireInput=value;break;case 1:reloadInput=1;break;case 2:buyInput=value;break;case 3:selectInput=value;break;case 4:altInput=1;break;case 5:jumpInput=1;break;case 6:spawnInput=value?value:1;break;case 7:armorInput=value?value:1;break;case 8:paused=value;fireInput=0;break;}
}
