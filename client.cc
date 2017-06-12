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
#include <endian.h>
#endif

#include "protocol.h"
#include "util.h"
#include "rand.h"
#include "map.h"

#define BUFFER_SIZE 1000

constexpr const char* usage_msg =
"USAGE:  ./siktacka-client player_name game_server_host[:port] [ui_server_host[:port]]\n"
"  player_name – 0-64 drukowalne znaki ASCII (bez spacji, \"\" oznacza obserwatora\n"
"  game_server_host – adres IPv4, IPv6 lub nazwa wêz³a\n"
"  game_server_port - port serwera gry (domyœlnie 12345)\n"
"  ui_server_host   - nazwa serwera interfejsu u¿ytkownika (domyœlnie localhost)\n"
"  ui_server_port   - port serwera interfejsu u¿ytkownika (domyœlnie 12346)\n";

static std::uint64_t session_id;
static std::string player_name;

static struct {
	std::string game_server_host;
	std::uint16_t game_server_port = 12345;
	std::string ui_server_host = "localhost";
	std::uint16_t ui_server_port = 12346;
} configuration;

namespace {
	template<typename T>
	T parse(const char* str, T min, T max)
	{
		T value = T();
		try
		{
			value = static_cast<T>(util::parse_bounded(str, min, max));
		}
		catch (std::exception& e)
		{
			util::fatal("Invalid argument %s (%s)", str, e.what());
		}
		return value;
	}

	std::tuple<std::string, bool, std::string> split_hostname(const std::string& address)
	{
		size_t first_of = address.find_first_of(':');
		size_t last_of = address.find_last_of(':');

		const bool multiple_semicolons = first_of != last_of;
		const bool can_be_ipv6 = !multiple_semicolons ? false :
			address[0] == '[' && address[last_of - 1] == ']';
		if (last_of == std::string::npos || (multiple_semicolons && !can_be_ipv6))
			return std::make_tuple(address, false, std::string());
		else
		{
			const size_t port_off = last_of + sizeof(':');
			std::string host = (multiple_semicolons && can_be_ipv6)
				? address.substr(1, last_of - 2)
				: address.substr(0, last_of);
			return std::make_tuple(std::move(host), true, address.substr(port_off));
		}
	}
}

int main(int argc, const char* argv[])
{
	session_id = static_cast<std::uint64_t>(time(nullptr));

	if (argc < 3)
	{
		printf(usage_msg);
		std::exit(1);
	}

	if (strlen(argv[1]) > 64)
		util::fatal("Given player name (%s) is longer than 64 chars\n", argv[1]);

	player_name = std::string(argv[1]);
	if (player_name.compare("\"\"") == 0)
		player_name = "";

	// Split to get optional port, server host will be validated later with getaddrinfo()
	auto game_server = split_hostname(argv[2]);
	configuration.game_server_host = std::get<0>(game_server);
	if (std::get<1>(game_server))
	{
		configuration.game_server_port = parse<std::uint16_t>(std::get<2>(game_server).data(), 0, 65535);
	}
	// Ditto optionally for ui server hostname
	if (argc >= 4)
	{
		auto ui_server = split_hostname(argv[3]);
		configuration.ui_server_host = std::get<0>(ui_server);
		if (std::get<1>(ui_server))
		{
			configuration.ui_server_port = parse<std::uint16_t>(std::get<2>(ui_server).data(), 0, 65535);
		}
	}


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

	// TODO: Validate ui/server host names

	struct addrinfo hints, *res, *res0;
	memset(&hints, 0x00, sizeof(hints));
	int error = getaddrinfo(argv[2], nullptr, nullptr, &res0);

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

	const char* server_addr = "localhost";
	const char* port = "12345";

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
	if (getaddrinfo(server_addr, NULL, &addr_hints, &addr_result) != 0) {
		util::fatal("getaddrinfo");
	}

	my_address.sin_family = AF_INET; // IPv4
	my_address.sin_addr.s_addr =
		((struct sockaddr_in*) (addr_result->ai_addr))->sin_addr.s_addr; // address IP
	my_address.sin_port = htons((uint16_t)atoi(port)); // port from the command line

	freeaddrinfo(addr_result);

	sock = socket(PF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		util::fatal("socket");

	memset(buffer, 0x00, BUFFER_SIZE);
	client_message msg;
	msg.session_id = 1234;
	msg.turn_direction = 1;
	msg.next_expected_event = 31337;
	memcpy(msg.player_name, "Gowniak", strlen("Gowniak"));

	auto stream = msg.as_stream();
	memcpy(buffer, stream.data(), stream.size());

	len = stream.size();

	(void)printf("sending to socket: %s\n", buffer);
	sflags = 0;
	rcva_len = (socklen_t) sizeof(my_address);
	snd_len = sendto(sock, buffer, len, sflags,
		(struct sockaddr *) &my_address, rcva_len);

	if (snd_len != (ssize_t)len) {
		util::fatal("partial / failed write");
	}

	while (true)
	{
		(void)memset(buffer, 0, sizeof(buffer));
		flags = 0;
		len = (size_t) sizeof(buffer) - 1;
		rcva_len = (socklen_t) sizeof(srvr_address);
		rcv_len = recvfrom(sock, buffer, len, flags,
			(struct sockaddr *) &srvr_address, &rcva_len);

		if (rcv_len < 0) {
			util::fatal("read");
		}
		(void)printf("read from socket: %zd bytes: %s\n", rcv_len, buffer);
	}

	return 0;
}
