#pragma once

// CombatHooks.h - client-side recv-packet dispatch for the combat revamp.
//
// Layout contract (must stay in sync with HpMpAlert.cpp):
//   CInPacket::Data points at the packet buffer, Data[4..5] is the
//   little-endian opcode, payload starts at Data[6]. PacketLen includes the
//   four header bytes and the two opcode bytes.
//
// Layout proof: CInPacket::GetType @0x42470C does
//     mov edx, [ecx+0x14]              ; Offset
//     mov eax, [ecx+0x08]              ; Data
//     movzx esi, word ptr [ecx+0x0C]   ; length  <-- 16 bit, not 32
//     sub esi, edx / add eax, edx
//     mov ax, word ptr [eax]           ; opcode
//     add edx, 2 / mov [ecx+0x14], edx ; Offset += 2
// so the length MUST be read as unsigned short. Reading it as a 32-bit value
// pulls the neighbouring +0x0E field into the high half and yields eight-digit
// lengths (R15 T1 observed len=3855876127). Never widen this field again.
//
// IMPORTANT: 0x004965F1 already has exactly one Memory::SetHook in
// HpMpAlert.cpp (HookHpMpAlertRecv). All new cases MUST be dispatched from
// that hook. Do not add a second SetHook on that address.

struct CInPacket {
    bool Loopback;              // +0x00
    int State;                  // +0x04
    void* Data;                 // +0x08  buffer base (opcode lives at +4)
    unsigned short PacketLen;   // +0x0C  total length, 16-bit, header included
    unsigned short RawSeq;      // +0x0E
    unsigned short Field10;     // +0x10
    unsigned short Field12;     // +0x12
    unsigned int Offset;        // +0x14  read cursor, starts at 4
    void* Unk;                  // +0x18
};

namespace CombatHooks {

// Globals are read by absolute address from codecaves (A2A3 section 2.4).
// Keep them in this translation unit and keep the order stable.
extern volatile unsigned char g_shadowActive;   // 1 while SHADOWPARTNER is on
extern volatile int g_combat[6];                // critRate, critDmg, finalDmg, bossDmg, ignoreDef, growthPercent
extern volatile unsigned long g_trace[6];       // diagnostic counters, see CombatHooks.cpp

// Orb count, parsed from the plain GIVE_BUFF(0x20) packet (see R14).
// g_comboOrbs is what the finisher formula must use: server-side
// numFinisherOrbs = comboBuff - 1 (CloseRangeDamageHandler:91).
extern volatile int g_comboBuff;                // raw BuffStat.COMBO value, 0..11
extern volatile int g_comboOrbs;                // max(0, g_comboBuff - 1)

// Called once per inbound packet from the existing ProcessPacket_Hook.
void DispatchRecvPacket(CInPacket* packet);

} // namespace CombatHooks
