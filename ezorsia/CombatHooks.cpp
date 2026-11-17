#include "stdafx.h"
#include "CombatHooks.h"
#include <cstring>

// CombatHooks.cpp - recv-packet dispatch for combat revamp (first drop).
//
// Design rules (R9 / R10 / R13):
//   * One hook only: HpMpAlert.cpp already calls Memory::SetHook on 0x004965F1.
//   * g_shadowActive is a pure mirror of server buff packets. The DLL does NOT
//     invent its own death / expiry / map-change transitions.
//   * Death clears the flag because Character.playerDead():7013 calls
//     cancelAllBuffs(false), which emits CANCEL_BUFF(0x21) with the
//     SHADOWPARTNER bit set. respawn():7124 emits it a second time.
//   * Logout is the one case with no server packet (socket already gone), so
//     the flag is cleared at the next session boundary (login-server packets).
//   * Map change (0x7D) and channel change (0x10) never touch the flag:
//     SHADOWPARTNER has a duration and keeps ticking across them.
//   * Combat-stats packet accepts both 1-byte and 4-byte version layouts by
//     total length, so a server-side writeByte / writeInt mismatch can never
//     silently shift all six ints by three bytes.

namespace CombatHooks {
volatile unsigned char g_shadowActive = 0;
volatile int g_combat[6] = { 0, 0, 0, 0, 0, 0 };
volatile unsigned long g_trace[6] = { 0, 0, 0, 0, 0, 0 };
volatile int g_comboBuff = 0;
volatile int g_comboOrbs = 0;
}

namespace {

enum TraceSlot {
    kTraceGiveBuff     = 0,
    kTraceCancelBuff   = 1,
    kTraceSessionReset = 2,
    kTraceCombatStats  = 3,
    kTraceReject       = 4,
    kTraceCombo        = 5
};

// SendOpcode values, from gms-server .../net/opcodes/SendOpcode.java
constexpr unsigned short kOpLoginStatus  = 0x0000;
constexpr unsigned short kOpServerList   = 0x000A;
constexpr unsigned short kOpCharList     = 0x000B;
constexpr unsigned short kOpGiveBuff     = 0x0020;
constexpr unsigned short kOpCancelBuff   = 0x0021;
constexpr unsigned short kOpCombatStats  = 0x373E;

// BuffStat.SHADOWPARTNER = 0x400000000000000 with isFirst() == false,
// so it lives in the SECOND 8-byte mask of GIVE_BUFF / CANCEL_BUFF.
constexpr unsigned long long kShadowPartnerMask = 0x0400000000000000ULL;

// BuffStat.COMBO = 0x20000000000000 with isFirst() == false -> second mask,
// bit 53. It is always sent as a singleton stat list (CloseRangeDamageHandler:132,
// Character.handleOrbconsume), so the packet length is fixed. See R14.
constexpr unsigned long long kComboMask = 0x0020000000000000ULL;
constexpr unsigned long kComboGiveBuffSize = 41;  // 6 + 16 mask + 10 stat + 4 + 1 + 4
constexpr int kComboMax = 11;                     // server cap is x+1 (R12 9.7)

inline void Bump(TraceSlot slot) {
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&CombatHooks::g_trace[slot]));
}

void SetShadow(unsigned char value, const char* reason) {
    if (CombatHooks::g_shadowActive == value) {
        return;
    }
    CombatHooks::g_shadowActive = value;
    char line[96];
    wsprintfA(line, "[combat] shadow=%u reason=%s\r\n", static_cast<unsigned>(value), reason);
    OutputDebugStringA(line);
}

// PacketLen is 16-bit (see the layout proof in CombatHooks.h). Anything outside
// a sane range means the struct layout drifted, so report "unknown" (0) and let
// the caller fall back to content self-checks instead of rejecting a good packet.
unsigned long PacketLength(const CInPacket* packet) {
    const unsigned short len = packet->PacketLen;
    if (len >= 6 && len <= 4096) {
        return len;
    }
    return 0;
}

// One-shot layout probe. R15 T1 printed eight-digit lengths because the old
// code widened the u16 length field; we keep a short, rate-limited dump so a
// future drift is visible in DebugView without a debugger attached.
constexpr int kDiagBudget = 8;
int g_diagUsed = 0;
// Separate budget for "a packet that carries the COMBO bit". Rate-limited so a
// long grind cannot flood the debugger, but independent of kDiagBudget so the
// login-time buff burst cannot starve it.
constexpr int kComboProbeBudget = 4;
int g_comboProbe = 0;

void MaybeDiag(unsigned short opcode, const CInPacket* packet, const unsigned char* data, unsigned long len) {
    if (opcode != kOpCombatStats && opcode != kOpGiveBuff && opcode != kOpCancelBuff) {
        return;
    }
    // P2-4: this dump reads data[0..7]. Check before spending a diag budget
    // slot so a short packet cannot starve the real layout probes.
    if (len < 8) {
        return;
    }
    if (g_diagUsed >= kDiagBudget) {
        return;
    }
    ++g_diagUsed;
    char line[192];
    wsprintfA(line,
              "[combat] DIAG op=%04X pkt=%08X data=%08X len=%lu w0C=%04X off=%u head=%02X%02X%02X%02X%02X%02X%02X%02X\r\n",
              static_cast<unsigned>(opcode),
              reinterpret_cast<unsigned>(packet),
              reinterpret_cast<unsigned>(data),
              len,
              static_cast<unsigned>(packet->PacketLen),
              static_cast<unsigned>(packet->Offset),
              data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
    OutputDebugStringA(line);
}

bool HasShadowPartnerMask(const unsigned char* data) {
    // 4 header + 2 opcode + 8 firstmask + 8 secondmask = 22 bytes minimum, and
    // every GIVE_BUFF / CANCEL_BUFF carries both masks, so this read stays in
    // bounds even when the length field is not trustworthy.
    unsigned long long secondMask = 0;
    memcpy(&secondMask, data + 14, sizeof(secondMask));
    return (secondMask & kShadowPartnerMask) != 0;
}

void SetCombo(int buff, const char* reason) {
    if (buff < 0) {
        buff = 0;
    }
    if (buff > kComboMax) {
        buff = kComboMax;
    }
    int orbs = (buff > 0) ? (buff - 1) : 0;
    if (CombatHooks::g_comboBuff == buff) {
        return;
    }
    CombatHooks::g_comboBuff = buff;
    CombatHooks::g_comboOrbs = orbs;
    char line[96];
    wsprintfA(line, "[combat] combo buff=%d orbs=%d reason=%s\r\n", buff, orbs, reason);
    OutputDebugStringA(line);
}

// GIVE_BUFF payload for a COMBO-only stat list (R14 section 2.2):
//   6..13  firstmask  (must be 0)
//  14..21  secondmask (must be exactly the COMBO bit)
//  22..23  short  comboBuff
//  24..27  int    buffid
//  28..31  int    duration
//  32..35  int    0
//  36      byte   0
//  37..40  int    comboBuff again
// Every guard must pass. The second mask must match the COMBO bit EXACTLY and
// the trailing int must repeat the short, so the packet identifies itself from
// its own bytes; the length check is only a fast pre-filter. When the length is
// unknown (len == 0) the self-check alone decides. A layout drift must degrade
// to "value not updated", never to "value jumps around".
void ParseComboOrbs(const unsigned char* data, unsigned long len) {
    unsigned long long firstMask = 0;
    unsigned long long secondMask = 0;
    memcpy(&firstMask, data + 6, sizeof(firstMask));
    memcpy(&secondMask, data + 14, sizeof(secondMask));
    // Probe: any 0x20 packet whose second mask carries the COMBO bit is worth
    // one line, so a wrong 41-byte assumption shows up as "COMBOPKT len=NN"
    // instead of silence. Guarded by its own budget (see g_comboProbe).
    if ((secondMask & kComboMask) != 0 && g_comboProbe < kComboProbeBudget) {
        ++g_comboProbe;
        unsigned short probeRaw = 0;
        memcpy(&probeRaw, data + 22, sizeof(probeRaw));
        char probe[128];
        wsprintfA(probe, "[combat] COMBOPKT len=%lu exact=%u raw=%d\r\n",
                  len,
                  static_cast<unsigned>((firstMask == 0 && secondMask == kComboMask) ? 1 : 0),
                  static_cast<int>(probeRaw));
        OutputDebugStringA(probe);
    }
    if (len != 0 && len != kComboGiveBuffSize) {
        return;
    }
    if (firstMask != 0 || secondMask != kComboMask) {
        return;
    }
    unsigned short raw = 0;
    int duplicate = 0;
    memcpy(&raw, data + 22, sizeof(raw));
    memcpy(&duplicate, data + 37, sizeof(duplicate));
    if (raw > kComboMax || duplicate != static_cast<int>(raw)) {
        Bump(kTraceReject);
        return;
    }
    SetCombo(static_cast<int>(raw), "GIVE_BUFF 0x20");
    Bump(kTraceCombo);
}

bool HasComboMask(const unsigned char* data) {
    unsigned long long secondMask = 0;
    memcpy(&secondMask, data + 14, sizeof(secondMask));
    return (secondMask & kComboMask) != 0;
}

void HandleCombatStats(const unsigned char* data, unsigned long len) {
    // A: 4 header + 2 opcode + 1 version + 6*4 = 31 bytes  <- live server layout
    // B: 4 header + 2 opcode + 4 version + 6*4 = 34 bytes  <- forward compatible
    // wt-skill PacketCreator.getCombatParams writes writeByte(1) + 6*writeInt,
    // so A is what the server actually emits today.
    // 0x373E is a private opcode with exactly one producer, therefore a single
    // 1-byte version is unambiguous even when the length field is unavailable.
    const unsigned char* payload = nullptr;
    int version = 0;
    if (len == 34) {
        memcpy(&version, data + 6, sizeof(version));
        payload = data + 10;
    } else if (len == 0 || len == 31) {
        version = data[6];
        payload = data + 7;
    } else {
        Bump(kTraceReject);
        char bad[96];
        wsprintfA(bad, "[combat] C2 REJECT len=%lu (want 31 or 34)\r\n", len);
        OutputDebugStringA(bad);
        return;
    }
    if (version != 1) {
        Bump(kTraceReject);
        char bad[96];
        wsprintfA(bad, "[combat] C2 REJECT version=%d len=%lu\r\n", version, len);
        OutputDebugStringA(bad);
        return;
    }
    int values[6];
    memcpy(values, payload, sizeof(values));
    for (int i = 0; i < 6; ++i) {
        CombatHooks::g_combat[i] = values[i];
    }
    Bump(kTraceCombatStats);
    char line[160];
    wsprintfA(line, "[combat] C2 v=%d len=%lu crit=%d critDmg=%d final=%d boss=%d idef=%d growth=%d\r\n",
              version, len,
              static_cast<int>(CombatHooks::g_combat[0]), static_cast<int>(CombatHooks::g_combat[1]),
              static_cast<int>(CombatHooks::g_combat[2]), static_cast<int>(CombatHooks::g_combat[3]),
              static_cast<int>(CombatHooks::g_combat[4]), static_cast<int>(CombatHooks::g_combat[5]));
    OutputDebugStringA(line);
}

void ResetSession(const char* reason) {
    CombatHooks::g_shadowActive = 0;
    for (int i = 0; i < 6; ++i) {
        CombatHooks::g_combat[i] = 0;
    }
    CombatHooks::g_comboBuff = 0;
    CombatHooks::g_comboOrbs = 0;
    Bump(kTraceSessionReset);
    char line[96];
    wsprintfA(line, "[combat] session reset: %s\r\n", reason);
    OutputDebugStringA(line);
}

} // namespace

void CombatHooks::DispatchRecvPacket(CInPacket* packet) {
    if (packet == nullptr || packet->Data == nullptr) {
        return;
    }
    __try {
        const unsigned char* data = reinterpret_cast<const unsigned char*>(packet->Data);
        // P2-4 (R20 5.1): guard BEFORE reading data[4..5]. PacketLength()
        // already returns 0 for <6 or >4096; formerly the out-of-bounds read
        // was silently swallowed by the SEH frame below.
        const unsigned long len = PacketLength(packet);
        if (len < 6) {
            return;
        }
        unsigned short opcode = 0;
        memcpy(&opcode, data + 4, sizeof(opcode));
        MaybeDiag(opcode, packet, data, len);
        switch (opcode) {
        case kOpCombatStats:
            // HandleCombatStats already REJECTs anything but len 31 / 34.
            HandleCombatStats(data, len);
            break;
        case kOpGiveBuff:
            // P2-4: both mask readers need data[6..21] and ParseComboOrbs
            // reads data[6..23], so 24 bytes is the floor for this case.
            if (len < 24) {
                break;
            }
            if (HasShadowPartnerMask(data)) {
                SetShadow(1, "GIVE_BUFF 0x20");
                Bump(kTraceGiveBuff);
            }
            ParseComboOrbs(data, len);
            break;
        case kOpCancelBuff:
            // P2-4: HasShadowPartnerMask / HasComboMask both read data[6..21].
            if (len < 22) {
                break;
            }
            if (HasShadowPartnerMask(data)) {
                SetShadow(0, "CANCEL_BUFF 0x21");
                Bump(kTraceCancelBuff);
            }
            if (HasComboMask(data)) {
                SetCombo(0, "CANCEL_BUFF 0x21");
            }
            break;
        case kOpLoginStatus:
        case kOpServerList:
        case kOpCharList:
            ResetSession("login-server boundary");
            break;
        default:
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
}
