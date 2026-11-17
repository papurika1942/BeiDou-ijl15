#include "stdafx.h"
#include "CombatDamage.h"
#include "CombatHooks.h"

// CombatDamage.cpp - damage pipeline injection (client side).
//
// ---------------------------------------------------------------------------
// Landing zones. "fcom qword ptr [0x00AFE8A0]" (the output cap compare) occurs
// exactly 6 times in .text and every one of them is byte-identical:
//
//     DC 15 A0 E8 AF 00   fcom qword ptr [0x00AFE8A0]   6 bytes
//     DF E0               fnstsw ax                     <- resume here (site+6)
//     9E                  sahf
//     72 xx               jb ...
//
// Only four of those six are top-level exits; the other two are downstream of
// one of the four, so patching them would multiply twice:
//
//   site            enclosing function          resume      note
//   0x00790586      PDamage 0x0078DF87          0x0079058C  bit0, tested
//   0x00791F8B      0x00791617 (C1)             0x00791F91  bit1
//   0x0079343F      0x0079309F (C3, ret 0x1C)   0x00793445  bit2
//   0x00793668      0x0079345E (C4, ret 0x18)   0x0079366E  bit3
//   0x007907B7      clamp helper 0x00790782     0x007907BD  DO NOT PATCH
//   0x00792FE4      0x00792FA8, called by C3/C4  0x00792FEA  DO NOT PATCH
//
// Verified by call-graph scan: PDamage / C1 / C3 / C4 never call each other.
// C3 and C4 both call 0x00792FA8 (which owns the 5th/6th compare), so those
// two must stay untouched as long as C3/C4 are patched.
//
// Each top-level exit returns ONE damage value per hit, so a multiplier here
// is a per-hit multiplier, never a per-row accumulator.
//
// Why 6 and not 7: "DC 15 + disp32" is 2 + 4. Patching 7 bytes eats the DF of
// the resume "fnstsw ax" and the cave returns into "90 E0 9E 72" (nop;loopne).
// That mistake cost one evening (23:3x crash), see R16 9.3.
//
// NOT covered here: the eight shadow-partner skill ids branch out at
// 0x0078E3E7 / 0x0078E413 straight to 0x007905A5 / 0x007905B4 and never touch
// 0x00790586. Those rows need the N-batch.
//
// Keep this file pure ASCII: MSVC builds with code page 936 and a non-ASCII
// byte here once swallowed the next declaration (C4819 + bogus C2065).

volatile long g_caveHits = 0;
volatile long g_probePrinted = 0;
int g_probeEnabled = 0;
long g_probeBudget = 40;
// damageProbe < 0 means "print every row, including no-op rows", which is the
// only way to see the raw damage distribution BEFORE our multiplier is applied.
// That distribution is the decisive test for a native client-side critical roll.
int g_probeAll = 0;
// critMode (R23):
//   0 = off. Strictly no-op for critRate/critDmg; keeps the 11-13 baseline.
//   1 = legacy per-row random roll (a SECOND crit roll on top of the native
//       one; produces 4 damage tiers and lifts expected damage). A/B only.
//   2 = scheme B: add critRate/critDmg to the NATIVE accumulators inside
//       PDamage 0x78DF87 BEFORE its single roll. Implemented by the N1 cave,
//       NOT here. See CombatNativeCritAdd below and R23.
// Off by default: with a second roll on, a buffed 6-row skill jumps from x3 to
// up to x6 per row, which silently changes the 11-13 regression baseline (and
// can trip the server 5x addPoint line).
int g_critMode = 0;

// CORRECTION (R20 3.1): 0x416563 is NOT an RNG. It is an obfuscated-state
// decoder with an integrity check; the exe's real RNG is 0x4165EC (a
// xorshift-style generator using the global state 0x00BEDD38). R23 found the
// native crit roll: PDamage calls 0x4165EC seven times at 0x78E003 to fill
// [ebp-0x64..-0x4C], then each line uses rand % 10000000 * 1.0000001e-5
// (0x00AFE838) as a 0..100 roll at 0x79018E. Do not hook 0x416563.
// Mode 1 below is therefore a SECOND, INDEPENDENT roll; mode 2 removes it.
volatile long g_rngState = 0;
volatile long g_nativeLogHits = 0;
volatile long g_c1LogHits = 0;   // R31: separate log budget for the C1 cave.

// R33: let 4221001 (Assassinate) crit on every segment. PDamage compares
// [ebp-0x28] against 3211003 / 4111004 / 4221001 at 0x7900EE / 0x7900F7 /
// 0x790100, then all three land on 0x790109 "cmp [ebp+0x30],0x3C"; a segment
// that is not 0x3C takes 0x79010D jne 0x7901B8 and skips the whole crit block,
// so even the C2 values N1 already added are wasted. Rewriting the immediate of
// the 0x790100 compare to an impossible id makes every 4221001 segment fall to
// 0x790113 (native 0x765095 accumulators + N1's C2 add). The final blow is 0x3C
// anyway, so its WZ 90/250 behaviour is untouched. The other two skills keep the
// original skip.
//
// R37 CORRECTION: WZ 4221001 lv30 is prop=90 / criticalDamage=250 (lv1 is
// 32 / 105 -- the "105" in R32/R33 was the LEVEL-1 criticalDamage and must never
// be read as a rate). So an unbuffed Assassinate segment has a 90% crit chance,
// NOT 100%. Only Sharp Eyes pushes it to 90+15=105 >= 100 ("all segments crit"
// in the R37 run). The +15/+140 comes from the Sharp-Eyes stat block at
// 0x78E160 / 0x78E320, NOT from the C2 packet and NOT from N1.
int g_assassinOpen = 0;
// R33 S5: enable the melee shadow-partner row expansion at PDamage entry.
int g_shadowMelee = 0;
// R36 U1: also double the upper-layer row count so that the damage-number loop
// and the apply/packet call follow the extra rows. 0 = compute only (old S5).
int g_shadowBcast = 1;
// R33 M1: apply finalDamagePercent to Meso Explosion (4211006), which has its
// own damage routine at 0x791FBC.
int g_mesoFinal = 0;
// R37 A3: give 4111004 (Shadow Meso, the "money attack") the NATIVE crit chain
// instead of its WZ-fixed prop=10 pair. Two writes, both needed:
//   - 0x7900FA imm32 0x003EBA9C -> 0x7FFFFFFF: 4111004 stops matching the
//     three-skill list, so every row reaches 0x790113 and becomes eligible.
//   - the A2 cave at 0x78E0FF now answers "force NATIVE" for 4111004, so the
//     rate/dmg come from 0x765095 + the Sharp-Eyes block + N1's C2 add.
// WZ 4111004 lv30 has prop=10 and NO criticalDamage node, so untouched it is a
// 10% fixed crit with a x1.0 multiplier -- exactly the "10% chance" in the
// skill text. The server only checks moneyCon*10*1.5 (=8550 at lv30), it never
// recomputes the damage, so scaling the client value cannot be rejected.
int g_mesoAttack = 0;
// R39: 4111004 keeps its own WZ pair and STACKS the native aggregate on top,
// exactly like 4221001 does after AN1. The skill text promises "10% chance,
// +50% damage" but WZ 4111004 has no criticalDamage node at all, so the
// decoded value is 0 and the original code multiplied by (0+100)/100 = x1.0:
// the yellow number showed up but the damage never moved.
//   > 0 : use that number as the WZ half of the multiplier
//   = 0 : supply nothing (pure native multiplier)
//   < 0 : follow the skill's own WZ `x` (lv30 = 50), which is what the text
//         calls the "50%". NOTE: `x` is ALSO the damage base -- the branch
//         computes u in [0.5x, 1.5x] then ftol(u)*10 -- so raising x scales
//         the base AND this multiplier at the same time.
// A real `criticalDamage` node always wins over all three.
int g_mesoAtkCd = -1;

static unsigned int NextRand() {
    long s = InterlockedIncrement(&g_rngState);
    unsigned int x = static_cast<unsigned int>(s) * 1103515245u + 12345u;
    x ^= GetTickCount();
    x ^= x >> 16;
    x *= 2246822519u;
    x ^= x >> 13;
    return x;
}

// Multiplier contract (keep in sync with the WZ nodes):
//   g_combat[0] critRate  : percent. Mode 1 rolls PER ROW (>=100 always hits).
//   g_combat[1] critDmg   : critDamagePercent. Mode 1 factor is
//                          (1 + critDmg/100), i.e. 100 means "crit doubles".
//   g_combat[2] finalPct  : finalDamagePercent, factor (1 + finalPct/100).
//   R23: scheme B adds [0]/[1] to the NATIVE accumulators before the native
//   roll. Those two values must NOT be consumed in this cave.
extern "C" void CombatApplyDamageModifiers(double* dmg);
extern "C" void CombatApplyDamageModifiers(double* dmg) {
    const int critRate = CombatHooks::g_combat[0];
    const int critDmg = CombatHooks::g_combat[1];
    const int finalPct = CombatHooks::g_combat[2];

    int critHit = 0;
    double mult = 1.0;
    if (g_critMode == 1 && critRate > 0 &&
        (critRate >= 100 || static_cast<int>(NextRand() % 100u) < critRate)) {
        // Legacy dual-roll path. Kept for A/B comparison only (R21 4).
        critHit = 1;
        if (critDmg > 0) {
            mult *= (1.0 + static_cast<double>(critDmg) / 100.0);
        }
    }
    // R23: scheme B is NOT applied here. critRate/critDmg are added to the
    // native accumulators by the N1 cave before this point. Any crit
    // multiplier applied here would be a second roll and is wrong.
    if (finalPct > 0) {
        mult *= (1.0 + static_cast<double>(finalPct) / 100.0);
    }

    const int before = static_cast<int>(*dmg);
    if (mult != 1.0) {
        *dmg *= mult;
    }

    // Probe policy (see R16 10.2 / R18 5): the very first hit of the session
    // always prints. By default only modified rows print and no-op rows spend
    // no budget; with g_probeAll (negative damageProbe) every row prints so the
    // raw `in` distribution can reveal a native client-side crit roll.
    long hit = 0;
    if (g_probeEnabled != 0) {
        hit = InterlockedIncrement(&g_caveHits);
    }
    // "changed" is more robust than (critHit || finalPct): any future
    // multiplier that is not driven by those two flags still gets logged.
    const bool interesting = (hit == 1) || (g_probeAll != 0) || (mult != 1.0);
    if (g_probeEnabled != 0 && interesting) {
        // Budget must count PRINTED lines only. The old code compared the global
        // hit counter, so a long buff-less warm-up silently used up the whole
        // budget and the buff rows never appeared (R16 10.2 regression).
        const long n = InterlockedIncrement(&g_probePrinted);
        if (n <= g_probeBudget) {
            char line[176];
            // R37: these are the C2 PACKET values, not the crit parameters the
            // frame actually uses -- see CRITFRAME for those. Kept as c2cr/c2cd
            // so nobody reads zeros here as "no crit".
            wsprintfA(line, "[combat] DAMAGECAVE hit=%d c2cr=%d c2cd=%d crit=%d c2pct=%d in=%d out=%d md=%d\r\n",
                      static_cast<int>(hit), critRate, critDmg, critHit, finalPct,
                      before, static_cast<int>(*dmg), g_critMode);
            OutputDebugStringA(line);
        }
    }
}

// ---------------------------------------------------------------------------
// R29 / scheme A (R28 2.4): a shadow-partner copy row must NOT be multiplied.
//
// Byte-level root cause of the "final damage counted twice" bug:
//   PDamage's discount branch at 0x7901DD copies the matching front-half row
//     0x790239  mov eax, [ecx+eax]   ; damage[j - n/2]
//   and the array slot it reads was written at
//     0x7905A1  mov [ecx], eax       ; right after call 0xA62018
//   which is immediately AFTER our cave at 0x790586. So the copy source is
//   ALREADY multiplied; applying the cave to the copy squares the multiplier.
//   (R6 1.2: "strictly follows the body hit, never re-rolls".)
//
// Reuse the native branch's own predicate verbatim (R6 1.1):
//   [ebp+0x34] != 0                   discount/shadow flag (parameter 12)
//   [ebp-0x28] != 0x003EE1AB          excluded skill
//   n = [ebp+0x20] (numDamage), j = [ebp+0x24] (current row)
//   j >= n / 2                        back half = copy rows
// One extra safety floor: n >= 2. With n == 1 the native test is trivially
// true (j=0 >= 0), and skipping the only row would silently remove final
// damage from every single-hit skill. Real copy rows always have n >= 2.
//
// Deliberately NOT gated on CombatHooks::g_shadowActive: that mirror has never
// been field-verified (P2-5 buff state machine still open), and the predicate
// above is the definition of a copy row regardless of which buff caused it.
// It is printed in the log as shdw= so the mirror can be checked in passing.
// ---------------------------------------------------------------------------
static int FrameInt(const char* f, int off) {
    return *reinterpret_cast<const int*>(f + off);
}

static bool IsDamageCopyRow(const void* frameEbp) {
    if (frameEbp == 0) {
        return false;
    }
    const char* f = static_cast<const char*>(frameEbp);
    const int shadowFlag = FrameInt(f, 0x34);
    const int numDamage  = FrameInt(f, 0x20);
    const int rowIndex   = FrameInt(f, 0x24);
    const int skillId    = FrameInt(f, -0x28);
    if (shadowFlag == 0 || numDamage < 2 || skillId == 0x003EE1AB) {
        return false;
    }
    return rowIndex >= (numDamage >> 1);
}

// S1 / PDamage only. frameEbp is the game's EBP saved by the cave's pushad,
// NOT our own frame. Site2/3/4 keep calling CombatApplyDamageModifiers as-is:
// shadow partner was measured to travel exclusively through S1 (R29).
//
// R37: the crit numbers a player actually sees are the GAME FRAME slots, not
// the C2 packet. PDamage keeps crit rate at [ebp-0x40], crit damage at
// [ebp-0x34] and the eligibility flag at [ebp-0x80]; the last writer is
// 0x78E364 (and the N1 cave at 0x78E369), after which they are read only
// (0x790113 / 0x790176 / 0x790190). The S1 cave therefore still sees the final
// values. Printing g_combat[] here was the R36 defect: without a C2 packet the
// line read cr=0 cd=0 while the client was in fact critting at 90% from the WZ
// node plus Sharp Eyes. CRITFRAME replaces that inference with the real slots.
volatile long g_frameLogHits = 0;
volatile long g_critHitHits = 0;
// R40 A5: 1 = replace the native crit add with a straight `dmg *= cd/100`.
// The original sequence is `fild cd; add -100; fmul 0.01; fimul [ebp-8];
// fadd [ebp-0x20]`, i.e. it scales a SNAPSHOT of the row value taken before
// any earlier multiplier ran. When [ebp-8] and the live total at [ebp-0x20]
// disagree the crit row lands BELOW an uncritted row -- exactly the 5.5w
// yellow vs 5.0w red the owner reported. Multiplying the live total is
// identical whenever the two agree, so this cannot regress the normal case.
int g_critFix = 1;

static void LogCritFrame(const void* frameEbp, int inDmg, int outDmg) {
    if (g_probeEnabled == 0 || frameEbp == 0) {
        return;
    }
    const long n = InterlockedIncrement(&g_frameLogHits);
    if (n > 80) {
        return;   // first 80 rows only: enough for one attack of every skill
    }
    const char* f = static_cast<const char*>(frameEbp);
    const int j = FrameInt(f, 0x24);
    // R40: [ebp-0x80] is NOT "did this row crit" -- it is the eligibility
    // marker (0x78E123 stores [ebp+0x3c] there on the WZ path, the return of
    // 0x765095 on the weapon path), so it read 0 even on a red number.
    // [ebp+0x44] is the per-row array: 0x7901A2 writes 1 when the row crits.
    int rowv = -1;
    const int* rows = reinterpret_cast<const int*>(FrameInt(f, 0x44));
    if (rows != 0 && j >= 0 && j < 64) {
        rowv = rows[j];
    }
    char line[224];
    wsprintfA(line,
              "[combat] CRITFRAME n=%d skill=%08X j=%d/%d rate=%d cd=%d row=%d base8=%d in=%d out=%d\r\n",
              static_cast<int>(n), static_cast<unsigned>(FrameInt(f, -0x28)),
              j, FrameInt(f, 0x20),
              FrameInt(f, -0x40), FrameInt(f, -0x34), rowv, FrameInt(f, -8),
              inDmg, outDmg);
    OutputDebugStringA(line);
}

// R40 A5: straight crit scaling, see g_critFix above.
extern "C" void CombatCritScale(void* frame) {
    char* f = static_cast<char*>(frame);
    const int cd = *reinterpret_cast<int*>(f - 0x34);
    const int dmg0 = *reinterpret_cast<int*>(f - 8);
    double* dmg = reinterpret_cast<double*>(f - 0x20);
    const int before = static_cast<int>(*dmg);
    if (cd > 100) {
        *dmg = *dmg * (static_cast<double>(cd) / 100.0);
    }
    if (g_probeEnabled != 0) {
        const long k = InterlockedIncrement(&g_critHitHits);
        if (k <= 160) {
            char line[200];
            // base8= is what the ORIGINAL sequence would have scaled. A value
            // far from in= is the proof that the old add under/over-shot.
            wsprintfA(line,
                      "[combat] CRITHIT j=%d/%d cd=%d base8=%d in=%d out=%d k=%d\r\n",
                      *reinterpret_cast<int*>(f + 0x24),
                      *reinterpret_cast<int*>(f + 0x20),
                      cd, dmg0, before, static_cast<int>(*dmg),
                      static_cast<int>(k));
            OutputDebugStringA(line);
        }
    }
}

extern "C" __declspec(naked) void CombatCritScaleCave(void) {
    __asm {
        // The replaced 6 bytes are `fmul qword ptr [0xaf14f0]`; the `fild`
        // just before it left (cd-100) on the x87 stack, so drop it first and
        // keep the FPU stack balanced for the C call.
        fstp st(0)
        pushad
        push ebp
        call CombatCritScale
        add  esp, 4
        popad
        mov  eax, 0x007901B8
        jmp  eax
    }
}

extern "C" void CombatApplyDamageModifiersS1(double* dmg, const void* frameEbp);
extern "C" void CombatApplyDamageModifiersS1(double* dmg, const void* frameEbp) {
    if (!IsDamageCopyRow(frameEbp)) {
        const int before = static_cast<int>(*dmg);
        CombatApplyDamageModifiers(dmg);
        LogCritFrame(frameEbp, before, static_cast<int>(*dmg));
        return;
    }
    // Leave the value untouched: it already carries finalPct and the native
    // crit result from its front-half source. Emit a dedicated line so the
    // fix is verifiable without a debugger.
    if (g_probeEnabled != 0) {
        const long n = InterlockedIncrement(&g_probePrinted);
        if (n <= g_probeBudget) {
            const char* f = static_cast<const char*>(frameEbp);
            char line[160];
            wsprintfA(line,
                      "[combat] SHADOWSKIP in=%d out=%d pct=%d j=%d n=%d shdw=%u\r\n",
                      static_cast<int>(*dmg), static_cast<int>(*dmg),
                      CombatHooks::g_combat[2], FrameInt(f, 0x24),
                      FrameInt(f, 0x20),
                      static_cast<unsigned>(CombatHooks::g_shadowActive));
            OutputDebugStringA(line);
        }
    }
}

// Cave body, written out once per site. NOT a macro on purpose: a macro uses
// backslash continuations, the preprocessor joins those into ONE line, and
// MSVC's inline assembler needs a newline between instructions. A joined line
// fails to assemble ("sub esp, 8 fstp qword...").
//
// Stack discipline: the staging slot is OUR OWN 8 bytes (never [ebp-0x20]),
// so no assumption about the game frame is made. pushad moves esp by 32, so
// the slot is at [esp+32] after it. Net FPU effect is neutral: fstp(-1) then
// fld(+1), and the displaced fcom neither pushes nor pops.
// R29: S1 additionally forwards the game's saved EBP. After pushad the 8
// saved dwords live at [esp+0..+28] and EBP is the one at [esp+8]. We push it
// as the 2nd cdecl argument, so the staging slot moves from [esp+32] to
// [esp+36] while computing the pointer, and the call needs add esp,8.
extern "C" __declspec(naked) void CombatDamageCaveSite1(void) {
    __asm {
        sub  esp, 8
        fstp qword ptr [esp]
        pushad
        push dword ptr [esp + 8]
        lea  eax, [esp + 36]
        push eax
        call CombatApplyDamageModifiersS1
        add  esp, 8
        popad
        fld  qword ptr [esp]
        add  esp, 8
        fcom qword ptr ds:[0x00AFE8A0]
        mov  eax, 0x0079058C
        jmp  eax
    }
}

extern "C" __declspec(naked) void CombatDamageCaveSite2(void) {
    __asm {
        sub  esp, 8
        fstp qword ptr [esp]
        pushad
        lea  eax, [esp + 32]
        push eax
        call CombatApplyDamageModifiers
        add  esp, 4
        popad
        fld  qword ptr [esp]
        add  esp, 8
        fcom qword ptr ds:[0x00AFE8A0]
        mov  eax, 0x00791F91
        jmp  eax
    }
}

extern "C" __declspec(naked) void CombatDamageCaveSite3(void) {
    __asm {
        sub  esp, 8
        fstp qword ptr [esp]
        pushad
        lea  eax, [esp + 32]
        push eax
        call CombatApplyDamageModifiers
        add  esp, 4
        popad
        fld  qword ptr [esp]
        add  esp, 8
        fcom qword ptr ds:[0x00AFE8A0]
        mov  eax, 0x00793445
        jmp  eax
    }
}

extern "C" __declspec(naked) void CombatDamageCaveSite4(void) {
    __asm {
        sub  esp, 8
        fstp qword ptr [esp]
        pushad
        lea  eax, [esp + 32]
        push eax
        call CombatApplyDamageModifiers
        add  esp, 4
        popad
        fld  qword ptr [esp]
        add  esp, 8
        fcom qword ptr ds:[0x00AFE8A0]
        mov  eax, 0x0079366E
        jmp  eax
    }
}

// ---------------------------------------------------------------------------
// R23 / scheme B: NATIVE crit entry, PDamage 0x0078DF87 only.
//
// The client builds two TOTAL accumulators once per PDamage call:
//   [ebp-0x40] = total crit rate (percent)
//   [ebp-0x34] = total crit damage (percent, 200 means x2, 240 means x2.4)
// Sources (already additive, exactly what the owner described):
//   0x78E112 call 0x765095 -> passive skill 4100001 / 3000001 / weapon type
//            48, decodes +0xF4 (prop) and +0xD0 (damage)
//   0x78E18B / 0x78E1CA    -> buff slot A (sharp eyes x / y-100)
//   0x78E235 / 0x78E24F    -> buff slot B
//   0x78E333 / 0x78E364    -> buff slot C
// The per-line loop 0x78E3CF..0x790603 then rolls ONCE per line at 0x79018E:
//   roll  = (rand % 10000000) * 1.0000001e-5      (0x00AFE838, ~0..100)
//   crit  = roll < [ebp-0x40]                       (0x79018E jae -> skip)
//   extra = ([ebp-0x34] - 100) * 0.01 * baseDamage (0x790190..0x7901B5)
//   flag  = 1 in the per-line crit array            (0x7901A2)
//
// => Adding C2 critRate/critDmg to those two accumulators IS "one roll, both
// additive". No second roll, no expected-value term, no 4 damage tiers.
//
// Patch window 0x78E369: `cmp dword ptr [ebp-0x28], 0x3EE1B0` is exactly 7
// bytes (81 7D D8 B0 E1 3E 00), has no jump into the middle, and is reached
// once per call AFTER the last accumulator write (0x78E364) but BEFORE the
// per-line loop (0x78E3CF). Patching inside the loop would add the C2 values
// once per line and is wrong.
//
// 0x790125..0x79014C is a four-way gate: crit is skipped unless one of
// [ebp-0x80] / a decoded buff field / [ebp-0x44] / [ebp-0x6c] is nonzero.
// Classes with no native crit passive have all four zero, so mode 2 forces
// [ebp-0x80] > 0 to let the C2 crit actually reach the roll.
// ---------------------------------------------------------------------------
extern "C" void CombatNativeCritAdd(void* gameEbp);
extern "C" void CombatNativeCritAdd(void* gameEbp) {
    if (g_critMode != 2) {
        return;
    }
    char* f = static_cast<char*>(gameEbp);
    volatile int* rate = reinterpret_cast<volatile int*>(f - 0x40);
    volatile int* dmg = reinterpret_cast<volatile int*>(f - 0x34);
    volatile int* gate = reinterpret_cast<volatile int*>(f - 0x80);
    const int cr = CombatHooks::g_combat[0];
    const int cd = CombatHooks::g_combat[1];
    if (cr <= 0 && cd <= 0) {
        return;
    }

    const int rate0 = *rate;
    const int dmg0 = *dmg;
    if (cr > 0) {
        *rate += cr;
        if (*gate <= 0) {
            *gate = 1;
        }
    }
    if (cd > 0) {
        *dmg += cd;
    }

    // Dynamic confirmation without a debugger: shows the native accumulator
    // (e.g. 50/200) and the value after our addition (e.g. 65/240).
    if (g_probeEnabled != 0) {
        const long n = InterlockedIncrement(&g_nativeLogHits);
        if (n <= 20) {
            char line[192];
            wsprintfA(line,
                      "[combat] NATIVECRIT rate0=%d dmg0=%d rate=%d dmg=%d cr=%d cd=%d gate=%d\r\n",
                      rate0, dmg0, static_cast<int>(*rate), static_cast<int>(*dmg),
                      cr, cd, static_cast<int>(*gate));
            OutputDebugStringA(line);
        }
    }
}

extern "C" __declspec(naked) void CombatNativeCritCave(void) {
    __asm {
        pushad
        push ebp
        call CombatNativeCritAdd
        add  esp, 4
        popad
        cmp  dword ptr [ebp - 0x28], 0x003EE1B0
        mov  eax, 0x0078E370
        jmp  eax
    }
}

// ---------------------------------------------------------------------------
// R31 / N2: C1 (0x791617) is a SEPARATE damage pipeline used by mage magic
// skills. damageCrit=2 only patched PDamage (N1), so the C2 crit values never
// reached C1 and mage magic skills could not crit at all (R30 2.1).
//
// C1 frame layout (dis86.py verified):
//   [ebp+0x0C]  total crit RATE  (0x7917A8/AD clamps to <=100, 0x791837 add)
//   [ebp-0x24]  total crit DMG   (final write 0x79184B)
//   NOTE: [ebp+0xC] is a reused PARAMETER slot, NOT PDamage's [ebp-0x40].
//
// Patch site 0x791859, 6 bytes:
//   8B 55 30   mov edx,[ebp+0x30]
//   8B 45 34   mov eax,[ebp+0x34]
//   resume 0x79185F (sub eax,edx)
// This sits AFTER the last accumulator write and BEFORE the per-row loop at
// 0x79186A (back-edge 0x791FAF -> 0x79186A), so it runs ONCE per C1 call.
//
// C1's gate is NOT PDamage's [ebp-0x80] flag. It is a DOUBLE test:
//   0x791D80 cmp [ebp+0xC],0  / jle skip
//   0x791D85 cmp [ebp-0x24],0 / jle skip
// Adding both C2 values makes both slots positive, so no gate forcing is
// needed (forcing would change native semantics). Scheme B stays strict:
// rate += cr, dmg += cd, then C1's own single roll at 0x791DB1 consumes it.
//
// C1 crit damage is NOT S1's "snapshot + (total-100)/100" formula. C1 uses
// 1 + (dmg - base)/100, base = 100 (dmg<=200) / 200 (else), at 0x791DC3..DFD.
// Do NOT predict C1 numbers with the S1 formula.
// ---------------------------------------------------------------------------
extern "C" void CombatNativeCritAddC1(void* gameEbp);
extern "C" void CombatNativeCritAddC1(void* gameEbp) {
    if (g_critMode != 2) {
        return;
    }
    const int cr = CombatHooks::g_combat[0];
    const int cd = CombatHooks::g_combat[1];
    if (cr <= 0 && cd <= 0) {
        return;
    }
    char* f = static_cast<char*>(gameEbp);
    volatile int* rate = reinterpret_cast<volatile int*>(f + 0x0C);
    volatile int* dmg  = reinterpret_cast<volatile int*>(f - 0x24);
    const int rate0 = *rate;
    const int dmg0  = *dmg;
    if (cr > 0) {
        *rate += cr;
    }
    if (cd > 0) {
        *dmg += cd;
    }
    if (g_probeEnabled != 0) {
        const long n = InterlockedIncrement(&g_c1LogHits);
        if (n <= 20) {
            char line[192];
            wsprintfA(line,
                      "[combat] NATIVECRITC1 rate0=%d dmg0=%d rate=%d dmg=%d cr=%d cd=%d\r\n",
                      rate0, dmg0, static_cast<int>(*rate), static_cast<int>(*dmg),
                      cr, cd);
            OutputDebugStringA(line);
        }
    }
}

extern "C" __declspec(naked) void CombatNativeCritC1Cave(void) {
    __asm {
        pushad
        push ebp
        call CombatNativeCritAddC1
        add  esp, 4
        popad
        mov  edx, dword ptr [ebp + 0x30]
        mov  eax, dword ptr [ebp + 0x34]
        push 0x0079185F
        ret
    }
}

// ---------------------------------------------------------------------------
// R33 A2: give 4221001 (Assassinate) rows that are NOT the 0x3C final blow the
// same WZ crit parameters the final blow already uses. PDamage decides at
// 0x78E0E6:  cmp [ebp+0x30],0x3C ; je 0x78E11F (WZ path)  else call 0x765095.
// We hook the `je` itself (6 bytes @ 0x78E0FF) and re-emit it after asking the
// helper. EFLAGS survive the helper call via pushfd/popfd, because the `je`
// consumes the ZF left by that cmp three instructions earlier (the x87 ops in
// between do not touch EFLAGS). x87 is empty at this point: fild @0x78E0E0 is
// balanced by fstp @0x78E0FC.
// ---------------------------------------------------------------------------
volatile long g_assassinLogHits = 0;
// The cave restores EAX with popad, so the helper's answer has to survive in
// memory instead of a register: 0 = leave the native ZF verdict alone,
// 1 = force the WZ branch (4221001 non-0x3C rows), 2 = force the NATIVE branch.
volatile int g_assassinWant = 0;

extern "C" int CombatAssassinWantWz(void* frame) {
    unsigned char* f = reinterpret_cast<unsigned char*>(frame);
    const int skill = *reinterpret_cast<int*>(f - 0x28);
    if (skill != 0x00406849) {
        g_assassinWant = 0;
        return 0;   // only 4221001
    }
    if (*reinterpret_cast<int*>(f + 0x30) == 0x3c) {
        g_assassinWant = 0;
        return 0;   // final blow already takes the WZ path natively
    }
    if (g_probeEnabled != 0) {
        const long n = InterlockedIncrement(&g_assassinLogHits);
        if (n <= 20) {
            OutputDebugStringA("[combat] ASSASSINWZ 4221001 row -> WZ crit params\r\n");
        }
    }
    g_assassinWant = 1;
    return 1;
}

extern "C" __declspec(naked) void CombatAssassinWzCave(void) {
    __asm {
        pushfd
        pushad
        push ebp
        call CombatAssassinWantWz
        add  esp, 4
        popad
        popfd
        // R37: the verdict is re-read from memory and the original `cmp
        // [ebp+0x30],0x3C` is re-evaluated here, so this path no longer
        // depends on the ZF that survived the helper call.
        mov  eax, g_assassinWant
        cmp  eax, 1
        je   force_wz
        cmp  eax, 2
        je   keep_weapon_path
        cmp  dword ptr [ebp + 0x30], 0x3c
        jne  keep_weapon_path
        mov  eax, 0x0078E11F
        jmp  eax
    keep_weapon_path:
        // 0x78E0FF..0x78E104 is what the 6-byte patch replaced: the `je`
        // itself PLUS `lea eax,[ebp-0x34]` and `push eax`. Those two have to
        // be re-emitted here; resuming at 0x78E101 would land inside our own
        // E9 rel32.
        lea  eax, [ebp - 0x34]
        push eax
        mov  eax, 0x0078E105
        jmp  eax
    force_wz:
        mov  eax, 0x0078E11F
        jmp  eax
    }
}

// ---------------------------------------------------------------------------
// R38 AN1: give 4221001 the NATIVE crit base as well.
//
// A2 sends every Assassinate row down the WZ branch, and that branch OVERWRITES
// [ebp-0x40]/[ebp-0x34] with WZ prop/criticalDamage (lv30 = 90 / 250). Every
// other crit source sits AFTER the two branches merge, so a WZ-branch skill
// already eats all of them:
//   0x78E160 sharp-eyes stat block   (rate +15, dmg +140)
//   0x78E1CE stun mastery 5110000    (rate +prop, dmg +damage, only if stunned)
//   0x78E254 war-god 21110000        (rate +prop, dmg +damage)
//   0x78E369 N1's C2 add             (rate +cr, dmg +cd)
// The ONLY source it misses is the 0x765095 base, which is weapon typed:
//   45/46 bow/xbow -> 13000000, 47 claw -> 14100001, 48 knuckle -> 15110000,
//   anything else (dagger 30, sword 40 ...) -> both outputs stay 0.
// AN1 calls 0x765095 a second time into two temps and ADDs them, so Assassinate
// keeps its 90/250 AND eats every native source. For a dagger the call is a
// no-op (0/0), which is exactly why 420 needs 5110000 to have native crit at
// all -- the class itself has none.
// Patch window 0x78E160 = `push dword ptr [ebx+0x5e4]` (6 bytes), re-emitted
// here; resume 0x78E166. x87 is empty at 0x78E160 (0x78E0FC fstp), and
// 0x765095 is also called with an empty x87 stack by the game itself.
// ---------------------------------------------------------------------------
int g_assassinNative = 1;
volatile long g_assassinNativeHits = 0;

extern "C" void CombatAssassinAddNative(void* frame) {
    if (g_assassinNative == 0 || g_assassinOpen == 0) {
        return;
    }
    char* f = static_cast<char*>(frame);
    if (*reinterpret_cast<int*>(f - 0x28) != 0x00406849) {
        return;   // 4221001 only
    }
    typedef int (__cdecl *NativeCritFn)(void*, int, int, int*, int*);
    static NativeCritFn const nativeCrit = reinterpret_cast<NativeCritFn>(0x00765095);
    int rate = 0;
    int dmg = 0;
    nativeCrit(*reinterpret_cast<void**>(f + 0x08),
               *reinterpret_cast<int*>(f + 0x24),
               *reinterpret_cast<int*>(f + 0x2c),
               &rate, &dmg);
    if (rate > 0) {
        *reinterpret_cast<int*>(f - 0x40) += rate;
    }
    if (dmg > 0) {
        *reinterpret_cast<int*>(f - 0x34) += dmg;
    }
    // R40: log unconditionally. The old `rate > 0 || dmg > 0` guard meant a
    // short-sword (blade) user never saw a line at all, which read as "the
    // hook did not fire" -- 420's base really is 0/0 and that IS the answer.
    if (g_probeEnabled != 0) {
        const long n = InterlockedIncrement(&g_assassinNativeHits);
        if (n <= 24 || (n % 20) == 0) {
            char line[192];
            wsprintfA(line, "[combat] ASSASSINNATIVE add rate=%d dmg=%d tot=%d/%d j=%d n=%d\r\n",
                      rate, dmg,
                      *reinterpret_cast<int*>(f - 0x40),
                      *reinterpret_cast<int*>(f - 0x34),
                      *reinterpret_cast<int*>(f + 0x24),
                      static_cast<int>(n));
            OutputDebugStringA(line);
        }
    }
}

extern "C" __declspec(naked) void CombatAssassinNativeCave(void) {
    __asm {
        pushad
        push ebp
        call CombatAssassinAddNative
        add  esp, 4
        popad
        push dword ptr [ebx + 0x5e4]
        mov  eax, 0x0078E166
        jmp  eax
    }
}

// ---------------------------------------------------------------------------
// R38 A4: 4111004 (money attack) has its OWN damage + crit branch inside
// PDamage and never reaches the shared accumulators.
//
// PDamage selects a per-skill damage branch at 0x78F860. 4111004 owns
// 0x78F897..0x78F9FA:
//   x    = decode(skill + 0xAC / +0xB4)                 ; WZ `x`, lv30 = 50
//   lo   = x * 0.5 (0xAF0D48), hi = x * 1.5 (0xAFE808)  ; u = rand in [lo,hi]
//   dmg  = ftol(x*u) * 10                               ; lea eax,[eax+eax*4];shl
//   roll = (rand % 1e7) * 1e-5                          ; 0xAFE838 -> 0..100
//   crit = decode(skill + 0xF4 / +0xFC) > roll          ; WZ `prop`, lv30 = 10
//   mult = (decode(skill + 0x13C / +0x144) + 100)*0.01  ; = 1.00 (node absent)
// It then jumps to 0x78FE9A, i.e. it never executes 0x7900EE. R37's A3 (imm32
// @0x7900FA plus A2's "force native") therefore could not have changed
// anything: 4111004 kept its fixed 10% roll and its x1.00 "crit" -- the WZ
// description's "+50%" only ever produced a yellow number.
// A4 hooks the branch itself:
//   4A @ 0x78F9BE (6B, `jbe 0x78FE9A`): with mesoAttack the verdict comes from
//      the shared accumulators -- [ebp-0x40] rate against the roll already in
//      [ebp-0x0C] -- instead of the skill's own prop.
//   4B @ 0x78F9D6 (6B, `add eax,0x64` + `mov [ebp-0x10],eax`): the multiplier
//      input becomes [ebp-0x34] (total crit damage, 240 with sharp eyes), so
//      the crit is a real x2.4 instead of x1.00.
// Both caves replay the original instructions when mesoAttack is 0.
// ---------------------------------------------------------------------------
volatile long g_mesoAtkLogHits = 0;
// Effective WZ-half multiplier for the current 4111004 row, computed by 4A and
// consumed by 4B (which runs a few instructions later for the very same row).
volatile int g_mesoAtkWzCd = 0;

typedef int (__cdecl *MesoDecodeFn)(void* buf, int len);

// The client keeps decrypted skill numbers as (buffer, length) pairs eight
// bytes apart: 0xAC/0xB4 = `x`, 0xF4/0xFC = `prop`, 0x13C/0x144 =
// `criticalDamage`. 0x416563 is the decoder used by the original code.
static int DecodeSkillInt(void* skill, int off) {
    char* s = static_cast<char*>(skill);
    return reinterpret_cast<MesoDecodeFn>(0x00416563)(
        s + off, *reinterpret_cast<int*>(s + off + 8));
}

extern "C" int CombatMesoAttackCrit(void* frame, void* skillPtr) {
    char* f = static_cast<char*>(frame);
    const int skill = *reinterpret_cast<int*>(f - 0x28);
    const double roll = *reinterpret_cast<double*>(f - 0x0c);
    const int prop = *reinterpret_cast<int*>(f - 0x10);
    if (g_mesoAttack != 0 && skill == 0x003EBA9C) {
        // R39: stack, do not replace. prop is the skill's own WZ rate (lv30 =
        // 10, i.e. the description's "10% chance"), [ebp-0x40] is the shared
        // total built at 0x78E0E6..0x78E369 (weapon-typed native base + sharp
        // eyes + stun mastery 5110000 + war god 21110000 + N1's C2 add).
        // Also resolve the WZ half of the multiplier here so 4B stays tiny and
        // the probe can report every input at once.
        int wzX = 0;
        int wzCd = 0;
        if (skillPtr != 0) {
            wzX = DecodeSkillInt(skillPtr, 0xAC);
            wzCd = DecodeSkillInt(skillPtr, 0x13C);
        }
        int eff = wzCd;
        if (eff <= 0) {
            eff = (g_mesoAtkCd < 0) ? wzX : g_mesoAtkCd;
        }
        g_mesoAtkWzCd = eff;
        const int shared = *reinterpret_cast<int*>(f - 0x40);
        const int rate = prop + (shared > 0 ? shared : 0);
        const int cd = *reinterpret_cast<int*>(f - 0x34);
        const int crit = (rate > 0 && roll < static_cast<double>(rate)) ? 1 : 0;
        if (g_probeEnabled != 0) {
            const long n = InterlockedIncrement(&g_mesoAtkLogHits);
            // R40: every crit row is printed (up to 40) so a red number can be
            // matched line by line; misses are sampled every 10th.
            if ((crit != 0 && n <= 40) || (n % 10) == 0) {
                char line[256];
                // x= is the WZ `x` (also the damage base), wzcd= is the decoded
                // `criticalDamage` (0 = node absent), eff= is what 4B will use.
                wsprintfA(line,
                          "[combat] MESOATK crit=%d prop=%d shared=%d rate=%d cd=%d x=%d wzcd=%d eff=%d roll=%d n=%d\r\n",
                          crit, prop, shared, rate, cd, wzX, wzCd, eff,
                          static_cast<int>(roll), static_cast<int>(n));
                OutputDebugStringA(line);
            }
        }
        return crit;
    }
    return (roll < static_cast<double>(prop)) ? 1 : 0;
}

extern "C" __declspec(naked) void CombatMesoAttackCritCave(void) {
    __asm {
        pushad
        push edi
        push ebp
        call CombatMesoAttackCrit
        add  esp, 8
        test eax, eax
        popad
        jnz  meso_crit
        mov  eax, 0x0078FE9A
        jmp  eax
    meso_crit:
        mov  eax, 0x0078F9C4
        jmp  eax
    }
}

extern "C" __declspec(naked) void CombatMesoAttackDmgCave(void) {
    __asm {
        cmp  dword ptr [g_mesoAttack], 0
        je   meso_legacy
        // R39: stack the two multipliers instead of swapping them.
        // eax = decode(skill + 0x13C) = WZ criticalDamage (0 when the node is
        // absent, which is the case for 4111004 today -> use the fallback).
        test eax, eax
        jg   meso_wz
        mov  eax, dword ptr [g_mesoAtkWzCd]
    meso_wz:
        // [ebp-0x34] is the shared total crit damage (100 = x1.0), already
        // containing sharp eyes / stun / war god / C2 / the weapon base.
        mov  ecx, [ebp - 0x34]
        test ecx, ecx
        jg   meso_sum
        mov  ecx, 100
    meso_sum:
        add  eax, ecx
        mov  [ebp - 0x10], eax
        mov  eax, 0x0078F9DC
        jmp  eax
    meso_legacy:
        add  eax, 0x64
        mov  [ebp - 0x10], eax
        mov  eax, 0x0078F9DC
        jmp  eax
    }
}

// ---------------------------------------------------------------------------
// R33 S5: shadow-partner rows for non-ranged attacks. PDamage already owns the
// folding maths (0x7901DD): once [ebp+0x34] is set it rebuilds every row
// j >= n/2 from row j-n/2 at the shadow coefficient read out of WZ, and R29's S1
// cave keeps its hands off those rows (SHADOWSKIP). All that is missing for melee
// is that the caller never doubles n and never sets the flag, so this entry cave
// does exactly those two writes.
// The row buffer is the 15-int array at attack+0x18 (the crit-flag array sits at
// +0x3c = 60 bytes = 15 ints), so we refuse to grow past 15 rows.
// ---------------------------------------------------------------------------
volatile long g_shadowLogHits = 0;

extern "C" void CombatShadowExpand(void* frame) {
    if (g_shadowMelee == 0 || CombatHooks::g_shadowActive == 0) {
        return;
    }
    unsigned char* f = reinterpret_cast<unsigned char*>(frame);
    int* flag = reinterpret_cast<int*>(f + 0x34);
    int* rows = reinterpret_cast<int*>(f + 0x20);
    if (*flag != 0) {
        return;   // ranged path already folded its own rows
    }
    // R36 U1: when the caller is the melee pipeline A (0x950747) or B (0x953760)
    // its upper cave has already doubled the row count, so only the fold flag is
    // still missing. Doubling here as well would quadruple the rows.
    const unsigned int ret = reinterpret_cast<unsigned int*>(f)[1];
    if (g_shadowBcast != 0 && (ret == 0x00951E47u || ret == 0x00954CF5u)) {
        *flag = 1;
        return;
    }
    const int m = *rows;
    if (m < 1 || m > 7) {
        if (g_probeEnabled != 0) {
            const long n = InterlockedIncrement(&g_shadowLogHits);
            if (n <= 20) {
                char skip[128];
                wsprintfA(skip, "[combat] SHADOWEXP skip rows=%d (buffer=15)\r\n", m);
                OutputDebugStringA(skip);
            }
        }
        return;   // 2*m must stay inside the 15-int buffer
    }
    *rows = m * 2;
    *flag = 1;
    if (g_probeEnabled != 0) {
        const long n = InterlockedIncrement(&g_shadowLogHits);
        if (n <= 20) {
            char line[160];
            wsprintfA(line, "[combat] SHADOWEXP rows %d -> %d shdw=%d\r\n",
                      m, m * 2, static_cast<unsigned>(CombatHooks::g_shadowActive));
            OutputDebugStringA(line);
        }
    }
}

extern "C" __declspec(naked) void CombatShadowExpandCave(void) {
    __asm {
        pushad
        push ebp
        call CombatShadowExpand
        add  esp, 4
        popad
        sub  esp, 0x1a8
        mov  eax, 0x0078DF90
        jmp  eax
    }
}

// ---------------------------------------------------------------------------
// R36 U1: melee shadow-partner BROADCAST (damage numbers) and apply.
// PDamage only fills the row array and returns (0x79060D `ret 0x50`); it never
// draws a number and never builds the attack packet. Both live one layer up and
// are driven by a single per-frame row count:
//   A 0x950747: count [ebp-0x58] -> PDamage arg7 0x951e27,
//               number loop 0x9527d4, apply 0x9527e7
//   B 0x953760: count [ebp-0x18] -> PDamage arg7 0x954cc8,
//               number loop 0x9550cf, apply 0x9550e6
// Doubling that one slot once per invocation makes compute, broadcast and apply
// agree. Both caves sit on the 6-byte `jle` that guards the target loop, so the
// doubling happens once per attack, not once per target.
// A always passes arg12 = 0 (esi is zeroed at 0x951cc0) so it needs no check;
// B passes its own flag from [ebp-0xd8] and must be skipped when already set.
// The packet then carries 2n rows, so the server side (ADH segment count and
// calcDmgMax) has to accept them -- see the joint plan doc.
// ---------------------------------------------------------------------------
volatile long g_shadowUpLogHits = 0;

static void ShadowExpandUpper(void* frame, int countOff, int flagOff, const char* tag) {
    if (g_shadowBcast == 0 || g_shadowMelee == 0 || CombatHooks::g_shadowActive == 0) {
        return;
    }
    unsigned char* f = reinterpret_cast<unsigned char*>(frame);
    if (flagOff != 0 && *reinterpret_cast<int*>(f + flagOff) != 0) {
        return;   // this path already folds its own rows
    }
    int* rows = reinterpret_cast<int*>(f + countOff);
    const int m = *rows;
    if (m < 1 || m > 7) {
        if (g_probeEnabled != 0) {
            const long n = InterlockedIncrement(&g_shadowUpLogHits);
            if (n <= 20) {
                char skip[128];
                wsprintfA(skip, "[combat] SHADOWUP %s skip rows=%d (buffer=15)\r\n", tag, m);
                OutputDebugStringA(skip);
            }
        }
        return;   // 2*m must stay inside the 15-int row buffer
    }
    *rows = m * 2;
    if (g_probeEnabled != 0) {
        const long n = InterlockedIncrement(&g_shadowUpLogHits);
        if (n <= 20) {
            char line[160];
            wsprintfA(line, "[combat] SHADOWUP %s rows %d -> %d shdw=%d\r\n",
                      tag, m, m * 2, static_cast<unsigned>(CombatHooks::g_shadowActive));
            OutputDebugStringA(line);
        }
    }
}

extern "C" void CombatShadowUpperA(void* frame) {
    ShadowExpandUpper(frame, -0x58, 0, "A");
}

extern "C" void CombatShadowUpperB(void* frame) {
    ShadowExpandUpper(frame, -0x18, -0xd8, "B");
}

extern "C" __declspec(naked) void CombatShadowUpperACave(void) {
    __asm {
        pushad
        push ebp
        call CombatShadowUpperA
        add  esp, 4
        popad
        cmp  dword ptr [ebp - 0x14], 0
        mov  eax, 0x00951CA2
        jle  upper_a_done
        mov  eax, 0x00951C12
    upper_a_done:
        jmp  eax
    }
}

extern "C" __declspec(naked) void CombatShadowUpperBCave(void) {
    __asm {
        pushad
        push ebp
        call CombatShadowUpperB
        add  esp, 4
        popad
        cmp  dword ptr [ebp + 0x1c], 0
        mov  eax, 0x00954D2E
        jle  upper_b_done
        mov  eax, 0x00954C69
    upper_b_done:
        jmp  eax
    }
}

// ---------------------------------------------------------------------------
// R33 M1: Meso Explosion (4211006) runs on its own damage routine, not PDamage.
// 0x791FBC is that routine (single caller 0x96B97E). Its per-meso value is built
// at 0x79211D..0x792131 and converted with `call 0xa62018` (_ftol) at 0x792134;
// the native tail is `fstp st(0)` (0x792139) then `mov [esi],eax` (0x79213B).
// We hook the _ftol call (5 bytes) and scale ST0 by the C2 finalDamagePercent
// before the conversion. x87 bookkeeping is preserved exactly: _ftol pops the
// converted value, so the duplicate left by `fmul st(1)` is what the native
// `fstp st(0)` removes -- we resume at that fstp, not after it.
// ---------------------------------------------------------------------------
volatile long g_mesoLogHits = 0;
volatile long g_mesoCrits = 0;

// R33 M1c: Meso Explosion has no native crit roll at all (its routine never
// calls 0x765095 and its WZ node has no crit field), so the aggregated C2 crit
// has to be rolled here, once per meso, in native terms: total multiplier is
// critDmg/100 (critDmg is the total crit percent, see R32 4).
// R37: the cave also forwards the game frame, because the two numbers that
// explain every "damage swings wildly" report live there and nowhere else:
//   [ebp+0x18] = 50 * L (the per-meso base, L decoded from an obfuscated int)
//   [ebp+0x20] = pointer to this meso's value v
// The whole native formula (0x79211D..0x792131) is
//   dmg = 50*L * (0.5 + r/19999997.6) * f(v),  r in [0,10000000)
//   f(v) = (0.82v + 28) / 5300      for v <= 1000
//        = v / (v + 5250)           for v >  1000
// i.e. a flat 0.5..1.0 random times a meso-value curve that saturates at 1.0.
// Nothing here rolls a crit: 4211006 never calls 0x765095.
extern "C" void CombatMesoScale(double* value, const void* frame) {
    if (g_mesoFinal == 0) {
        return;
    }
    const int finalPct = CombatHooks::g_combat[2];
    const int critRate = CombatHooks::g_combat[0];
    const int critDmg = CombatHooks::g_combat[1];
    int crit = 0;
    if (critRate > 0 && critDmg > 100) {
        if (critRate >= 100 || static_cast<int>(NextRand() % 100u) < critRate) {
            crit = 1;
        }
    }
    if (finalPct > 0) {
        *value *= (1.0 + static_cast<double>(finalPct) / 100.0);
    }
    if (crit != 0) {
        *value *= (static_cast<double>(critDmg) / 100.0);
        InterlockedIncrement(&g_mesoCrits);
    }
    // R37: log even when nothing was applied. The old early return made an
    // unbuffed run print NOTHING, which reads exactly like "the hook is gone"
    // and hid the base/meso columns for the very test that needs them.
    if (g_probeEnabled != 0) {
        const long n = InterlockedIncrement(&g_mesoLogHits);
        // wsprintf has no float support: a %f specifier misaligns every later
        // argument, so the damage is printed as an int and never as a double.
        // R37: every 10th row (was 50) and the base/meso columns, so a gap in
        // the HIT sequence can no longer be mistaken for a lost hook.
        if (n <= 30 || (n % 10) == 0) {
            int base = -1;
            int meso = -1;
            const char* f = static_cast<const char*>(frame);
            if (f != 0) {
                __try {
                    base = FrameInt(f, 0x18);
                    const int* p = *reinterpret_cast<const int* const*>(f + 0x20);
                    if (p != 0) {
                        meso = *p;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    base = -2;
                    meso = -2;
                }
            }
            char line[224];
            wsprintfA(line,
                      "[combat] MESOFINAL n=%d pct=%d crit=%d rate=%d cd=%d base=%d meso=%d dmg=%d crits=%d\r\n",
                      static_cast<int>(n), finalPct, crit, critRate, critDmg,
                      base, meso, static_cast<int>(*value),
                      static_cast<int>(g_mesoCrits));
            OutputDebugStringA(line);
        }
    }
}

extern "C" __declspec(naked) void CombatMesoFinalCave(void) {
    __asm {
        sub  esp, 8
        fstp qword ptr [esp]
        pushad
        push dword ptr [esp + 8]
        lea  eax, [esp + 36]
        push eax
        call CombatMesoScale
        add  esp, 8
        popad
        fld  qword ptr [esp]
        add  esp, 8
        mov  eax, 0x00A62018
        call eax
        push 0x00792139
        ret
    }
}

namespace CombatDamage {

static bool PatchSite(void* cave, DWORD va, const char* tag, int patchLen = 6) {
    Memory::CodeCave(cave, va, patchLen);
    // CodeCave swallows its own exceptions, so a failed VirtualProtect would
    // otherwise look like success. Read the first byte back: E9 means armed.
    const unsigned char first = *reinterpret_cast<const unsigned char*>(va);
    const bool ok = (first == 0xE9);
    char line[160];
    wsprintfA(line, "[combat] cave %s %s @%08X first=%02X probe=%d budget=%d all=%d mode=%d\r\n",
              tag, ok ? "armed" : "FAILED", va,
              static_cast<unsigned>(first), g_probeEnabled,
              static_cast<int>(g_probeBudget), g_probeAll, g_critMode);
    OutputDebugStringA(line);
    return ok;
}

// R33: rewrite one imm32 inside an existing instruction. Instruction length is
// unchanged, so no code cave is needed. The old value is verified first: if the
// base or the build differs we leave the byte alone and log FAILED.
static bool PatchImm32(DWORD va, unsigned int expect, unsigned int value, const char* tag) {
    DWORD oldProtect = 0;
    const unsigned int before = *reinterpret_cast<const unsigned int*>(va);
    bool ok = false;
    if (before == expect &&
        VirtualProtect(reinterpret_cast<void*>(va), sizeof(unsigned int), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        *reinterpret_cast<unsigned int*>(va) = value;
        VirtualProtect(reinterpret_cast<void*>(va), sizeof(unsigned int), oldProtect, &oldProtect);
        ok = (*reinterpret_cast<const unsigned int*>(va) == value);
    }
    char line[160];
    wsprintfA(line, "[combat] imm32 %s %s @%08X before=%08X after=%08X\r\n",
              tag, ok ? "armed" : "FAILED", va, before,
              static_cast<unsigned>(*reinterpret_cast<const unsigned int*>(va)));
    OutputDebugStringA(line);
    return ok;
}

bool Install(bool enableProbe, int probeBudget, int siteMask, int critMode, int shadowMelee,
             int mesoFinal, int shadowBcast, int mesoAttack, int assassinNative, int mesoAttackCd,
             int critFix) {
    g_probeEnabled = enableProbe ? 1 : 0;
    // Negative budget switches on "print every row" (raw damage distribution).
    g_probeAll = (probeBudget < 0) ? 1 : 0;
    g_probeBudget = (probeBudget < 0) ? -probeBudget : probeBudget;
    // R33: critMode 3 == mode 2 (native add) + Assassinate all-segment crit.
    g_assassinOpen = (critMode == 3) ? 1 : 0;
    g_shadowMelee = (shadowMelee == 1) ? 1 : 0;
    g_shadowBcast = (shadowBcast == 1) ? 1 : 0;
    g_mesoFinal = (mesoFinal == 1) ? 1 : 0;
    g_mesoAttack = (mesoAttack == 1) ? 1 : 0;
    // > 0 fixed, = 0 off, < 0 follow the skill's WZ `x` (R39).
    g_mesoAtkCd = mesoAttackCd;
    g_assassinNative = (assassinNative != 0) ? 1 : 0;
    g_critFix = (critFix != 0) ? 1 : 0;
    g_critMode = (critMode == 1) ? 1 : ((critMode == 2 || critMode == 3) ? 2 : 0);
    if (siteMask <= 0) {
        siteMask = 1;
    }
    // R34: print the effective switch set once, so a value like damageCrit=2
    // (which silently leaves A1/A2 unarmed) is visible in the very first frame
    // instead of being guessed from the missing armed lines.
    {
        char cfg[256];
        wsprintfA(cfg,
                  "[combat] config crit=%d(mode=%d assassin=%d asnNat=%d fix=%d) sites=%d shadow=%d bcast=%d meso=%d mesoAtk=%d(cd=%d) probe=%d\r\n",
                  critMode, g_critMode, g_assassinOpen, g_assassinNative, g_critFix, siteMask,
                  g_shadowMelee, g_shadowBcast, g_mesoFinal, g_mesoAttack, g_mesoAtkCd,
                  g_probeEnabled);
        OutputDebugStringA(cfg);
        if (critMode == 2) {
            OutputDebugStringA("[combat] note: A1/A2 need damageCrit=3, mode 2 alone skips them\r\n");
        }
    }

    bool armed = false;
    __try {
        if (siteMask & 1) {
            armed = PatchSite(reinterpret_cast<void*>(&CombatDamageCaveSite1), 0x00790586, "S1/PDamage") || armed;
        }
        if (siteMask & 2) {
            armed = PatchSite(reinterpret_cast<void*>(&CombatDamageCaveSite2), 0x00791F8B, "S2/C1") || armed;
        }
        if (siteMask & 4) {
            armed = PatchSite(reinterpret_cast<void*>(&CombatDamageCaveSite3), 0x0079343F, "S3/C3") || armed;
        }
        if (siteMask & 8) {
            armed = PatchSite(reinterpret_cast<void*>(&CombatDamageCaveSite4), 0x00793668, "S4/C4") || armed;
        }
        // Mode 2 arms the native crit accumulator patch. This is independent
        // of the four output caves: mode 2 needs NO multiplier at the output,
        // only a changed native accumulator before PDamage's own roll.
        if (g_critMode == 2) {
            armed = PatchSite(reinterpret_cast<void*>(&CombatNativeCritCave),
                              0x0078E369, "N1/native-crit", 7) || armed;
            // R31: mage magic skills travel through C1, not PDamage. N2 adds
            // the same C2 values to C1's own accumulators. Independent of
            // siteMask: this is an input-side patch, not an output cave.
            armed = PatchSite(reinterpret_cast<void*>(&CombatNativeCritC1Cave),
                              0x00791859, "N2/C1-native-crit", 6) || armed;
        }
        if (g_assassinOpen) {
            // 0x790100  81 7D D8 imm32  =>  cmp [ebp-0x28], 0x406849
            // imm32 sits at 0x790103, NOT 0x790106 (that is the `jne`).
            // Impossible id => every 4221001 row falls through to 0x790113.
            armed = PatchImm32(0x00790103, 0x00406849u, 0x7FFFFFFFu, "A1/assassinate-3hit") || armed;
            // A2 makes those rows take the WZ crit-param path, so the first
            // three segments get the same 90/250 the final blow uses.
            armed = PatchSite(reinterpret_cast<void*>(&CombatAssassinWzCave),
                              0x0078E0FF, "A2/assassinate-wz", 6) || armed;
            // R38 AN1: add the 0x765095 native base on top of the WZ 90/250.
            // For a dagger the base is 0, so 420 is unchanged; for a claw /
            // bow / knuckle build casting 4221001 the base is real.
            if (g_assassinNative) {
                armed = PatchSite(reinterpret_cast<void*>(&CombatAssassinNativeCave),
                                  0x0078E160, "AN1/assassin-native", 6) || armed;
            }
        }
        if (g_critFix) {
            // R40 A5: 0x7901A9  DC 0D F0 14 AF 00  =>  fmul [0xaf14f0]
            // Replaces the snapshot-based add with `dmg *= cd/100` on the live
            // total. Also the CRITHIT probe: every successful crit passes here.
            armed = PatchSite(reinterpret_cast<void*>(&CombatCritScaleCave),
                              0x007901A9, "A5/crit-scale", 6) || armed;
        }
        if (g_shadowMelee) {
            // S5 sits on PDamage's `sub esp,0x1a8` prologue and re-executes it.
            armed = PatchSite(reinterpret_cast<void*>(&CombatShadowExpandCave),
                              0x0078DF8A, "S5/shadow-melee", 6) || armed;
            if (g_shadowBcast) {
                // R36 U1: raise the upper-layer row count once per attack so the
                // damage-number loop and the apply/packet call follow the extra
                // rows. Both sites are the 6-byte `jle` guarding the target loop.
                armed = PatchSite(reinterpret_cast<void*>(&CombatShadowUpperACave),
                                  0x00951C0C, "U1/shadow-bcast-A", 6) || armed;
                armed = PatchSite(reinterpret_cast<void*>(&CombatShadowUpperBCave),
                                  0x00954C63, "U1/shadow-bcast-B", 6) || armed;
            }
        }
        if (g_mesoFinal) {
            // M1 = the `call _ftol` inside the Meso Explosion routine (5 bytes).
            armed = PatchSite(reinterpret_cast<void*>(&CombatMesoFinalCave),
                              0x00792134, "M1/meso-final", 5) || armed;
        }
        if (g_mesoAttack) {
            // R38 A4: hook 4111004's own branch, not the shared gate. R37's A3
            // (imm32 @0x7900FA + A2 "force native") was a dead end: the branch
            // at 0x78F897 computes its own damage and crit, then jumps to
            // 0x78FE9A without ever reaching 0x7900EE. The imm32 is therefore
            // NOT patched any more (keeping it would leave a latent
            // double-crit if a future build routes the skill through the
            // shared gate).
            // 4A: 0x78F9BE  0F 86 D6 04 00 00  =>  jbe 0x78FE9A
            armed = PatchSite(reinterpret_cast<void*>(&CombatMesoAttackCritCave),
                              0x0078F9BE, "A4/meso-attack-crit", 6) || armed;
            // 4B: 0x78F9D6  83 C0 64 89 45 F0  =>  add eax,0x64 / mov [ebp-0x10],eax
            armed = PatchSite(reinterpret_cast<void*>(&CombatMesoAttackDmgCave),
                              0x0078F9D6, "A4/meso-attack-mult", 6) || armed;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return armed;
}

} // namespace CombatDamage
