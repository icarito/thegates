#pragma once

// libzmq includes this for AF_UNIX outside MSVC; MinGW provides it through Winsock.
#include <winsock2.h>
#include <ws2tcpip.h>
