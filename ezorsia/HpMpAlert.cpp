#include "stdafx.h"
#include "HpMpAlert.h"
namespace {
constexpr DWORD kSaveGlobalAddr = 0x0049C8E7;
constexpr DWORD kUIStatusBarPtr = 0x00BEBF9C;
constexpr DWORD kHpAlertOffset = 0x80;
constexpr DWORD kMpAlertOffset = 0x84;
constexpr DWORD kClientSocketPtr = 0x00BE7914;
constexpr DWORD kProcessPacketAddr = 0x004965F1;
constexpr WORD kOpcodeSetHpMpAlert = 0x1000;
struct COutPacket {
    int Loopback;
    union {
        unsigned char* Data;
        void* Unk;
        unsigned short* Header;
    };
    unsigned long Size;
    unsigned int Offset;
    int EncryptedByShanda;
};
struct CInPacket {
    bool Loopback;
    int State;
    void* Data;
    unsigned long Size;
    unsigned short RawSeq;
    unsigned short DataLen;
    unsigned short Unknown;
    unsigned int Offset;
    void* Unk;
};
using SendPacket_t = void(__fastcall*)(void* pThis, void* edx, COutPacket* packet);
static SendPacket_t g_SendPacket = reinterpret_cast<SendPacket_t>(0x0049637B);
static bool TryReadDword(DWORD address, DWORD& out) {
    __try {
        out = *reinterpret_cast<DWORD*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = 0;
        return false;
    }
}
static unsigned char ClampAlert(int value) {
    if (value < 0) return 0;
    if (value > 20) return 20;
    return static_cast<unsigned char>(value);
}
static void SendHpMpAlertFromStatusBar() {
    DWORD statusBar = 0;
    if (!TryReadDword(kUIStatusBarPtr, statusBar) || statusBar == 0) {
        return;
    }
    DWORD hpRaw = 0;
    DWORD mpRaw = 0;
    if (!TryReadDword(statusBar + kHpAlertOffset, hpRaw)) {
        return;
    }
    if (!TryReadDword(statusBar + kMpAlertOffset, mpRaw)) {
        return;
    }
    const unsigned char hpAlert = ClampAlert(static_cast<int>(hpRaw));
    const unsigned char mpAlert = ClampAlert(static_cast<int>(mpRaw));
    DWORD socketPtr = 0;
    if (!TryReadDword(kClientSocketPtr, socketPtr) || socketPtr == 0) {
        return;
    }
    unsigned char payload[4] = {
        static_cast<unsigned char>(kOpcodeSetHpMpAlert & 0xFF),
        static_cast<unsigned char>((kOpcodeSetHpMpAlert >> 8) & 0xFF),
        hpAlert,
        mpAlert
    };
    COutPacket packet{};
    packet.Loopback = 0;
    packet.Data = payload;
    packet.Size = sizeof(payload);
    packet.Offset = 0;
    packet.EncryptedByShanda = 0;
    g_SendPacket(reinterpret_cast<void*>(socketPtr), nullptr, &packet);
}
static void ApplyHpMpAlertToStatusBar(unsigned char hpAlert, unsigned char mpAlert) {
    DWORD statusBar = 0;
    if (!TryReadDword(kUIStatusBarPtr, statusBar) || statusBar == 0) {
        return;
    }
    Memory::WriteInt(statusBar + kHpAlertOffset, hpAlert);
    Memory::WriteInt(statusBar + kMpAlertOffset, mpAlert);
}
static void HandleHpMpAlertPacket(CInPacket* packet) {
    if (packet == nullptr) {
        return;
    }
    __try {
        if (packet->Data == nullptr || packet->Size < 8) {
            return;
        }
        const unsigned char* data = reinterpret_cast<const unsigned char*>(packet->Data);
        const unsigned short opcode = *reinterpret_cast<const unsigned short*>(data + 4);
        if (opcode != kOpcodeSetHpMpAlert) {
            return;
        }
        // First two bytes are HP/MP alert thresholds; append more settings after if needed.
        const unsigned char hpAlert = ClampAlert(static_cast<int>(data[6]));
        const unsigned char mpAlert = ClampAlert(static_cast<int>(data[7]));
        ApplyHpMpAlertToStatusBar(hpAlert, mpAlert);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
}
using SaveGlobal_t = void(__fastcall*)(void* pThis, void* edx);
static SaveGlobal_t s_SaveGlobal = reinterpret_cast<SaveGlobal_t>(kSaveGlobalAddr);
static void __fastcall SaveGlobal_Hook(void* pThis, void* edx) {
    s_SaveGlobal(pThis, edx);
    SendHpMpAlertFromStatusBar();
}
using ProcessPacket_t = void(__fastcall*)(void* pThis, void* edx, CInPacket* packet);
static ProcessPacket_t s_ProcessPacket = reinterpret_cast<ProcessPacket_t>(kProcessPacketAddr);
static void __fastcall ProcessPacket_Hook(void* pThis, void* edx, CInPacket* packet) {
    HandleHpMpAlertPacket(packet);
    s_ProcessPacket(pThis, edx, packet);
}
} // namespace
void HookSaveGlobal(bool enable) {
    Memory::SetHook(enable, reinterpret_cast<void**>(&s_SaveGlobal), SaveGlobal_Hook);
}
void HookHpMpAlertRecv(bool enable) {
    Memory::SetHook(enable, reinterpret_cast<void**>(&s_ProcessPacket), ProcessPacket_Hook);
}
