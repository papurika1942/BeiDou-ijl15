#pragma once

class Memory
{
public:
	static bool SetHook(bool attach, void** ptrTarget, void* ptrDetour);
	static void FillBytes(DWORD dwOriginAddress, unsigned char ucValue, int nCount);
	static void ReplaceString(DWORD dwOriginAddress, const char* sContent, const char* oContent);
	static void WriteString(DWORD dwOriginAddress, const char* sContent, const int oSize);
	static void WriteString(DWORD dwOriginAddress, const char* sContent);
	static void WriteByte(DWORD dwOriginAddress, unsigned char ucValue);
	static void WriteShort(DWORD dwOriginAddress, unsigned short usValue);
	static void WriteInt(DWORD dwOriginAddress, unsigned int dwValue);
	static void WriteDouble(DWORD dwOriginAddress, double dwValue);
	static void CodeCave(void* ptrCodeCave, DWORD dwOriginAddress, int nNOPCount);
	static void WriteByteArray(DWORD dwOriginAddress, unsigned char* ucValue, const int ucValueSize);
	static bool UseVirtuProtect;
    static void PatchNop(DWORD dwOriginAddress, int nCount) {
        if (nCount <= 0) return;

        // 根据需要设置内存保护
        DWORD oldProtect;
        if (UseVirtuProtect) {
            VirtualProtect((LPVOID)dwOriginAddress, nCount, PAGE_EXECUTE_READWRITE, &oldProtect);
        }

        // 填充NOP指令(0x90)
        memset((void*)dwOriginAddress, 0x90, nCount);

        // 恢复内存保护
        if (UseVirtuProtect) {
            DWORD temp;
            VirtualProtect((LPVOID)dwOriginAddress, nCount, oldProtect, &temp);
        }
    }
};

