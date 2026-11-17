#pragma once

// CombatDamage.h - damage-pipeline injection (client side).
//
// Four landing zones, one bit each (see CombatDamage.cpp for the address table
// and for why the 5th/6th compare must stay untouched - they sit downstream of
// C3/C4 and patching them as well would multiply twice):
//
//   bit0 0x00790586  PDamage   (per-row loop, tested 11-13)
//   bit1 0x00791F8B  C1        (0x00791617)
//   bit2 0x0079343F  C3        (0x0079309F)
//   bit3 0x00793668  C4        (0x0079345E)
//
// Everything reads CombatHooks::g_combat (critRate / critDmg / finalPct), so
// the 0x373E packet must arrive first. With g_combat all zero this is a strict
// no-op: the caves still run, but every multiplier is skipped.
//
// `enableProbe` only controls the DebugView lines; the multiply runs either
// way. `probeBudget` caps how many PRINTED rows appear (the first hit of the
// session is always printed; no-op rows are skipped and never spend budget).
// A NEGATIVE probeBudget means "print every row including no-op rows" and uses
// its absolute value as the cap. That mode exposes the raw `in` distribution,
// which is the decisive test for a native client-side crit roll (R18 5).

namespace CombatDamage {

// Returns true if at least one requested site was really patched (the first
// byte of each site is read back and must be 0xE9).
//
// `critMode` (R23):
//   0 = off. Strictly no-op for critRate/critDmg; keeps the 11-13 baseline.
//   1 = legacy per-row random roll (a SECOND crit roll on top of the native
//       one; produces 4 damage tiers and lifts expected damage). A/B only.
//   2 = scheme B / native entry. Arms TWO input-side caves:
//       N1 @ 0x0078E369 (7 bytes) -> PDamage accumulators [ebp-0x40]/[ebp-0x34]
//          before the single native roll at 0x79018E (R23).
//       N2 @ 0x00791859 (6 bytes) -> C1 accumulators [ebp+0xC]/[ebp-0x24]
//          before C1's single roll at 0x791DB1 (R31, mage magic skills).
//       Exactly one roll per pipeline, both values additive, native logic
//       preserved. C3/C4 are NOT hooked (measured dead exits, R29 4.1).
//       Both caves are independent of siteMask (they are not output caves).
//   3 = mode 2 PLUS Assassinate (4221001) all-segment crit (R33). Two extra
//       hooks on top of N1/N2:
//         A1 @ 0x00790106 imm32: stops 4221001 entering the
//            "cmp [ebp+0x30],0x3C" gate at 0x790109, so every segment reaches
//            the crit block at 0x790113.
//         A2 @ 0x0078E0FF (6 bytes, the `je 0x78E11F`): forces the WZ crit-param
//            path for 4221001 rows whose marker is not 0x3C, so the first three
//            segments use the same WZ prop/criticalDamage as the final blow.
//       3211003 / 4111004 keep the original skip unless mesoAttack is on.
// `mesoAttack` (R39, was R37 A3 / R38 A4) is independent: 1 = 4111004 (money
// attack) STACKS the native aggregate on top of its own WZ pair instead of
// replacing it. The skill owns a private branch (0x78F897..0x78F9FA) that never
// reaches the shared gate at 0x7900EE, so it is hooked inside that branch:
//   rate = WZ `prop` (lv30 = 10) + [ebp-0x40] (native base + sharp eyes + stun
//          mastery 5110000 + war god 21110000 + N1's C2);
//   mult = WZ `criticalDamage` (absent -> `mesoAttackCd`, default 50) +
//          [ebp-0x34] (shared total crit damage, 100 = x1.0).
// Both terms are additive, exactly like 4221001 after AN1. Every native source
// is therefore kept AND the description's "10% chance, +50%" becomes real.
// 0 restores the untouched 10% / x1.0 (yellow number only).
// `mesoAttackCd` (R39) is the fallback WZ crit damage for 4111004 while its WZ
// node is still missing; a real `criticalDamage` node always wins over it.
// 0 disables the fallback (pure native multiplier).
// `shadowMelee` (R33) is an independent switch: 1 = PDamage adds shadow-partner
// rows for non-ranged attacks (cave at 0x0078DF8A). Native ranged folding is
// untouched because the cave only fires when [ebp+0x34] == 0.
// `shadowBcast` (R36 U1) rides on shadowMelee: 1 = also double the upper-layer
// row count so the damage-number loop and the apply/packet call follow. Two
// caves, one per melee pipeline (A 0x00951C0C count [ebp-0x58], B 0x00954C63
// count [ebp-0x18]); each sits on the 6-byte `jle` guarding the target loop so
// the doubling happens once per attack. 0 keeps the old compute-only behaviour.
// NOTE: the attack packet then carries 2n rows, so the server must accept them.
// `mesoFinal` (R33) is independent: 1 = Meso Explosion (4211006) joins the
// aggregate, i.e. finalDamagePercent AND the C2 crit are applied. Its damage is
// computed by its own routine at 0x791FBC (cave at 0x00792134, the _ftol call);
// PDamage never sees that skill and the skill has no native crit at all.
// Crit is rolled once per meso with total = critDmg/100 (R32 4). Note: the
// client merges its damage numbers into 5 buckets, so there is no per-meso
// yellow crit number -- crit only shows up as a bigger value.
// `assassinNative` (R38 AN1, default 1, only meaningful with critMode 3): add
// the weapon-typed native crit base (0x765095) on top of the WZ 90/250 that A2
// already gives 4221001. Sources that live after the two branches merge (sharp
// eyes 0x78E160, stun mastery 5110000 0x78E1CE, war-god 21110000 0x78E254,
// N1's C2 0x78E369) are therefore already shared -- AN1 only closes the base.
// 0x765095 returns 0/0 for daggers, so a 420 is numerically unchanged; claw
// (14100001), bow/xbow (13000000) and knuckle (15110000) builds gain theirs.
// 0 switches the cave off and restores the pure WZ behaviour.
bool Install(bool enableProbe = true, int probeBudget = 40, int siteMask = 1,
             int critMode = 0, int shadowMelee = 0, int mesoFinal = 0,
             int shadowBcast = 1, int mesoAttack = 0, int assassinNative = 1,
             int mesoAttackCd = 50, int critFix = 1);

} // namespace CombatDamage
