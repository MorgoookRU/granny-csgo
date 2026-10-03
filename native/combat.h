#pragma once
#include <stdint.h>

#define MAX_WEAPONS 48
typedef struct {
    const char *name;
    int kind, price, magazine, reserve, pellets;
    float damage, rpm, reload, spread, kick, speed;
    int automatic, scoped, silenced, burst, per_shell;
} Weapon;

typedef struct {
    int owned[MAX_WEAPONS], ammo[MAX_WEAPONS], reserve[MAX_WEAPONS];
    int weapon, money, kills, health, armor, trigger_down, shots;
    float cooldown, reload_left;
    int helmet;
} Combat;

static inline void combat_init(Combat *c, const Weapon *w, int count) {
#ifdef __cplusplus
    *c = Combat{};
#else
    *c = (Combat){0};
#endif
    c->money = 16000; c->health = 100; c->armor = 100;
    for (int i = 0; i < count; ++i) {
        c->ammo[i] = w[i].magazine; c->reserve[i] = w[i].reserve;
        if (i == 0 || w[i].kind == 6) c->owned[i] = 1;
    }
}
static inline int combat_buy(Combat *c, const Weapon *w, int count, int id) {
    if (id < 0 || id >= count || c->money < w[id].price) return 0;
    c->money -= w[id].price;
    c->owned[id] = 1; c->ammo[id] = w[id].magazine;
    c->reserve[id] = w[id].reserve; c->weapon = id;
    c->reload_left = 0; c->shots = 0; c->trigger_down = 0;
    return 1;
}
static inline int combat_select(Combat *c, int count, int id) {
    if (id < 0 || id >= count || !c->owned[id]) return 0;
    c->weapon = id; c->reload_left = 0; c->shots = 0;
    c->trigger_down = 0; return 1;
}
static inline void combat_reload(Combat *c, const Weapon *w) {
    int i = c->weapon;
    if (c->reload_left <= 0 && c->ammo[i] < w[i].magazine &&
        c->reserve[i] > 0 && w[i].kind < 6) c->reload_left = w[i].reload;
}
static inline void combat_tick(Combat *c, const Weapon *w, float dt) {
    if (dt <= 0) return;
    c->cooldown -= dt; if (c->cooldown < 0) c->cooldown = 0;
    if (c->reload_left > 0) {
        c->reload_left -= dt;
        if (c->reload_left <= 0) {
            int i = c->weapon;
            int n = w[i].per_shell ? 1 : w[i].magazine - c->ammo[i];
            if (n > c->reserve[i]) n = c->reserve[i];
            c->ammo[i] += n; c->reserve[i] -= n;
            c->reload_left = w[i].per_shell && c->ammo[i] < w[i].magazine &&
                             c->reserve[i] > 0 ? w[i].reload : 0;
        }
    }
}
static inline int combat_fire(Combat *c, const Weapon *w, int held) {
    int i = c->weapon;
    int edge = held && !c->trigger_down;
    c->trigger_down = held;
    if (!held) { c->shots = 0; return 0; }
    if (!c->owned[i] || c->health <= 0 || c->cooldown > 0) return 0;
    if (!w[i].automatic && !edge) return 0;
    if (c->reload_left > 0) {
        if (!w[i].per_shell || c->ammo[i] <= 0) return 0;
        c->reload_left = 0;
    }
    if (c->ammo[i] <= 0) { combat_reload(c, w); return 0; }
    if (w[i].kind != 6) --c->ammo[i];
    c->cooldown = 60.0f / w[i].rpm; ++c->shots;
    return 1;
}
static inline void combat_reward(Combat *c, int reward) {
    c->money += reward;
    if (c->money > 16000) c->money = 16000;
    ++c->kills;
}
static inline void combat_damage(Combat *c, int damage) {
    if (damage <= 0) return;
    int absorbed = c->armor > 0 ? damage * 2 / 5 : 0;
    if (absorbed > c->armor) absorbed = c->armor;
    c->armor -= absorbed; c->health -= damage - absorbed;
    if (c->health < 0) c->health = 0;
}
