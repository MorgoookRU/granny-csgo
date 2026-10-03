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
#include <vector>
#include "shadowhook.h"
#include "combat.h"
#include "weapons_generated.h"
#include "target_layout.h"

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
static float clamp(float v,float a,float b){return std::max(a,std::min(v,b));}

static JavaVM *jvm;
static jclass overlay;
static jmethodID soundCallback;
static std::mutex logMutex, hudMutex;
static FILE *logFile;
static std::string readyFile;
static std::string bootStatus="Подключение мода…", hud="{}";
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
}
TraceScope::TraceScope(const char *stage){++traceDepth;log(std::string("STAGE ")+stage);}
static void setStatus(const std::string &s){
    {std::lock_guard<std::mutex> lock(hudMutex);bootStatus=s;}
    log(s);
}
static std::string escape(const std::string &s){
    std::string o;
    for(char c:s){if(c=='"'||c=='\\')o+='\\';if(c=='\n')o+="\\n";else if((unsigned char)c>=32)o+=c;}
    return o;
}

struct Runtime {
    void *handle=nullptr;
    void *(*domain_get)();
    const void **(*domain_get_assemblies)(void *,size_t *);
    const void *(*assembly_get_image)(const void *);
    Klass *(*class_from_name)(const void *,const char *,const char *);
    Klass *(*class_get_parent)(Klass *);
    Klass *(*object_get_class)(Obj);
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
    void (*field_get_value)(Obj,Field *,void *);
    void (*field_set_value)(Obj,Field *,void *);
    Obj (*runtime_invoke)(const Method *,Obj,void **,Obj *);
    void *(*object_unbox)(Obj);
    Obj (*object_new)(Klass *);
    Array *(*array_new)(Klass *,uintptr_t);
    Obj (*string_new)(const char *);
    void *(*thread_attach)(void *);
    void (*thread_detach)(void *);
    void (*gc_write)(Obj,void **,Obj);
    uint32_t (*gchandle_new)(Obj,bool);
    void (*gchandle_free)(uint32_t);
    void (*free_mem)(void *);
    std::unordered_map<std::string,Klass *> classes;
    std::unordered_map<std::string,const Method *> methods;
    std::vector<uint32_t> roots;
    Klass *unityObject=nullptr;
    const Method *nativeAlive=nullptr;
    int errors=0;

    bool init(){
#define BIND(member,symbol) member=reinterpret_cast<decltype(member)>(dlsym(handle,symbol));if(!member){setStatus(std::string("Нет API: ")+symbol);return false;}
        BIND(domain_get,"il2cpp_domain_get")
        BIND(domain_get_assemblies,"il2cpp_domain_get_assemblies")
        BIND(assembly_get_image,"il2cpp_assembly_get_image")
        BIND(class_from_name,"il2cpp_class_from_name")
        BIND(class_get_parent,"il2cpp_class_get_parent")
        BIND(object_get_class,"il2cpp_object_get_class")
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
        BIND(field_get_value,"il2cpp_field_get_value")
        BIND(field_set_value,"il2cpp_field_set_value")
        BIND(runtime_invoke,"il2cpp_runtime_invoke")
        BIND(object_unbox,"il2cpp_object_unbox")
        BIND(object_new,"il2cpp_object_new")
        BIND(array_new,"il2cpp_array_new")
        BIND(string_new,"il2cpp_string_new")
        BIND(thread_attach,"il2cpp_thread_attach")
        BIND(thread_detach,"il2cpp_thread_detach")
        BIND(gc_write,"il2cpp_gc_wbarrier_set_field")
        BIND(gchandle_new,"il2cpp_gchandle_new")
        BIND(gchandle_free,"il2cpp_gchandle_free")
        BIND(free_mem,"il2cpp_free")
#undef BIND
        return true;
    }
    Klass *klass(const char *ns,const char *name){
        std::string key=std::string(ns)+"."+name;
        auto f=classes.find(key);if(f!=classes.end())return f->second;
        size_t count=0;auto assemblies=domain_get_assemblies(domain_get(),&count);
        for(size_t i=0;i<count;i++){
            Klass *c=class_from_name(assembly_get_image(assemblies[i]),ns,name);
            if(c){classes[key]=c;return c;}
        }
        return nullptr;
    }
    const Method *method(Klass *c,const char *name,std::initializer_list<const char *> signature){
        if(!c)return nullptr;
        std::string key=std::to_string((uintptr_t)c)+":"+name;
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
        methods[key]=nullptr;log(std::string("Unavailable method: ")+key);return nullptr;
    }
    Obj call(const Method *m,Obj self=nullptr,std::initializer_list<void *> args={}){
        if(!m)return nullptr;
        uint32_t impl=0;bool instance=(method_get_flags(m,&impl)&0x10)==0;
        if(instance&&(!self||!class_is_assignable_from(method_get_class(m),object_get_class(self)))){
            if(errors++<30)log(std::string("Skipped invalid receiver: ")+method_get_name(m));return nullptr;
        }
        if(instance&&nativeAlive&&unityObject&&strcmp(method_get_name(m),".ctor")&&class_is_assignable_from(unityObject,object_get_class(self))){
            if(!value<bool>(call(nativeAlive,nullptr,{self}))){
                if(errors++<30)log(std::string("Skipped destroyed Unity object: ")+method_get_name(m));return nullptr;
            }
        }
        if(traceDepth)log(std::string("CALL ")+method_get_name(m));
        std::vector<void *> params(args);
        Obj exception=nullptr;Obj result=runtime_invoke(m,self,params.empty()?nullptr:params.data(),&exception);
        if(exception){if(errors++<30){char message[2048]{};format_exception(exception,message,sizeof(message));log(std::string("Unity exception in ")+method_get_name(m)+": "+message);}return nullptr;}
        return result;
    }
    template<class T>T value(Obj o){
        T v{};if(!o)return v;Klass *c=object_get_class(o);uint32_t alignment=0;
        if(!c||!class_is_valuetype(c)||class_value_size(c,&alignment)!=(int)sizeof(v)){
            if(errors++<30)log("Skipped unexpected boxed value layout");return v;
        }
        void *raw=object_unbox(o);if(raw)memcpy(&v,raw,sizeof(v));return v;
    }
    template<class T>T field(Obj o,Klass *c,const char *name){T v{};auto f=class_get_field_from_name(c,name);if(o&&f)field_get_value(o,f,&v);return v;}
    template<class T>void set(Obj o,Klass *c,const char *name,T v){auto f=class_get_field_from_name(c,name);if(o&&f)field_set_value(o,f,&v);}
    Obj type(Klass *c){return c?type_get_object(class_get_type(c)):nullptr;}
    Obj pin(Obj o){if(o)roots.push_back(gchandle_new(o,false));return o;}
    void clearRoots(){for(auto h:roots)gchandle_free(h);roots.clear();}
} R;

struct Unity {
    Klass *object,*gameObject,*component,*transform,*renderer,*material,*mesh,*meshFilter,*meshRenderer;
    Klass *camera,*time,*physics,*hit,*nav,*agent,*capsule,*vec,*integer,*fps,*granny;
    const Method *objectAlive,*destroy,*findObjects,*goCtor,*getTransform,*componentTransform,*componentGO;
    const Method *addComponent,*getComponent,*setActive,*setLayer,*getPosition,*setPosition,*getForward,*getRight;
    const Method *getParent,*setParent,*setLocalPosition,*setLocalScale,*setEuler,*rotate,*lookAt;
    const Method *meshCtor,*setVertices,*setNormals,*setTriangles,*setMesh,*getMaterial,*setMaterial,*matCtor,*matColor,*matTexture;
    const Method *cameraMain,*setFov,*setClip,*fixedDelta,*raycast,*hitTransform,*findById,*sampleNav,*setDestination,*onNav,*agentSpeed,*agentStop;
    const Method *capsuleHeight,*capsuleRadius,*capsuleCenter,*getVelocity,*playerDeath;
    bool bind(){
#define CLASS(member,ns,name) member=R.klass(ns,name);if(!member){setStatus("Нет класса " name);return false;}
        CLASS(object,"UnityEngine","Object") CLASS(gameObject,"UnityEngine","GameObject")
        CLASS(component,"UnityEngine","Component") CLASS(transform,"UnityEngine","Transform")
        CLASS(renderer,"UnityEngine","Renderer") CLASS(material,"UnityEngine","Material")
        CLASS(mesh,"UnityEngine","Mesh") CLASS(meshFilter,"UnityEngine","MeshFilter")
        CLASS(meshRenderer,"UnityEngine","MeshRenderer") CLASS(camera,"UnityEngine","Camera")
        CLASS(time,"UnityEngine","Time") CLASS(physics,"UnityEngine","Physics")
        CLASS(hit,"UnityEngine","RaycastHit") CLASS(nav,"UnityEngine.AI","NavMesh")
        CLASS(agent,"UnityEngine.AI","NavMeshAgent") CLASS(capsule,"UnityEngine","CapsuleCollider")
        CLASS(vec,"UnityEngine","Vector3") CLASS(integer,"System","Int32")
        CLASS(fps,"","FPSControllerNEW") CLASS(granny,"","EnemyAIGranny")
#undef CLASS
#define M(dst,cl,n,...) dst=R.method(cl,n,{__VA_ARGS__})
        M(objectAlive,object,"op_Implicit","UnityEngine.Object");
        M(destroy,object,"Destroy","UnityEngine.Object","System.Single");
        M(findObjects,object,"FindObjectsOfType","System.Type");
        M(goCtor,gameObject,".ctor","System.String");
        M(getTransform,gameObject,"get_transform");M(componentTransform,component,"get_transform");M(componentGO,component,"get_gameObject");
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
        M(matCtor,material,".ctor","UnityEngine.Material");M(matColor,material,"set_color","UnityEngine.Color");M(matTexture,material,"set_mainTexture","UnityEngine.Texture");
        M(cameraMain,camera,"get_main");M(setFov,camera,"set_fieldOfView","System.Single");M(setClip,camera,"set_nearClipPlane","System.Single");M(fixedDelta,time,"get_fixedDeltaTime");
        M(raycast,physics,"Raycast","UnityEngine.Vector3","UnityEngine.Vector3","UnityEngine.RaycastHit&","System.Single","System.Int32","UnityEngine.QueryTriggerInteraction");
        M(hitTransform,hit,"get_transform");M(sampleNav,nav,"SamplePosition","UnityEngine.Vector3","UnityEngine.AI.NavMeshHit&","System.Single","System.Int32");
        M(findById,object,"FindObjectFromInstanceID","System.Int32");
        M(setDestination,agent,"SetDestination","UnityEngine.Vector3");M(onNav,agent,"get_isOnNavMesh");M(agentSpeed,agent,"set_speed","System.Single");M(agentStop,agent,"set_stoppingDistance","System.Single");
        M(capsuleHeight,capsule,"set_height","System.Single");M(capsuleRadius,capsule,"set_radius","System.Single");M(capsuleCenter,capsule,"set_center","UnityEngine.Vector3");
        M(getVelocity,R.klass("UnityEngine","CharacterController"),"get_velocity");M(playerDeath,fps,"PlayerGetsCaught");
#undef M
        const Method *required[]={objectAlive,destroy,findObjects,goCtor,getTransform,componentTransform,componentGO,addComponent,setActive,getPosition,setPosition,getForward,getRight,setParent,setLocalPosition,setLocalScale,setEuler,meshCtor,setVertices,setNormals,setTriangles,setMesh,getMaterial,setMaterial,matCtor,matColor,cameraMain,raycast,hitTransform};
        for(auto m:required)if(!m){setStatus("Ошибка подключения Unity. Открой журнал мода.");return false;}
        R.unityObject=object;R.nativeAlive=objectAlive;
        return true;
    }
    bool alive(Obj o){return o&&R.value<bool>(R.call(objectAlive,nullptr,{o}));}
    Obj trans(Obj o,bool component=false){return o?R.call(component?componentTransform:getTransform,o):nullptr;}
    V3 pos(Obj t){return R.value<V3>(R.call(getPosition,t));}
    V3 forward(Obj t){return R.value<V3>(R.call(getForward,t));}
    V3 right(Obj t){return R.value<V3>(R.call(getRight,t));}
    void position(Obj t,V3 p){R.call(setPosition,t,{&p});}
    void local(Obj t,V3 p){R.call(setLocalPosition,t,{&p});}
    void scale(Obj t,V3 p){R.call(setLocalScale,t,{&p});}
    void euler(Obj t,V3 p){R.call(setEuler,t,{&p});}
    void active(Obj o,bool b){if(o)R.call(setActive,o,{&b});}
    void remove(Obj o){if(alive(o)){float delay=0;R.call(destroy,nullptr,{o,&delay});}}
    Obj newGO(const char *name){Obj o=R.pin(R.object_new(gameObject));R.call(goCtor,o,{R.string_new(name)});return o;}
    Obj add(Obj go,Klass *k){return R.pin(R.call(addComponent,go,{R.type(k)}));}
    Obj get(Obj go,Klass *k){return R.call(getComponent,go,{R.type(k)});}
    void parent(Obj t,Obj p){bool world=false;R.call(setParent,t,{p,&world});}
    Array *objects(Klass *k){return (Array *)R.call(findObjects,nullptr,{R.type(k)});}
    bool ray(V3 start,V3 dir,float distance,Hit &h){
        dir=normal(dir);int mask=-1,ignore=1;h={};
        return R.value<bool>(R.call(raycast,nullptr,{&start,&dir,&h,&distance,&mask,&ignore}));
    }
    Obj hitTrans(Hit &h){
        if(!h.collider||!findById)return nullptr;
        Obj collider=R.call(findById,nullptr,{&h.collider});return collider?trans(collider,true):nullptr;
    }
    V3 navPoint(V3 p){NavHit h{};float range=3;int mask=-1;if(R.value<bool>(R.call(sampleNav,nullptr,{&p,&h,&range,&mask})))return h.point;return p;}
    bool childOf(Obj t,Obj root){for(int i=0;t&&i<16;i++){if(t==root)return true;t=R.call(getParent,t);}return false;}
} U;

static Combat combat;
static Obj player, playerTransform, cameraObject, cameraTransform, character, gun, gunTransform, cubeMesh;
static Obj mats[8];
static std::vector<Obj> worldRoots;
static V3 waypoints[16];static int waypointCount=0;
static float now=0,recoilPitch=0,recoilYaw=0,lastPitch=0,lastYaw=0,flash=0,grannyHP=100,grannyReset=0;
static bool scoped=false,burstMode=false,wasCaught=false,inTick=false,botsEnabled=false;
static void resetNativeUI();
static void updateNativeUI(bool inGame);
static void inputNativeUI();
static float sceneRetry=0;
static int burstLeft=0;static float hudTimer=0;
static std::string notification="Открой магазин, выбери оружие и начни игру",killfeed;
struct Bot {Obj root=nullptr,transform=nullptr,agent=nullptr,head=nullptr,legs[2]{};float hp=100,armor=50,nextShot=0,respawn=0,blind=0,lastSeen=-100;V3 target{};int weapon=17;};
static Bot bots[4];
struct Effect {Obj root=nullptr,transform=nullptr;float expires=0;};static Effect effects[12];static int effectIndex=0;
struct Grenade {Obj root=nullptr,transform=nullptr,renderer=nullptr;V3 velocity{},position{};int kind=8;float age=0,remaining=0,tick=0;bool exploded=false;};static Grenade grenades[8];

static void sound(int kind,float volume=1.f){
    if(!jvm||!overlay||!soundCallback)return;
    JNIEnv *env=nullptr;bool detach=false;
    if(jvm->GetEnv((void **)&env,JNI_VERSION_1_6)!=JNI_OK){if(jvm->AttachCurrentThread(&env,nullptr)!=JNI_OK)return;detach=true;}
    env->CallStaticVoidMethod(overlay,soundCallback,kind,volume);
    if(env->ExceptionCheck())env->ExceptionClear();
    if(detach)jvm->DetachCurrentThread();
}
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
    if(!vertices||!ns||!ts){setStatus("Не удалось выделить данные модели");return nullptr;}
    memcpy(vertices->data,v,sizeof(v));memcpy(ns->data,normals,sizeof(normals));memcpy(ts->data,triangles,sizeof(triangles));
    Obj mesh=R.pin(R.object_new(U.mesh));R.call(U.meshCtor,mesh);
    R.call(U.setVertices,mesh,{vertices});R.call(U.setNormals,mesh,{ns});R.call(U.setTriangles,mesh,{ts});return mesh;
}
static bool makeMaterials(){
    TraceScope trace("scene materials");
    auto renderers=U.objects(U.renderer);Obj source=nullptr;
    if(renderers&&renderers->length<10000){auto objects=(Obj *)renderers->data;for(size_t i=0;i<renderers->length;i++){source=R.call(U.getMaterial,objects[i]);if(U.alive(source))break;}}
    if(!source){notification="Не найден материал сцены";return false;}
    const Color colors[]={{.13f,.15f,.17f,1},{.42f,.2f,.07f,1},{.65f,.7f,.75f,1},{.15f,.3f,.45f,1},{.2f,.25f,.13f,1},{1,.68f,.12f,1},{.5f,.5f,.5f,1},{1,.22f,.025f,1}};
    for(int i=0;i<8;i++){mats[i]=R.pin(R.object_new(U.material));R.call(U.matCtor,mats[i],{source});R.call(U.matTexture,mats[i],{nullptr});Color c=colors[i];R.call(U.matColor,mats[i],{&c});}
    cubeMesh=makeMesh();return cubeMesh;
}
static Obj box(Obj parent,const char *name,V3 position,V3 size,int material,int layer=0){
    Obj root=U.newGO(name),t=R.pin(U.trans(root));if(parent)U.parent(t,parent);
    U.local(t,position);U.scale(t,size);R.call(U.setLayer,root,{&layer});
    Obj filter=U.add(root,U.meshFilter),renderer=U.add(root,U.meshRenderer);
    R.call(U.setMesh,filter,{cubeMesh});R.call(U.setMaterial,renderer,{mats[material]});return root;
}
static void weaponModel(){
    TraceScope trace("weapon model replacement");
    if(!cubeMesh||!U.alive(cameraTransform)){log("Weapon model deferred: scene is not ready");return;}
    Obj previous=gun;
    Obj previousTransform=gunTransform;
    gun=U.newGO("CSGO_Weapon");worldRoots.push_back(gun);gunTransform=R.pin(U.trans(gun));U.parent(gunTransform,cameraTransform);
    if(!U.alive(gun)||!U.alive(gunTransform)){gun=previous;gunTransform=previousTransform;notification="Не удалось создать оружие";return;}
    struct Replace {
        Obj old;
        ~Replace(){if(old){U.remove(old);worldRoots.erase(std::remove(worldRoots.begin(),worldRoots.end(),old),worldRoots.end());}}
    } replacement{previous};
    U.local(gunTransform,{.24f,-.2f,.46f});
    const Weapon &w=weapons[combat.weapon];float body=w.kind==0?.2f:(w.kind==3?.47f:.34f);
    if(w.kind>=8){box(gunTransform,"Grenade",{0,-.015f,.05f},{.07f,.12f,.07f},w.kind==11?1:4,2);box(gunTransform,"Pin",{0,.055f,.05f},{.03f,.02f,.035f},2,2);return;}
    if(w.kind==6){box(gunTransform,"Knife",{0,0,.15f},{.026f,.008f,.3f},2,2);box(gunTransform,"Handle",{0,0,-.065f},{.04f,.035f,.12f},0,2);return;}
    box(gunTransform,"Receiver",{0,0,body*.15f},{.065f,.065f,body},0,2);
    box(gunTransform,"Barrel",{0,.022f,body*.75f},{.026f,.025f,w.silenced?.22f:.15f},w.silenced?0:2,2);
    box(gunTransform,"Grip",{0,-.065f,-body*.24f},{.043f,.13f,.058f},combat.weapon==17?1:0,2);
    box(gunTransform,"Magazine",{0,-.055f,body*.08f},{.038f,w.kind==5?.1f:.13f,w.kind==5?.12f:.045f},0,2);
    if(w.kind>0&&w.kind<6){box(gunTransform,"Stock",{0,-.015f,-body*.7f},{.045f,.075f,.15f},combat.weapon==17?1:0,2);box(gunTransform,"Foregrip",{0,-.013f,body*.48f},{.06f,.075f,.14f},combat.weapon==17?1:0,2);}
    if(w.scoped){box(gunTransform,"Optic",{0,.075f,.025f},{.06f,.055f,w.kind==3?.18f:.07f},0,2);box(gunTransform,"Lens",{0,.075f,w.kind==3?.12f:.065f},{.045f,.04f,.006f},3,2);}
    if(combat.weapon==9){Obj second=box(gunTransform,"Second pistol",{-.34f,0,.025f},{.065f,.065f,.22f},0,2);box(U.trans(second),"Second grip",{0,-.8f,-.2f},{.7f,1.8f,.3f},0,2);}
}
static V3 spawnPosition(int i){
    V3 p=waypointCount?waypoints[(i*3+2)%waypointCount]:U.pos(playerTransform)+V3{float(4+i),0,3};
    return U.navPoint(p);
}
static void spawnBots(){
    TraceScope trace("spawn four bots");
    botsEnabled=true;
    for(int i=0;i<4;i++){
        Bot &b=bots[i];if(U.alive(b.root))U.remove(b.root);b={};b.weapon=(int[]){17,18,11,7}[i];
        b.root=U.newGO("CSGO_Bot");U.active(b.root,false);worldRoots.push_back(b.root);b.transform=R.pin(U.trans(b.root));U.position(b.transform,spawnPosition(i));
        box(b.transform,"Torso",{0,1.05f,0},{.46f,.6f,.28f},i%2?4:3);
        b.head=R.pin(U.trans(box(b.transform,"Helmet",{0,1.58f,0},{.31f,.31f,.3f},0)));
        for(int j=0;j<2;j++){b.legs[j]=R.pin(U.trans(box(b.transform,"Leg",{j?.14f:-.14f,.43f,0},{.16f,.78f,.19f},0)));box(b.transform,"Arm",{j?.31f:-.31f,1.09f,.14f},{.14f,.45f,.17f},i%2?4:3);}
        box(b.transform,"Rifle",{.2f,1.11f,.33f},{.07f,.075f,.48f},0);
        Obj collider=U.add(b.root,U.capsule);float height=1.85f,radius=.32f;V3 center{0,.925f,0};R.call(U.capsuleHeight,collider,{&height});R.call(U.capsuleRadius,collider,{&radius});R.call(U.capsuleCenter,collider,{&center});
        b.agent=U.add(b.root,U.agent);float speed=2.5f,stop=4;R.call(U.agentSpeed,b.agent,{&speed});R.call(U.agentStop,b.agent,{&stop});
        b.hp=100;b.armor=50;b.nextShot=now+6+i*.5f;b.target=spawnPosition(i+1);
        U.active(b.root,true);
    }
    notification="Добавлены 4 тестовых бота";
}
static bool resetWorld(Obj self){
    TraceScope trace("reset gameplay scene");
    sceneRetry=.5f;
    Obj readyCharacter=R.field<Obj>(self,U.fps,"character");
    Obj readyCamera=R.call(U.cameraMain);
    if(!U.alive(readyCharacter)||!U.alive(readyCamera)){log("Scene pending: player and camera not ready");return false;}
    resetNativeUI();
    for(auto root:worldRoots)U.remove(root);worldRoots.clear();R.clearRoots();
    player=R.pin(self);playerTransform=R.pin(U.trans(self,true));character=R.pin(R.field<Obj>(self,U.fps,"character"));
    cameraObject=R.pin(R.call(U.cameraMain));cameraTransform=R.pin(U.trans(cameraObject,true));
    combat_init(&combat,weapons,WEAPON_COUNT);now=0;hudTimer=0;flash=0;grannyHP=100;grannyReset=0;
    combat.owned[17]=1;combat.weapon=17;
    scoped=burstMode=wasCaught=false;burstLeft=0;recoilPitch=recoilYaw=lastPitch=lastYaw=0;
    gun=gunTransform=cubeMesh=nullptr;for(auto &b:bots)b={};for(auto &e:effects)e={};for(auto &g:grenades)g={};
    waypointCount=0;
    Obj grannyGO=R.field<Obj>(self,U.fps,"granny");Obj granny=grannyGO?U.get(grannyGO,U.granny):nullptr;
    if(granny){for(int i=1;i<=16;i++){std::string n="nav"+std::to_string(i);Obj t=R.field<Obj>(granny,U.granny,n.c_str());if(U.alive(t))waypoints[waypointCount++]=U.pos(t);}}
    if(!U.alive(cameraObject)||!makeMaterials()){notification="Ожидание камеры и материалов";return false;}
    float fov=75,clip=.04f;R.call(U.setFov,cameraObject,{&fov});R.call(U.setClip,cameraObject,{&clip});
    weaponModel();if(botsEnabled)spawnBots();setStatus("Мод подключён");log("Scene initialized; waypoints="+std::to_string(waypointCount));
    return true;
}
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
    b.hp-=damage;
    if(b.hp<=0){U.active(b.root,false);b.respawn=now+8;combat_reward(&combat,300);killfeed=std::string(weapons[combat.weapon].name)+(head?" • HEADSHOT":" • BOT");notification="Бот устранён: +$300";}
}
static Obj grannyObject(){Obj go=R.field<Obj>(player,U.fps,"granny");return U.alive(go)?U.get(go,U.granny):nullptr;}
static void hitDamage(Hit &h,float damage){
    Obj t=U.hitTrans(h);
    for(auto &b:bots)if(b.hp>0&&U.childOf(t,b.transform)){botDamage(b,damage,h.point.y-U.pos(b.transform).y>1.35f);return;}
    Obj ai=grannyObject();if(ai){Obj gt=U.trans(ai,true);if(U.childOf(t,gt)){
        grannyHP-=damage*(h.point.y-U.pos(gt).y>1.5f?4:1);
        if(grannyHP<=0&&now>grannyReset){R.call(R.method(U.granny,"grannyHitByGun",{}),ai);combat_reward(&combat,300);grannyReset=now+10;grannyHP=100;killfeed="GRANNY • +$300";}
    }}
}
static void tracer(V3 from,V3 to){
    Effect &e=effects[(effectIndex++)%12];
    if(!e.root){e.root=box(nullptr,"CSGO_Tracer",{},V3{.012f,.012f,1},5,2);e.transform=R.pin(U.trans(e.root));worldRoots.push_back(e.root);}
    U.active(e.root,true);U.position(e.transform,(from+to)*.5f);U.scale(e.transform,{.012f,.012f,length(to-from)});R.call(U.lookAt,e.transform,{&to});e.expires=now+.045f;
}
static void throwGrenade(int kind){
    Grenade *slot=nullptr;for(auto &g:grenades)if(!g.root){slot=&g;break;}
    if(!slot){notification="На карте уже 8 гранат";return;}
    *slot={};slot->kind=kind;slot->position=U.pos(cameraTransform)+U.forward(cameraTransform)*.45f;
    slot->velocity=U.forward(cameraTransform)*8+V3{0,3,0};
    slot->root=box(nullptr,"CSGO_Grenade",slot->position,{.12f,.18f,.12f},kind==11?1:4,2);
    slot->transform=R.pin(U.trans(slot->root));slot->renderer=U.get(slot->root,U.meshRenderer);worldRoots.push_back(slot->root);
}
static void shot(){
    const Weapon &w=weapons[combat.weapon];sound(std::min(w.kind,7),w.silenced?.28f:1.f);
    if(w.kind>=8){throwGrenade(w.kind);return;}
    V3 origin=U.pos(cameraTransform),forward=U.forward(cameraTransform),right=U.right(cameraTransform),up=cross(forward,right);
    V3 velocity=R.value<V3>(R.call(U.getVelocity,character));
    float spread=w.spread+(length(V3{velocity.x,0,velocity.z})>1?1.8f:0.f)+std::min(combat.shots*.12f,2.0f);
    if(!R.field<bool>(player,U.fps,"PlayerIsGrounded"))spread+=3;
    if(R.field<bool>(player,U.fps,"playerCrouch"))spread*=.6f;
    if(scoped)spread*=.25f;else if(w.kind==3)spread+=3;
    if(combat.weapon==33&&combat.shots>12)spread=.45f;
    float range=w.kind==6?2.2f:(w.kind==7?3.4f:150.f);
    for(int i=0;i<w.pellets;i++){
        V3 dir=normal(forward+right*((random01()-.5f)*spread*.0349f)+up*((random01()-.5f)*spread*.0349f));
        Hit h;V3 end=origin+dir*range;
        if(U.ray(origin,dir,range,h)){end=h.point;float damage=w.damage;if(w.kind==4)damage*=clamp(1-h.distance/22.f,.15f,1);hitDamage(h,damage);}
        if(w.kind<6)tracer(origin+forward*.3f,end);
    }
    recoilPitch=std::min(recoilPitch+w.kick,12.f);
    recoilYaw+=w.kick*.45f*std::sin(combat.shots*1.7f);
    notification=w.name;
}
static void radiusDamage(V3 p,float radius,float damage){
    for(auto &b:bots)if(b.hp>0){V3 target=U.pos(b.transform)+V3{0,1,0};float d=length(target-p);if(d<radius&&visible(p+V3{0,.15f,0},target,b.transform))botDamage(b,damage*(1-d/radius),false);}
    Obj ai=grannyObject();if(ai){V3 target=U.pos(U.trans(ai,true))+V3{0,1,0};if(length(target-p)<radius){grannyHP-=damage;if(grannyHP<=0&&now>grannyReset){R.call(R.method(U.granny,"grannyHitByGun",{}),ai);combat_reward(&combat,300);grannyReset=now+10;grannyHP=100;}}}
}
static void updateGrenades(float dt){
    for(auto &g:grenades)if(g.root){
        g.age+=dt;
        if(!g.exploded){
            g.velocity.y-=9.81f*dt;V3 step=g.velocity*dt;Hit hit;
            bool collision=U.ray(g.position,step,length(step)+.06f,hit);
            if(collision){g.position=hit.point+hit.normal*.08f;g.velocity=(g.velocity-hit.normal*(2*dot(g.velocity,hit.normal)))*.5f;}else g.position=g.position+step;
            U.position(g.transform,g.position);
            if(g.age<1.5f&&!(g.kind==11&&collision&&g.age>.2f))continue;
            g.exploded=true;
            if(g.kind==8){radiusDamage(g.position,6,140);U.scale(g.transform,{1.2f,1.2f,1.2f});R.call(U.setMaterial,g.renderer,{mats[5]});g.remaining=.12f;}
            if(g.kind==9){
                for(auto &b:bots)if(b.hp>0&&length(U.pos(b.transform)-g.position)<15&&visible(g.position,U.pos(b.transform)+V3{0,1.5f,0},b.transform))b.blind=now+4;
                V3 d=g.position-U.pos(cameraTransform);if(length(d)<15&&visible(U.pos(cameraTransform),g.position,nullptr))flash=clamp(dot(normal(d),U.forward(cameraTransform))*.8f+.2f,0,1);
                U.scale(g.transform,{.8f,.8f,.8f});R.call(U.setMaterial,g.renderer,{mats[2]});g.remaining=.08f;
            }
            if(g.kind==10){U.position(g.transform,g.position+V3{0,1,0});U.scale(g.transform,{3.8f,2.7f,3.8f});R.call(U.setMaterial,g.renderer,{mats[6]});g.remaining=18;}
            if(g.kind==11){U.scale(g.transform,{4,.14f,4});R.call(U.setMaterial,g.renderer,{mats[7]});g.remaining=7;}
            if(g.kind==12){g.remaining=12;}
        }else{
            g.remaining-=dt;g.tick-=dt;
            if(g.kind==11&&g.tick<=0){radiusDamage(g.position,2.5f,9);if(length(U.pos(playerTransform)-g.position)<2.5f)combat_damage(&combat,6);g.tick=.3f;}
            if(g.kind==12&&g.tick<=0){sound(2,.4f);g.tick=.4f;for(auto &b:bots)if(now-b.lastSeen>3)b.target=g.position;}
            if(g.remaining<=0){U.remove(g.root);g.root=nullptr;}
        }
    }
}
static void updateBots(){
    V3 target=U.pos(cameraTransform),ground=U.pos(playerTransform);
    for(int i=0;i<4;i++){
        Bot &b=bots[i];if(!U.alive(b.root))continue;
        if(b.hp<=0){if(now>=b.respawn){U.position(b.transform,spawnPosition(i));U.active(b.root,true);b.hp=100;b.armor=50;b.nextShot=now+1;}continue;}
        V3 p=U.pos(b.transform),eye=p+V3{0,1.5f,0};float dist=length(target-eye);
        bool seen=now>b.blind&&dist<25&&visible(eye,target,playerTransform);
        if(seen){if(now-b.lastSeen>.2f)b.nextShot=std::max(b.nextShot,now+.35f);b.lastSeen=now;b.target=ground;V3 flat=ground;flat.y=p.y;R.call(U.lookAt,b.transform,{&flat});}
        else if(now-b.lastSeen>6&&length(b.target-p)<2)b.target=spawnPosition(i+(int)now/10);
        if(R.value<bool>(R.call(U.onNav,b.agent)))R.call(U.setDestination,b.agent,{&b.target});
        float stride=seen&&dist<5?0:std::sin(now*8+i)*16;for(int j=0;j<2;j++)U.euler(b.legs[j],{j?stride:-stride,0,0});
        if(seen&&now>=b.nextShot&&combat.health>0){
            const Weapon &w=weapons[b.weapon];b.nextShot=now+std::max(.18f,60.f/w.rpm);
            tracer(eye,target);sound(std::min(w.kind,5),.16f);
            V3 v=R.value<V3>(R.call(U.getVelocity,character));
            float chance=clamp(.8f-dist*.018f-length(v)*.035f,.15f,.85f);
            if(random01()<chance){int damage=(int)(w.damage*.4f);if(random01()<.08f)damage*=2;combat_damage(&combat,damage);flash=std::max(flash,.12f);}
        }
    }
}
#include "unity_ui.inc"

static void makeHud(bool inGame){
    updateNativeUI(inGame);
    std::ostringstream s;std::string status;
    {std::lock_guard<std::mutex> lock(hudMutex);status=bootStatus;}
    s<<"{\"status\":\""<<escape(status)<<"\",\"game\":"<<(inGame?"true":"false")
     <<",\"weapon\":"<<combat.weapon<<",\"money\":"<<combat.money<<",\"health\":"<<combat.health
     <<",\"armor\":"<<combat.armor<<",\"ammo\":"<<combat.ammo[combat.weapon]<<",\"reserve\":"<<combat.reserve[combat.weapon]
     <<",\"kills\":"<<combat.kills<<",\"reload\":"<<(combat.reload_left>0?"true":"false")
     <<",\"scope\":"<<(scoped?"true":"false")<<",\"flash\":"<<flash
     <<",\"message\":\""<<escape(notification)<<"\",\"killfeed\":\""<<escape(killfeed)<<"\",\"owned\":[";
    for(int i=0;i<WEAPON_COUNT;i++){if(i)s<<',';s<<(combat.owned[i]?"true":"false");}s<<"]}";
    std::lock_guard<std::mutex> lock(hudMutex);hud=s.str();
}
static void tick(Obj self){
    if(paused){fireInput=0;return;}
    float dt=R.value<float>(R.call(U.fixedDelta));dt=clamp(dt,.001f,.1f);
    if(self!=player||!U.alive(cameraObject)){if(!resetWorld(self)){makeHud(false);return;}}
    else if(!cubeMesh){sceneRetry-=dt;if(sceneRetry<=0&&!resetWorld(self)){makeHud(false);return;}}
    if(!cameraTransform||!cubeMesh){makeHud(false);return;}
    now+=dt;
    bool caught=R.field<bool>(self,U.fps,"playerCaught");
    if(caught){wasCaught=true;fireInput=0;makeHud(false);return;}
    if(wasCaught){combat.health=100;combat.armor=100;wasCaught=false;if(botsEnabled)spawnBots();}
    inputNativeUI();
    int id=buyInput.exchange(-1);
    if(id>=0){if(combat_buy(&combat,weapons,WEAPON_COUNT,id)){scoped=burstMode=false;burstLeft=0;weaponModel();notification=std::string("Куплено: ")+weapons[id].name;}else notification="Недостаточно денег";}
    id=selectInput.exchange(-1);if(id>=0&&combat_select(&combat,WEAPON_COUNT,id)){scoped=burstMode=false;burstLeft=0;weaponModel();}
    if(armorInput.exchange(0)){if(combat.money>=1000){combat.money-=1000;combat.armor=100;notification="Броня: 100";}else notification="Недостаточно денег";}
    if(spawnInput.exchange(0))spawnBots();
    const Weapon &w=weapons[combat.weapon];
    if(altInput.exchange(0)){if(w.scoped)scoped=!scoped;else if(w.burst)burstMode=!burstMode;else notification="У этого оружия нет альтернативного режима";}
    float fov=scoped?(w.kind==3?25.f:42.f):75.f;R.call(U.setFov,cameraObject,{&fov});
    float speed=w.speed*(scoped?.7f:1.f);R.set(self,U.fps,"forwardSpeed",speed);R.set(self,U.fps,"backwardSpeed",speed*.8f);R.set(self,U.fps,"sidestepSpeed",speed);
    if(jumpInput.exchange(0)&&R.field<bool>(self,U.fps,"PlayerIsGrounded")){V3 v=R.field<V3>(self,U.fps,"velocity");v.y=4.6f;R.set(self,U.fps,"velocity",v);R.set(self,U.fps,"canJump",true);}
    if(reloadInput.exchange(0))combat_reload(&combat,weapons);
    combat_tick(&combat,weapons,dt);
    bool held=fireInput.load();
    if(burstMode&&w.burst){
        if(held&&!combat.trigger_down&&burstLeft==0)burstLeft=3;
        if(burstLeft>0&&combat.cooldown<=0&&combat.reload_left<=0){combat.trigger_down=0;if(combat_fire(&combat,weapons,1)){shot();--burstLeft;combat.cooldown=burstLeft?0.085f:.3f;}else burstLeft=0;}
        combat.trigger_down=held;
    }else if(combat_fire(&combat,weapons,held))shot();
    updateBots();updateGrenades(dt);
    for(auto &e:effects)if(e.root&&now>e.expires)U.active(e.root,false);
    recoilPitch=std::max(0.f,recoilPitch-dt*7);recoilYaw*=std::max(0.f,1-dt*7);
    V3 kick{-recoilPitch,recoilYaw,0};R.call(U.rotate,cameraTransform,{&kick});lastPitch=recoilPitch;lastYaw=recoilYaw;
    if(gunTransform){float bob=std::sin(now*7)*.004f;U.local(gunTransform,{scoped?.08f:.24f,-.2f+bob,.46f-recoilPitch*.002f});U.euler(gunTransform,{combat.reload_left>0?25.f:-recoilPitch*.4f,0,combat.reload_left>0?-15.f:0});}
    flash=std::max(0.f,flash-dt*.35f);
    if(combat.health<=0){R.call(U.playerDeath,self);fireInput=0;notification="Ты погиб. Начни следующий день.";}
    hudTimer-=dt;if(hudTimer<=0){makeHud(true);hudTimer=.1f;}
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
    if(runtimeReady&&self==player&&U.alive(cameraTransform)&&(lastPitch!=0||lastYaw!=0)){V3 undo{lastPitch,-lastYaw,0};R.call(U.rotate,cameraTransform,{&undo});lastPitch=lastYaw=0;}
    originalFixed(self,method);
    unityReady();
    if(!runtimeAttempted){
        runtimeAttempted=true;TraceScope trace("bind managed API on Unity gameplay thread");
        runtimeReady=R.init()&&U.bind();
        if(runtimeReady)log("Managed API bound after original gameplay callback");
    }
    if(runtimeReady)tick(self);inTick=false;
}
struct LibraryTarget {uintptr_t base=0;bool buildMatches=false,fixedExecutable=false,menuExecutable=false;};
static int inspectLibrary(dl_phdr_info *info,size_t,void *context){
    if(!info->dlpi_name)return 0;
    const char *name=strrchr(info->dlpi_name,'/');name=name?name+1:info->dlpi_name;
    if(strcmp(name,"libil2cpp.so"))return 0;
    auto &found=*static_cast<LibraryTarget*>(context);found.base=info->dlpi_addr;
    for(int i=0;i<info->dlpi_phnum;i++){
        const auto &header=info->dlpi_phdr[i];
        if(header.p_type==PT_LOAD&&(header.p_flags&PF_X)){
            auto inside=[&](uintptr_t rva){return rva>=header.p_vaddr&&rva+16<=header.p_vaddr+header.p_memsz;};
            found.fixedExecutable|=inside(target::fixedUpdate);found.menuExecutable|=inside(target::menuStart);
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
    void *address=(void*)(library.base+target::fixedUpdate),*menuAddress=(void*)(library.base+target::menuStart);
    if(memcmp(address,target::fixedBytes,sizeof(target::fixedBytes))||memcmp(menuAddress,target::menuBytes,sizeof(target::menuBytes))){setStatus("Мод не подключён: код контроллера отличается от проверенного");return;}
    log("BOOT original ELF build ID and both method entry points verified");
    int hookInit=shadowhook_init(SHADOWHOOK_MODE_UNIQUE,false);
    if(hookInit!=0){setStatus(std::string("Ошибка подключения: ")+shadowhook_to_errmsg(hookInit));return;}
    if(!address||!shadowhook_hook_func_addr(address,(void *)hookedFixed,(void **)&originalFixed)){
        setStatus(std::string("Не удалось подключить игровой цикл: ")+shadowhook_to_errmsg(shadowhook_get_errno()));return;
    }
    if(!shadowhook_hook_func_addr(menuAddress,(void*)hookedMenu,(void**)&originalMenu))log("Menu startup acknowledgement hook unavailable");
    setStatus("Подключение установлено. Оружие и боты включатся при начале игры.");
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm,void *){jvm=vm;return JNI_VERSION_1_6;}
extern "C" JNIEXPORT void JNICALL Java_org_modlab_granny_ModOverlay_nativeStart(JNIEnv *env,jclass cls,jstring path,jint mode,jstring token){
    if(started.exchange(true))return;
    const char *p=env->GetStringUTFChars(path,nullptr);std::string directory=p;env->ReleaseStringUTFChars(path,p);
    std::string filename=directory+"/granny-csgo.log";
    if(token){const char *value=env->GetStringUTFChars(token,nullptr);std::string id=value;env->ReleaseStringUTFChars(token,value);if(id.size()==36&&id.find_first_not_of("0123456789abcdef-")==std::string::npos)readyFile=directory+"/unity-ready-"+id;}
    logFile=fopen(filename.c_str(),"a");overlay=(jclass)env->NewGlobalRef(cls);soundCallback=env->GetStaticMethodID(cls,"playShot","(IF)V");
    botsEnabled=mode>=2;
    log("Granny Tactical Lab iteration 7 / Granny 1.8.12 / arm64-v8a / automatic mode="+std::to_string(mode));
    std::thread(boot).detach();
}
extern "C" JNIEXPORT void JNICALL Java_org_modlab_granny_ModOverlay_nativeAction(JNIEnv *,jclass,jint action,jint value){
    switch(action){case 0:fireInput=value;break;case 1:reloadInput=1;break;case 2:buyInput=value;break;case 3:selectInput=value;break;case 4:altInput=1;break;case 5:jumpInput=1;break;case 6:spawnInput=1;break;case 7:armorInput=1;break;case 8:paused=value;fireInput=0;break;}
}
extern "C" JNIEXPORT jstring JNICALL Java_org_modlab_granny_ModOverlay_nativeHud(JNIEnv *env,jclass){
    std::lock_guard<std::mutex> lock(hudMutex);
    if(hud=="{}")return env->NewStringUTF(("{\"status\":\""+escape(bootStatus)+"\",\"game\":false}").c_str());
    return env->NewStringUTF(hud.c_str());
}
