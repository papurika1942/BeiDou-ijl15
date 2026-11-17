// dllmain.cpp : Defines the entry point for the DLL application.
#include "stdafx.h"
#include "NMCO.h"
#include "ijl15.h"
#include "INIReader.h"
#include "ReplacementFuncs.h"
#include <comutil.h>
#include "BossHP.h"
#include "HpMpAlert.h"
#include "SelectCharMacFix.h"
#include "CombatDamage.h"
#include "fusion_tier.h"
#pragma comment(lib, "ws2_32.lib")

// config.ini can use IP or hostname (ServerIP_Address=...).
// The patch expects an IPv4 dotted string; resolve hostnames to IPv4.
// On failure, fall back to the original value.
static std::string ResolveToIpv4String(const std::string& hostOrIp)
{
	if (hostOrIp.empty()) return hostOrIp;

	IN_ADDR parsedAddr{};
	if (InetPtonA(AF_INET, hostOrIp.c_str(), &parsedAddr) == 1) {
		return hostOrIp;
	}

	WSADATA wsaData{};
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		return hostOrIp;
	}

	addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	addrinfo* result = nullptr;
	const int gaiRc = getaddrinfo(hostOrIp.c_str(), nullptr, &hints, &result);
	if (gaiRc != 0 || result == nullptr) {
		WSACleanup();
		return hostOrIp;
	}

	char ipBuf[INET_ADDRSTRLEN]{};
	const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(result->ai_addr);
	const PCSTR ipStr = InetNtopA(AF_INET, const_cast<IN_ADDR*>(&ipv4->sin_addr), ipBuf, sizeof(ipBuf));

	freeaddrinfo(result);
	WSACleanup();

	if (ipStr == nullptr) {
		return hostOrIp;
	}

	return std::string(ipStr);
}

// [optional] damageInject: 0 = off (default), 1 = multiply only,
// 2 = multiply + DebugView probe. Kept in the ini so the cave can be
// switched off without rebuilding if it ever misbehaves again.
// damageProbe caps the number of PRINTED rows (amplified rows only; the first
// hit of the session is always printed; no-op rows never spend the budget).
// A NEGATIVE value prints EVERY row including no-op rows, which exposes the
// raw `in` distribution for the native-crit check (R18 5). Uses strtol, so the
// minus sign parses fine.
static int g_damageInject = 0;
static int g_damageProbe = 40;
// [optional] damageSites: bitmask of landing zones.
//   1 = 0x00790586 PDamage, 2 = 0x00791F8B C1,
//   4 = 0x0079343F C3,       8 = 0x00793668 C4
// Default 1 keeps the 11-13 tested configuration. Set 15 to cover every
// top-level exit (the two nested compares must never be patched).
static int g_damageSites = 1;
// [optional] damageCrit (R23): 0=off, 1=legacy per-row crit roll (SECOND
// roll, 4 tiers, A/B only), 2=native-entry scheme B: add critRate/critDmg
// to PDamage's native accumulators before its single roll (see R23).
// Default 0 keeps the finalDamage baseline bit-identical to the 11-13 run.
// Read once at DLL_PROCESS_ATTACH: change requires a full client restart.
static int g_damageCrit = 0;
// [optional] shadowMelee (R33): 1 = let PDamage add shadow-partner rows for
// non-ranged attacks (short-knife melee). The native ranged path already folds
// its own rows, so the cave only acts when [ebp+0x34] == 0 and the SHADOWPARTNER
// buff is up. Default 0 = untouched baseline.
static int g_shadowMelee = 0;
// [optional] shadowBcast (R36 U1): 1 = when shadowMelee is on, also double the
// upper-layer row count (A 0x951C0C, B 0x954C63) so the damage-number loop and
// the apply/packet call follow the extra rows. 0 = compute only, no broadcast.
// The packet then carries 2n rows: the server side must accept them.
static int g_shadowBcast = 1;
// [optional] mesoFinal (R33): 1 = apply finalDamagePercent to Meso Explosion
// (4211006), which computes damage in its own routine (0x791FBC) and therefore
// never passes the four PDamage/C1 exits. Default 0.
static int g_mesoFinal = 0;
// [optional] mesoAttack (R39): 1 = 4111004 (money attack) STACKS the native
// crit aggregate on top of its own WZ pair inside its private branch
// (0x78F897..0x78F9FA), so the description's "10% chance / +50% damage" stays
// real while passive + sharp eyes + stun mastery + war god + C2 are added on
// top. 0 = untouched 10% / x1.0. Default 0.
static int g_mesoAttack = 0;
// [optional] mesoAttackCd (R39): WZ half of the 4111004 multiplier while the
// `criticalDamage` node is still missing (lv30 has none, hence the original
// x1.0 "crit" that only drew a yellow number). A real node always wins.
//   > 0 fixed value | = 0 no supplement | < 0 follow the skill's WZ `x`
// `x` is also the damage base (u in [0.5x,1.5x], then ftol(u)*10), so raising
// x scales the base and this multiplier together. Default -1.
static int g_mesoAttackCd = -1;
// [optional] critFix (R40 A5): 1 = scale the LIVE row total by cd/100 instead
// of the native `dmg += [ebp-8] * (cd-100)/100`. [ebp-8] is a snapshot taken
// before any earlier multiplier, so when the two diverge a crit row prints
// BELOW an uncritted one. Default 1.
static int g_critFix = 1;
// [optional] assassinNative (R38 AN1): 1 = 4221001 also takes the weapon-typed
// native crit base (0x765095) on top of its WZ 90/250, so it eats passive +
// sharp eyes + stun mastery 5110000 + war-god 21110000 + C2 like any other
// skill. Daggers return 0/0 from that call, so a 420 is unchanged. Default 1.
static int g_assassinNative = 1;
// R42 (ZiMo v2 native patches): all four are gameplay-changing exe byte
// patches lifted from ZiMo ijl15.dll 0x10011820. They are OFF by default so a
// merged dll + stock config stays bit-identical to the R41 baseline. Every
// site is verified against the shipped exe bytes before writing; a different
// client build is skipped instead of crashing. Set the matching [optional]
// key to true to enable, then restart the client (read once at attach).
static bool g_banFist = false;
static bool g_strengthenSewer = false;
static bool g_unStopAttack = false;
static bool g_unlockSP = false;

// Verify n bytes at va match expected. Direct read is safe (exe .text is
// readable); writes go through Memory::WriteXxx which handles VirtualProtect.
static bool R42VerifyBytes(DWORD va, const unsigned char* expected, int n)
{
	const unsigned char* p = reinterpret_cast<const unsigned char*>(va);
	for (int i = 0; i < n; ++i) {
		if (p[i] != expected[i]) return false;
	}
	return true;
}

static void ApplyNativePatches(bool banFist, bool strengthenSewer,
							   bool unStopAttack, bool unlockSP)
{
	// banFist: ranged melee must not draw the fist swing.
	//   0x009516C3  je(0x84) -> jns(0x89)
	if (banFist) {
		static const unsigned char exp[] = { 0x84 };
		if (R42VerifyBytes(0x009516C3, exp, 1)) {
			Memory::WriteByte(0x009516C3, 0x89);
		}
	}
	// unStopAttack: never break the attack loop.
	//   0x009536E0  jle(0x7E) -> jmp(0xEB)
	if (unStopAttack) {
		static const unsigned char exp[] = { 0x7E };
		if (R42VerifyBytes(0x009536E0, exp, 1)) {
			Memory::WriteByte(0x009536E0, 0xEB);
		}
	}
	// unlockSP: jump over the low-tier SP sufficiency check.
	//   ZiMo helper 0x1001bdf0(0x8AD227, 0x8AD01A, nop=0):
	//   0x008AD01A  je 0x8AD227 (0F 84 07 02 00 00) -> E9 rel32(0x208)
	if (unlockSP) {
		static const unsigned char exp[] = { 0x0F, 0x84, 0x07, 0x02, 0x00, 0x00 };
		if (R42VerifyBytes(0x008AD01A, exp, 6)) {
			Memory::WriteByte(0x008AD01A, 0xE9);
			Memory::WriteInt(0x008AD01B, 0x208); // 0x8AD227 - 0x8AD01A - 5
		}
	}
	// StrengthenSewer: five byte sites, applied atomically (all match or none).
	//   ZiMo 0x10011923..0x10011966. Values are the exact writes from ZiMo.
	if (strengthenSewer) {
		static const DWORD sites[5] = { 0x00950D96, 0x00968278, 0x00953055, 0x009530FD, 0x00953689 };
		static const unsigned char exps[5][4] = {
			{ 0x0E, 0x00, 0x00, 0x00 }, // je rel8 at 0x950D95+1
			{ 0xE9, 0xBE, 0x0E, 0x00 }, // jmp rel32 dword at 0x968278
			{ 0xE0, 0x01, 0x00, 0x00 }, // push imm16 at 0x953054+1
			{ 0xA8, 0x07, 0x00, 0x00 }, // add edi,imm32 at 0x9530FB+2
			{ 0xE8, 0x03, 0x00, 0x00 }  // call rel32 low word at 0x953689
		};
		static const int lens[5] = { 1, 4, 2, 2, 2 };
		bool allMatch = true;
		for (int i = 0; i < 5; ++i) {
			if (!R42VerifyBytes(sites[i], exps[i], lens[i])) { allMatch = false; break; }
		}
		if (allMatch) {
			Memory::WriteByte(0x00950D96, 0x2A);
			Memory::WriteInt(0x00968278, 0x19E9);
			Memory::WriteShort(0x00953055, 0x00FE);
			Memory::WriteShort(0x009530FD, 0x04B3);
			Memory::WriteShort(0x00953689, 0x0100);
		}
	}
}

void CreateConsole() {
	AllocConsole();
	FILE* stream;
	freopen_s(&stream, "CONOUT$", "w", stdout); //CONOUT$
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
	switch (ul_reason_for_call) {
	case DLL_PROCESS_ATTACH:
	{
		//CreateConsole();	//console for devs, use this to log stuff if you want
		INIReader reader("config.ini");
		if (reader.ParseError() == 0) {
			Client::m_nGameWidth = reader.GetInteger("general", "width", 1280);
			Client::m_nGameHeight = reader.GetInteger("general", "height", 720);
			Client::MsgAmount = reader.GetInteger("general", "MsgAmount", 26);
			Client::CustomLoginFrame = reader.GetBoolean("general", "CustomLoginFrame", true);
			Client::WindowedMode = reader.GetBoolean("general", "WindowedMode", true);
			Client::RemoveLogos = reader.GetBoolean("general", "RemoveLogos", true);
			Memory::UseVirtuProtect = reader.GetBoolean("general", "UseVirtuProtect", true);
			Client::setDamageCap = reader.GetReal("optional", "setDamageCap", 199999);
			Client::setMAtkCap = reader.GetReal("optional", "setMAtkCap", 1999);
			Client::setAccCap = reader.GetReal("optional", "setAccCap", 999);
			Client::setAvdCap = reader.GetReal("optional", "setAvdCap", 999);
			Client::setAtkOutCap = reader.GetReal("optional", "setAtkOutCap", 199999);
			Client::useTubi = reader.GetBoolean("optional", "useTubi", false);
			Client::bigLoginFrame = reader.GetBoolean("general", "bigLoginFrame", false);
			Client::SwitchChinese = reader.GetBoolean("general", "SwitchChinese", false);
			Client::speedMovementCap = reader.GetInteger("optional", "speedMovementCap", 140);
			Client::jumpCap = reader.GetInteger("optional", "jumpCap", 123);
			Client::debug = reader.GetBoolean("debug", "debug", false);
			Client::noPassword = reader.GetBoolean("debug", "noPassword", false);
			Client::imeType = reader.GetInteger("general", "imeType", 1);
			ownLoginFrame = reader.GetBoolean("optional", "ownLoginFrame", false);
			ownCashShopFrame = reader.GetBoolean("optional", "ownCashShopFrame", false);
			EzorsiaV2WzIncluded = reader.GetBoolean("general", "EzorsiaV2WzIncluded", true);
			Client::ServerIP_AddressFromINI = ResolveToIpv4String(reader.Get("general", "ServerIP_Address", "127.0.0.1"));
			Client::serverIP_Port = reader.GetInteger("general", "serverIP_Port", 8484);
			Client::climbSpeedAuto = reader.GetBoolean("optional", "climbSpeedAuto", false);
			Client::climbSpeed = reader.GetFloat("optional", "climbSpeed", 1.0);
			Client::talkRepeat = reader.GetBoolean("optional", "talkRepeat", false);
			Client::talkTime = reader.GetInteger("optional", "talkTime", 2000);
			g_damageInject = reader.GetInteger("optional", "damageInject", 0);
			g_damageProbe = reader.GetInteger("optional", "damageProbe", 40);
			g_damageSites = reader.GetInteger("optional", "damageSites", 1);
			g_damageCrit = reader.GetInteger("optional", "damageCrit", 0);
			g_shadowMelee = reader.GetInteger("optional", "shadowMelee", 0);
			g_shadowBcast = reader.GetInteger("optional", "shadowBcast", 1);
			g_mesoFinal = reader.GetInteger("optional", "mesoFinal", 0);
			g_mesoAttack = reader.GetInteger("optional", "mesoAttack", 0);
			g_mesoAttackCd = reader.GetInteger("optional", "mesoAttackCd", -1);
			g_critFix = reader.GetInteger("optional", "critFix", 1);
			g_assassinNative = reader.GetInteger("optional", "assassinNative", 1);
			// R42: gameplay byte patches, default false (紫陌 config sets them true).
			g_banFist = reader.GetBoolean("optional", "banFist", false);
			g_strengthenSewer = reader.GetBoolean("optional", "StrengthenSewer", false);
			g_unStopAttack = reader.GetBoolean("optional", "unStopAttack", false);
			g_unlockSP = reader.GetBoolean("optional", "unlockSP", false);
			// R41 baseline merge: the config keys that already exist in the
			// upstream source as hard-coded constants. Defaults equal the old
			// hard-coded values, so an old config.ini keeps working unchanged.
#if FUSION_TIER >= 41
			// R41 UI tier: read the 11 UI keys from config. Compiled out below
			// tier 41 so the statics keep their hard-coded R39 defaults.
			Client::StatusBarX = reader.GetInteger("statusbar", "StatusBarX", -1);
			Client::MoveBarFrame = reader.GetBoolean("statusbar", "MoveBarFrame", true);
			Client::MoveBarItems = reader.GetBoolean("statusbar", "MoveBarItems", true);
			Client::ChatBoxX = reader.GetInteger("statusbar", "ChatBoxX", 0);
			Client::BackgroundOffsetX = reader.GetInteger("statusbar", "BackgroundOffsetX", 0);
			Client::EnableLongSlot = reader.GetBoolean("quickslot", "EnableLongSlot", true);
			Client::QuickSlotX = reader.GetInteger("quickslot", "QuickSlotX", 0);
			Client::QuickSlotY = reader.GetInteger("quickslot", "QuickSlotY", 0);
			BossHP::Enabled = reader.GetBoolean("bosshp", "Enabled", true);
			BossHP::PercentOffsetY = reader.GetInteger("bosshp", "PercentOffsetY", 37);
#endif
		}

		Hook_CreateMutexA(true); //multiclient //ty darter, angel, and alias!
		HookCreateWindowExA(true); //default ezorsia
		HookGetModuleFileName(true); //default ezorsia
		HookPcCreateObject_IWzResMan(true);
		HookPcCreateObject_IWzNameSpace(true);
		HookPcCreateObject_IWzFileSystem(true);
		HookCWvsApp__Dir_BackSlashToSlash(true);
		HookCWvsApp__Dir_upDir(true);
		Hookbstr_ctor(true);
		HookIWzFileSystem__Init(true);
		HookIWzNameSpace__Mount(true);
		HookCWvsApp__InitializeResMan(false); //experimental //ty to all the contributors of the ragezone release: Client load .img instead of .wz v62~v92
		Hook_StringPool__GetString(true); //hook stringpool modification //ty !! popcorn //ty darter
		Hook_lpfn_NextLevel(true);
		HookSaveGlobal(true);
		HookHpMpAlertRecv(true);
		if (g_damageInject != 0) {
			if (!CombatDamage::Install(g_damageInject >= 2, g_damageProbe, g_damageSites, g_damageCrit, g_shadowMelee, g_mesoFinal, g_shadowBcast, g_mesoAttack, g_assassinNative, g_mesoAttackCd, g_critFix)) {
				std::cout << "[combat] damage cave install FAILED" << std::endl;
			}
		} else {
			std::cout << "[combat] damage cave disabled (optional:damageInject=0)" << std::endl;
		}
#if FUSION_TIER >= 42
		// R42: four gameplay byte patches. Physically compiled out below tier 42
		// so a staged build cannot write them even if a stale config sets them true.
		ApplyNativePatches(g_banFist, g_strengthenSewer, g_unStopAttack, g_unlockSP);
		std::cout << "[fusion] TIER=42 native patches armed" << std::endl;
#else
		std::cout << "[fusion] TIER=" << FUSION_TIER
#if FUSION_TIER < 41
			<< " native patches COMPILED OUT + UI tier COMPILED OUT (R39 hard-coded)"
#else
			<< " native patches COMPILED OUT, UI tier active"
#endif
			<< std::endl;
#endif
		HookSelectCharMacFix(true);
		//Hook_get_unknown(true);
		//Hook_get_resource_object(true); //helper function hooks  //ty teto for helping me get started
		//Hook_com_ptr_t_IWzProperty__ctor(true);
		//Hook_com_ptr_t_IWzProperty__dtor(true);

		Client::UpdateGameStartup();

		std::cout << "Applying resolution " << Client::m_nGameWidth << "x" << Client::m_nGameHeight << std::endl;
		Client::UpdateResolution();
		Client::FixMouseWheel();
		Client::Chinese();
#if FUSION_TIER >= 41
		if (Client::EnableLongSlot) {
			Client::LongQuickSlot();
		}
#else
		Client::LongQuickSlot(); // R39 baseline: unconditional
#endif
		Client::FixDateFormat();
		Client::FixItemType();
		Client::JumpCap();
		Client::FixChatPosHook();
		Client::NoPassword();
		Client::MoreHook();
#if FUSION_TIER >= 41
		if (BossHP::Enabled) {
			BossHP::Hook();
		}
#else
		BossHP::Hook(); // R39 baseline: unconditional
#endif
		Client::WorldMap();
		Client::RefreshRate(); 
		Client::DeleteChar();
		std::cout << "GetModuleFileName hook created" << std::endl;
		ijl15::CreateHook(); //NMCO::CreateHook();

		std::cout << "NMCO hook initialized" << std::endl;
		break;
	}
	default: break;
	case DLL_PROCESS_DETACH:
		ExitProcess(0);
	}
	return TRUE;
}





