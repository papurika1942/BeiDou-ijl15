#pragma once
class Client
{
public:
	static void UpdateGameStartup();
	static void EnableNewIGCipher();
	static void UpdateResolution();
	static void UpdateLogin();
	static void FixMouseWheel();
	static void Chinese();
	static void LongQuickSlot();
	static void FixDateFormat();
	static void FixItemType();
	static void JumpCap();
	static void FixChatPosHook();
	static void NoPassword();
	static void MoreHook();
	static void WorldMap();
	static void RefreshRate(); 
	static void DeleteChar();
	static const int m_nIGCipherHash = 0XC65053F2;
	static int m_nGameHeight;
	static int m_nGameWidth;
	static int MsgAmount;
	static bool CustomLoginFrame;
	static bool WindowedMode;
	static bool RemoveLogos;
	static int setDamageCap;
	static int setMAtkCap;
	static int setAccCap;
	static int setAvdCap;
	static double setAtkOutCap;
	static bool useTubi;
	static bool bigLoginFrame;
	static bool SwitchChinese;
	static bool debug;
	static bool noPassword;
	static bool climbSpeedAuto;
	static float climbSpeed;
	static int speedMovementCap;
	static unsigned char imeType;
	static DWORD jumpCap;
	static std::string ServerIP_AddressFromINI;
	static int serverIP_Port;
	// R41 baseline merge: [statusbar] / [quickslot] switches. All defaults
	// reproduce the previous hard-coded behaviour.
	static int StatusBarX;        // -1 = keep the client's own x
	static bool MoveBarFrame;     // false = leave the background panel alone
	static bool MoveBarItems;     // false = leave HP/MP/EXP + chat alone
	static int ChatBoxX;          // extra x for the chat input
	static int BackgroundOffsetX; // extra x for the background image only
	static bool EnableLongSlot;   // false = keep the native 8-slot bar
	static int QuickSlotX;        // extra x for the whole quick-slot bar
	static int QuickSlotY;        // extra y for the whole quick-slot bar
	static bool talkRepeat;
	static int talkTime;
};