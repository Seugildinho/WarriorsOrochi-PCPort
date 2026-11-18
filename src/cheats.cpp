#include "cheats.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

namespace {
// Officer table: unit 0 is player 1's active character (switching characters
// copies the new one in), unit 1 player 2's. Stats are signed halfwords.
constexpr uint32_t unitTable=0x4dd540u,unitSize=0xd0u;
constexpr uint32_t lifeMax=0x80u,lifeNow=0x82u,lifeShown=0x84u,musouMax=0x86u,musouNow=0x88u;
// Growth points in the camp; the game caps them at 99999 (0x823858, 0x1d7e78).
constexpr uint32_t growthPoints=0x4cd210u,growthCap=99999u;
// Where officers lose life (each seen lowering the player's in a battle):
// FUN_00206c00(actor, amount, flag), officer when actor flag 0x8000, unit
// index at actor+0x14; FUN_001eb160(unit, delta), delta < 0 is damage;
// FUN_001ee270, the hit reaction, which writes life itself.
constexpr uint32_t damagePc=0x206c00u,lifeDeltaPc=0x1eb160u,hitPc=0x1ee270u;
// Read once per frame and pad, on the game thread: where the per-frame cheats run.
constexpr uint32_t padReadPc=0x18eb70u;

bool invincible=false,infiniteLife=false,infiniteMusou=false,infinitePoints=false;
PS2Runtime::RecompiledFunction padReadOriginal{},damageOriginal{},lifeDeltaOriginal{},hitOriginal{};

int16_t half(const uint8_t *ram,uint32_t address) { int16_t v; std::memcpy(&v,ram+address,2); return v; }
void setHalf(uint8_t *ram,uint32_t address,int16_t v) { std::memcpy(ram+address,&v,2); }

void applyFrameCheats(uint8_t *ram) {
    for(uint32_t player=0;player<2;++player) {
        const uint32_t unit=unitTable+player*unitSize;
        const int16_t life=half(ram,unit+lifeMax),musou=half(ram,unit+musouMax);
        // Outside a battle the slots are empty or stale: only plausible stats
        // are touched, and a character already down stays down.
        if(infiniteLife && life>0 && life<10000 && half(ram,unit+lifeNow)>0) {
            setHalf(ram,unit+lifeNow,life);
            setHalf(ram,unit+lifeShown,life);
        }
        if(infiniteMusou && musou>0 && musou<10000) setHalf(ram,unit+musouNow,musou);
    }
    if(infinitePoints) {
        const uint32_t points=growthCap;
        std::memcpy(ram+growthPoints,&points,4);
    }
}

void readPad(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    padReadOriginal(ram,ctx,runtime);
    applyFrameCheats(ram);
}

void reportBlocked() {
    static bool reported=false;
    if(!reported) { reported=true; std::cout << "[Orochi:cheats] Damage to the player ignored.\n"; }
}

void changeLife(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    if(getRegU32(ctx,4)<2u && static_cast<int32_t>(getRegU32(ctx,5))<0) {
        reportBlocked();
        ctx->pc=getRegU32(ctx,31);
        return;
    }
    lifeDeltaOriginal(ram,ctx,runtime);
}

// The hit reaction subtracts and checks for a knockout itself: the players get
// a life no hit can use up for the call, then their own back.
void takeHit(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    constexpr int16_t untouchable=30000;
    struct { int16_t now,shown; bool held; } kept[2]{};
    for(uint32_t player=0;player<2;++player) {
        const uint32_t unit=unitTable+player*unitSize;
        kept[player]={half(ram,unit+lifeNow),half(ram,unit+lifeShown),false};
        if(kept[player].now>0) {
            kept[player].held=true;
            setHalf(ram,unit+lifeNow,untouchable);
        }
    }
    hitOriginal(ram,ctx,runtime);
    for(uint32_t player=0;player<2;++player) {
        if(!kept[player].held) continue;
        const uint32_t unit=unitTable+player*unitSize;
        if(half(ram,unit+lifeNow)!=untouchable) reportBlocked();
        setHalf(ram,unit+lifeNow,kept[player].now);
        setHalf(ram,unit+lifeShown,kept[player].shown);
    }
}

void takeDamage(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    const uint32_t actor=getRegU32(ctx,4)&0x1ffffffu;
    uint32_t flags=0,unit=~0u;
    if(actor && actor+0x84u<PS2_RAM_SIZE) {
        std::memcpy(&flags,ram+actor+0x80u,4);
        std::memcpy(&unit,ram+actor+0x14u,4);
    }
    if((flags & 0x8000u) && unit<2u) {
        reportBlocked();
        ctx->pc=getRegU32(ctx,31);
        return;
    }
    damageOriginal(ram,ctx,runtime);
}
}

bool installOrochiCheats(PS2Runtime &runtime) {
    const char *list=std::getenv("OROCHI_CHEATS");
    if(!list || !*list) return true;
    std::stringstream names(list);
    for(std::string name;std::getline(names,name,',');) {
        if(name=="invincible") invincible=true;
        else if(name=="life") infiniteLife=true;
        else if(name=="musou") infiniteMusou=true;
        else if(name=="points") infinitePoints=true;
    }
    std::cout << "[Orochi:cheats] invincible=" << invincible << " life=" << infiniteLife
              << " musou=" << infiniteMusou << " points=" << infinitePoints << '\n';
    if(infiniteLife || infiniteMusou || infinitePoints) {
        padReadOriginal=runtime.lookupFunction(padReadPc);
        if(!padReadOriginal || !runtime.replaceFunction(padReadPc,readPad)) return false;
    }
    if(invincible) {
        damageOriginal=runtime.lookupFunction(damagePc);
        lifeDeltaOriginal=runtime.lookupFunction(lifeDeltaPc);
        hitOriginal=runtime.lookupFunction(hitPc);
        if(!damageOriginal || !lifeDeltaOriginal || !hitOriginal || !runtime.replaceFunction(damagePc,takeDamage) ||
           !runtime.replaceFunction(lifeDeltaPc,changeLife) || !runtime.replaceFunction(hitPc,takeHit)) return false;
    }
    return true;
}
