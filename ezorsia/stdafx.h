#pragma once

#include "targetver.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
#endif

// Include before <windows.h> to avoid winsock.h conflicts.
// Needed for hostname->IPv4 resolution (getaddrinfo/InetPton/InetNtop).
#include <winsock2.h>
#include <ws2tcpip.h>

// Windows Header Files
#include <windows.h>

// reference additional headers your program requires here

#include <iostream>
#include "Client.h"
#include "Memory.h"

