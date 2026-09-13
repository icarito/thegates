#pragma once

// libzmq includes this for sockaddr_un outside MSVC; Windows declares it in
// afunix.h, which MinGW-w64 only ships from version 9.
#if __has_include(<afunix.h>)
#include <afunix.h>
#else
#include <winsock2.h>

#define UNIX_PATH_MAX 108

typedef struct sockaddr_un {
	ADDRESS_FAMILY sun_family;
	char sun_path[UNIX_PATH_MAX];
} SOCKADDR_UN, *PSOCKADDR_UN;
#endif
