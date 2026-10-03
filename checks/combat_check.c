#include <assert.h>
#include <stdio.h>
#include "combat.h"

int main(void) {
    const Weapon w[] = {
        {"Pistol",0,200,3,5,1,30,400,2,0,0,5,0,0,0,0,0},
        {"Automatic",2,2700,3,6,1,36,600,2,0,0,4,1,0,0,0,0},
        {"Shotgun",4,1050,4,3,8,26,68,.5f,0,0,4,0,0,0,0,1},
        {"Knife",6,0,1,0,1,40,100,0,0,0,5,0,0,0,0,0}
    };
    Combat c;combat_init(&c,w,4);
    assert(c.money==16000 && c.health==100 && c.owned[0] && !c.owned[1]);
    assert(!combat_buy(&c,w,4,-1) && !combat_buy(&c,w,4,4));
    assert(combat_fire(&c,w,1) && c.ammo[0]==2);
    combat_tick(&c,w,1);assert(!combat_fire(&c,w,1));
    combat_fire(&c,w,0);assert(combat_fire(&c,w,1));
    combat_reload(&c,w);combat_tick(&c,w,2.1f);
    assert(c.ammo[0]==3 && c.reserve[0]==3);
    assert(combat_buy(&c,w,4,1) && c.money==13300);
    assert(combat_fire(&c,w,1));combat_tick(&c,w,.2f);
    assert(combat_fire(&c,w,1));combat_tick(&c,w,.2f);
    assert(combat_fire(&c,w,1) && c.ammo[1]==0);
    combat_tick(&c,w,.2f);assert(!combat_fire(&c,w,1) && c.reload_left>0);
    combat_tick(&c,w,2.1f);assert(c.ammo[1]==3 && c.reserve[1]==3);
    c.money=1;assert(!combat_buy(&c,w,4,2) && c.money==1);
    c.money=16000;assert(combat_buy(&c,w,4,2));
    c.ammo[2]=0;combat_reload(&c,w);combat_tick(&c,w,.51f);
    assert(c.ammo[2]==1 && c.reserve[2]==2 && c.reload_left>0);
    c.cooldown=0;c.trigger_down=0;assert(combat_fire(&c,w,1) && c.reload_left==0);
    assert(combat_select(&c,4,3));combat_tick(&c,w,10);
    for(int i=0;i<10;i++){combat_fire(&c,w,0);assert(combat_fire(&c,w,1));combat_tick(&c,w,1);}
    assert(c.ammo[3]==1);
    c.money=15900;combat_reward(&c,300);assert(c.money==16000 && c.kills==1);
    c.health=100;c.armor=10;combat_damage(&c,50);assert(c.health==60 && c.armor==0);
    combat_damage(&c,999);assert(c.health==0 && !combat_fire(&c,w,1));
    puts("Combat checks passed: purchase, inventory, firing, reloads, armor, damage and money limit.");
}
