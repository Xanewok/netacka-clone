#define _USE_MATH_DEFINES
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <stdexcept>
#include <limits>
#include <ctime>
#include <set>
#include <map>
#include <memory>
#include <chrono>
#include <algorithm>
#include <array>
#include <mutex>

#ifdef _WIN32
#define NOMINMAX
#pragma comment(lib, "Ws2_32.lib")
#include <WinSock2.h> // endianness helpers
#include <ws2ipdef.h>
#include <ws2tcpip.h>
using ssize_t = SSIZE_T;
#else
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#define _BSD_SOURCE
#include <endian.h> // TODO: Verify
#endif

#include "protocol.h"
#include "util.h"
#include "rand.h"
#include "map.h"

#define BUFFER_SIZE 1000

namespace {
	void syserr(const char* msg)
	{
		fprintf(stderr, msg);
		std::exit(1);
	}
}

int main(int argc, char *argv[]) {
#ifdef _WIN32
	WORD wVersionRequested;
	WSADATA wsaData;

	/* Use the MAKEWORD(lowbyte, highbyte) macro declared in Windef.h */
	wVersionRequested = MAKEWORD(2, 2);

	int err = WSAStartup(wVersionRequested, &wsaData);
	if (err != 0) {
		/* Tell the user that we could not find a usable */
		/* Winsock DLL.                                  */
		printf("WSAStartup failed with error: %d\n", err);
		return 1;
	}
#endif



	int sock;
	struct addrinfo addr_hints;
	struct addrinfo *addr_result;

	int i, flags, sflags;
	char buffer[BUFFER_SIZE];
	size_t len;
	ssize_t snd_len, rcv_len;
	struct sockaddr_in my_address;
	struct sockaddr_in srvr_address;
	socklen_t rcva_len;

	if (argc < 3) {
		fprintf(stderr, "Usage: %s host port message ...\n", argv[0]);
		std::exit(1);
	}

	// 'converting' host/port in string to struct addrinfo
	(void)memset(&addr_hints, 0, sizeof(struct addrinfo));
	addr_hints.ai_family = AF_INET; // IPv4
	addr_hints.ai_socktype = SOCK_DGRAM;
	addr_hints.ai_protocol = IPPROTO_UDP;
	addr_hints.ai_flags = 0;
	addr_hints.ai_addrlen = 0;
	addr_hints.ai_addr = NULL;
	addr_hints.ai_canonname = NULL;
	addr_hints.ai_next = NULL;
	if (getaddrinfo(argv[1], NULL, &addr_hints, &addr_result) != 0) {
		syserr("getaddrinfo");
	}

	my_address.sin_family = AF_INET; // IPv4
	my_address.sin_addr.s_addr =
		((struct sockaddr_in*) (addr_result->ai_addr))->sin_addr.s_addr; // address IP
	my_address.sin_port = htons((uint16_t)atoi(argv[2])); // port from the command line

	freeaddrinfo(addr_result);

	sock = socket(PF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		syserr("socket");

	for (i = 3; i < argc; i++) {
		len = strnlen(argv[i], BUFFER_SIZE);
		if (len == BUFFER_SIZE) {
			(void)fprintf(stderr, "ignoring long parameter %d\n", i);
			continue;
		}
		(void)printf("sending to socket: %s\n", argv[i]);
		sflags = 0;
		rcva_len = (socklen_t) sizeof(my_address);
		snd_len = sendto(sock, argv[i], len, sflags,
			(struct sockaddr *) &my_address, rcva_len);

		if (snd_len != (ssize_t)len) {
			syserr("partial / failed write");
		}
	}

	return 0;
}
