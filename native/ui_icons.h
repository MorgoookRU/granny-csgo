#pragma once
// Procedural HUD artwork: anti-aliased signed-distance shapes rendered to RGBA8.
// Pure C++ so it can be previewed on the host (checks/ui_icons_preview.cpp).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace icons {

struct P {float x,y;};
static inline P sub(P a,P b){return {a.x-b.x,a.y-b.y};}
static inline float dotp(P a,P b){return a.x*b.x+a.y*b.y;}
static inline float len(P a){return std::sqrt(dotp(a,a));}
static inline float clampf(float v,float a,float b){return std::max(a,std::min(v,b));}

// Distances in the [-1,1] icon square; negative inside.
static inline float circle(P p,P c,float r){return len(sub(p,c))-r;}
static inline float ring(P p,P c,float r,float t){return std::fabs(len(sub(p,c))-r)-t;}
static inline float segment(P p,P a,P b,float t){
    P pa=sub(p,a),ba=sub(b,a);float h=clampf(dotp(pa,ba)/dotp(ba,ba),0,1);
    return len({pa.x-ba.x*h,pa.y-ba.y*h})-t;
}
static inline float box(P p,P c,P half,float round){
    P d={std::fabs(p.x-c.x)-half.x+round,std::fabs(p.y-c.y)-half.y+round};
    P o={std::max(d.x,0.f),std::max(d.y,0.f)};
    return len(o)+std::min(std::max(d.x,d.y),0.f)-round;
}
// Convex or concave polygon (Inigo Quilez's sdPolygon).
static inline float polygon(P p,const P *v,int n){
    float d=dotp(sub(p,v[0]),sub(p,v[0]));float s=1;
    for(int i=0,j=n-1;i<n;j=i,i++){
        P e=sub(v[j],v[i]),w=sub(p,v[i]);
        float h=clampf(dotp(w,e)/dotp(e,e),0,1);P b={w.x-e.x*h,w.y-e.y*h};d=std::min(d,dotp(b,b));
        bool c1=p.y>=v[i].y,c2=p.y<v[j].y,c3=e.x*w.y>e.y*w.x;
        if((c1&&c2&&c3)||(!c1&&!c2&&!c3))s=-s;
    }
    return s*std::sqrt(d);
}
// Arc of a ring between two angles (radians, counter-clockwise from +x).
static inline float arc(P p,P c,float r,float t,float a0,float a1){
    P q=sub(p,c);float a=std::atan2(q.y,q.x);if(a<a0)a+=6.2831853f;
    if(a<=a1)return std::fabs(len(q)-r)-t;
    P e0={c.x+r*std::cos(a0),c.y+r*std::sin(a0)},e1={c.x+r*std::cos(a1),c.y+r*std::sin(a1)};
    return std::min(len(sub(p,e0)),len(sub(p,e1)))-t;
}

enum Icon {Fire,Aim,Reload,Jump,Swap,Gear,Cart,Knife,Grenade,Count};

// Icon shape distance; the icons are white on transparent.
static inline float iconShape(int icon,P p){
    switch(icon){
    case Fire:{ // bullet pointing up
        P tip[]={{-.27f,.2f},{.27f,.2f},{.16f,.52f},{0,.7f},{-.16f,.52f}};
        float body=box(p,{0,-.2f},{.27f,.4f},.04f),head=polygon(p,tip,5);
        float rim=box(p,{0,-.62f},{.31f,.07f},.03f);
        return std::min(std::min(body,head),rim);
    }
    case Aim:{ // scope reticle
        float d=ring(p,{0,0},.48f,.055f);
        d=std::min(d,segment(p,{-.68f,0},{-.18f,0},.045f));d=std::min(d,segment(p,{.18f,0},{.68f,0},.045f));
        d=std::min(d,segment(p,{0,-.68f},{0,-.18f},.045f));d=std::min(d,segment(p,{0,.18f},{0,.68f},.045f));
        return std::min(d,circle(p,{0,0},.06f));
    }
    case Reload:{ // circular arrow
        float d=arc(p,{0,0},.46f,.075f,.6f,5.6f);
        float a=.6f;P e={.46f*std::cos(a),.46f*std::sin(a)};
        P head[]={{e.x-.22f,e.y+.02f},{e.x+.2f,e.y+.06f},{e.x+.02f,e.y-.28f}};
        return std::min(d,polygon(p,head,3));
    }
    case Jump:{ // double chevron up
        float d=segment(p,{-.42f,-.16f},{0,.22f},.075f);d=std::min(d,segment(p,{0,.22f},{.42f,-.16f},.075f));
        d=std::min(d,segment(p,{-.42f,-.52f},{0,-.14f},.075f));return std::min(d,segment(p,{0,-.14f},{.42f,-.52f},.075f));
    }
    case Swap:{ // two opposing arrows
        float d=segment(p,{-.5f,.22f},{.38f,.22f},.065f);
        P r[]={{.3f,.42f},{.62f,.22f},{.3f,.02f}};d=std::min(d,polygon(p,r,3));
        d=std::min(d,segment(p,{-.38f,-.22f},{.5f,-.22f},.065f));
        P l[]={{-.3f,-.02f},{-.62f,-.22f},{-.3f,-.42f}};return std::min(d,polygon(p,l,3));
    }
    case Gear:{
        float d=circle(p,{0,0},.42f);
        for(int i=0;i<8;i++){float a=i*.785398f;P c={.5f*std::cos(a),.5f*std::sin(a)};
            P q=sub(p,c);P r={q.x*std::cos(-a)-q.y*std::sin(-a),q.x*std::sin(-a)+q.y*std::cos(-a)};
            d=std::min(d,box(r,{0,0},{.13f,.1f},.02f));}
        return std::max(d,-circle(p,{0,0},.17f));
    }
    case Cart:{ // shopping cart
        float d=segment(p,{-.62f,.46f},{-.42f,.46f},.06f);
        P basket[]={{-.42f,.46f},{.6f,.34f},{.48f,-.12f},{-.3f,-.12f}};
        d=std::min(d,std::fabs(polygon(p,basket,4))-.06f);
        d=std::min(d,segment(p,{-.3f,-.12f},{-.36f,-.32f},.06f));d=std::min(d,segment(p,{-.36f,-.32f},{.46f,-.32f},.06f));
        d=std::min(d,circle(p,{-.22f,-.52f},.1f));return std::min(d,circle(p,{.36f,-.52f},.1f));
    }
    case Knife:{
        P blade[]={{-.12f,.18f},{.5f,.18f},{.74f,.02f},{.52f,-.12f},{-.12f,-.12f}};
        float d=polygon(p,blade,5);d=std::min(d,box(p,{-.46f,.03f},{.26f,.11f},.05f));
        return std::min(d,box(p,{-.15f,.03f},{.05f,.22f},.02f));
    }
    case Grenade:{
        float d=circle(p,{0,-.12f},.42f);d=std::min(d,box(p,{0,.36f},{.16f,.12f},.03f));
        return std::min(d,ring(p,{.3f,.42f},.14f,.04f));
    }
    }
    return 1;
}

static inline float coverage(float d,float pixel){return clampf(.5f-d/pixel,0,1);}

// White icon with a soft dark drop shadow, RGBA8 straight alpha.
static inline std::vector<uint8_t> renderIcon(int icon,int n){
    std::vector<uint8_t> out(size_t(n)*n*4,0);float px=2.f/n;
    for(int y=0;y<n;y++)for(int x=0;x<n;x++){
        P p={-1+(x+.5f)*px,1-(y+.5f)*px};
        float a=coverage(iconShape(icon,{p.x/.82f,p.y/.82f})*.82f,px);
        float s=coverage(iconShape(icon,{(p.x+.04f)/.82f,(p.y+.05f)/.82f})*.82f-.03f,px*3)*.55f;
        float alpha=a+s*(1-a);float lum=alpha>0?a/alpha:0;
        uint8_t *o=&out[(size_t(y)*n+x)*4];
        o[0]=o[1]=o[2]=uint8_t(255*lum);o[3]=uint8_t(255*alpha);
    }
    return out;
}

// Round button: translucent radial fill, bright rim and a soft outer glow.
static inline std::vector<uint8_t> renderButton(int n,bool accent){
    std::vector<uint8_t> out(size_t(n)*n*4,0);float px=2.f/n;
    for(int y=0;y<n;y++)for(int x=0;x<n;x++){
        P p={-1+(x+.5f)*px,1-(y+.5f)*px};float r=len(p);
        float fill=coverage(r-.86f,px);
        float rim=coverage(std::fabs(r-.86f)-.035f,px);
        float glow=clampf(1-(r-.86f)/.12f,0,1)*(r>.86f?1.f:0.f)*.35f;
        float shade=.55f+.25f*(1-clampf(r/.86f,0,1))+.12f*clampf(p.y,0,1);
        float a=std::max(fill*.62f,std::max(rim*.95f,glow*.6f));
        float lum=(fill*.62f*shade*.32f+rim*.95f*1.f+glow*.6f*.6f)/std::max(a,1e-4f);
        uint8_t *o=&out[(size_t(y)*n+x)*4];
        float cr=accent?1.f:.92f,cg=accent?.55f:.95f,cb=accent?.25f:.98f;
        o[0]=uint8_t(255*clampf(lum*cr,0,1));o[1]=uint8_t(255*clampf(lum*cg,0,1));o[2]=uint8_t(255*clampf(lum*cb,0,1));
        o[3]=uint8_t(255*clampf(a,0,1));
    }
    return out;
}

// Rounded panel used behind HUD readouts and in the buy wheel (stretched, so corners are soft).
static inline std::vector<uint8_t> renderPanel(int w,int h,float radius){
    std::vector<uint8_t> out(size_t(w)*h*4,0);
    for(int y=0;y<h;y++)for(int x=0;x<w;x++){
        float px=x+.5f-w*.5f,py=y+.5f-h*.5f;
        float d=box({px,py},{0,0},{w*.5f-1.5f,h*.5f-1.5f},radius);
        float a=coverage(d,1.5f),edge=coverage(std::fabs(d+1.2f)-.8f,1.5f);
        float vertical=1-float(y)/h;
        uint8_t *o=&out[(size_t(y)*w+x)*4];
        float lum=.05f+.05f*vertical+edge*.6f;
        o[0]=o[1]=o[2]=uint8_t(255*clampf(lum,0,1));o[3]=uint8_t(255*clampf(a*(.82f+edge*.18f),0,1));
    }
    return out;
}

}  // namespace icons
