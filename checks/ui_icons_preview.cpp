// Host preview of the procedural HUD artwork: writes raw RGBA files for checks/ui_preview.py.
#include <cstdio>
#include "ui_icons.h"
static void save(const char *name,const std::vector<uint8_t> &data){FILE *f=fopen(name,"wb");fwrite(data.data(),1,data.size(),f);fclose(f);}
int main(){
    char name[64];
    for(int i=0;i<icons::Count;i++){snprintf(name,sizeof name,"icon%d.rgba",i);save(name,icons::renderIcon(i,128));}
    save("button.rgba",icons::renderButton(128,false));save("button_accent.rgba",icons::renderButton(128,true));
    save("panel.rgba",icons::renderPanel(256,96,18));
    puts("rendered");
}
