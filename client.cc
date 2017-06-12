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

constexpr const char* usage_msg =
"USAGE:  ./siktacka-client player_name game_server_host[:port] [ui_server_host[:port]]\n"
"  player_name – 0-64 drukowalne znaki ASCII (bez spacji, \"\" oznacza obserwatora\n"
"  game_server_host – adres IPv4, IPv6 lub nazwa wêz³a\n"
"  game_server_port - port serwera gry (domyœlnie 12345)\n"
"  ui_server_host   - nazwa serwera interfejsu u¿ytkownika (domyœlnie localhost)\n"
"  ui_server_port   - port serwera interfejsu u¿ytkownika (domyœlnie 12346)\n";

struct server_connection {
	int socket;
	int family;
	int protocol;
	int desired_socktype;
	std::string hostname;
	std::uint16_t port;
};

static server_connection servers[2] = {
	{ 0, 0, 0, SOCK_DGRAM, "", 12345 }, // game server
	{ 0, 0, 0, SOCK_STREAM, "localhost", 123456 } // ui server
};

static server_connection& game_server = servers[0];
static server_connection& gui_server = servers[1];

static std::uint64_t session_id;
static std::string player_name;

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
	auto server_address = split_hostname(argv[2]);
	game_server.hostname = std::get<0>(server_address);
	if (std::get<1>(server_address))
	{
		game_server.port = parse<std::uint16_t>(std::get<2>(server_address).data(), 0, 65535);
	}
	// Ditto optionally for ui server hostname
	if (argc >= 4)
	{
		auto server_address = split_hostname(argv[3]);
		gui_server.hostname = std::get<0>(server_address);
		if (std::get<1>(server_address))
		{
			gui_server.port = parse<std::uint16_t>(std::get<2>(server_address).data(), 0, 65535);
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
#define close closesocket
#endif
	
	// Try to create sockets and connect via either IPv4 or IPv6 to game and ui server
	for (auto& server : servers)
	{
		const auto port = std::to_string(server.port);

		addrinfo hints, *res;
		memset(&hints, 0x00, sizeof(hints));
		hints.ai_socktype = server.desired_socktype;

		int error = getaddrinfo(server.hostname.c_str(), port.c_str(), &hints, &res);
		if (error != 0)
			util::fatal("Couldn't resolve ", server.hostname.c_str());

		addrinfo* p;
		for (p = res; p != nullptr; p = p->ai_next)
		{
			auto& sock = server.socket;
			if ((sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol) == -1))
			{
				perror("socket");
				continue;
			}

			if (connect(sock, p->ai_addr, p->ai_addrlen) == -1)
			{
				perror("connect");
				close(sock);
				continue;
			}

			server.family = p->ai_family;
			server.protocol = p->ai_protocol;
			break;
		}

		if (p == nullptr)
			util::fatal("Couldn't establish a connection to ", server.hostname.c_str());

		freeaddrinfo(res);
	}

	// Turn off Nagle's algorithm for GUI TCP connection
	int off = 1;
	int result = setsockopt(gui_server.socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&off, sizeof(off));
	if (result < 0)
		util::fatal("Couldn't turn off Nagle's algorithm");

	// TODO: Implement server<>client and client<>GUI communication

	return 0;
}
