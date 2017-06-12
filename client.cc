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
#include <thread>
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
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#define _BSD_SOURCE
#include <endian.h>
#endif

#include "protocol.h"
#include "util.h"
#include "rand.h"
#include "map.h"

using namespace std::chrono;

constexpr const char* usage_msg =
"USAGE:  ./siktacka-client player_name game_server_host[:port] [ui_server_host[:port]]\n"
"  player_name      - 0-64 drukowalne znaki ASCII (bez spacji, \"\" oznacza obserwatora\n"
"  game_server_host - adres IPv4, IPv6 lub nazwa w�z�a\n"
"  game_server_port - port serwera gry (domy�lnie 12345)\n"
"  ui_server_host   - nazwa serwera interfejsu u�ytkownika (domy�lnie localhost)\n"
"  ui_server_port   - port serwera interfejsu u�ytkownika (domy�lnie 12346)\n";

struct server_connection {
	int socket;
	std::unique_ptr<sockaddr> addr = nullptr;
	size_t addrlen = 0;
	int family;
	int protocol;
	int desired_socktype;
	std::string hostname;
	std::uint16_t port;
};

static server_connection servers[2] = {
	{ 0, nullptr, 0, 0, 0, SOCK_DGRAM, "", (std::uint16_t)12345 }, // game server
	{ 0, nullptr, 0, 0, 0, SOCK_STREAM, "localhost", (std::uint16_t)12346 } // ui server
};

static server_connection& game_server = servers[0];
static server_connection& gui_server = servers[1];

// TODO: Handle this
static std::uint32_t game_id;
static std::uint32_t next_expected_event;

static std::uint32_t maxx;
static std::uint32_t maxy;
static std::vector<std::string> active_player_names;

static std::uint64_t session_id;
static std::string player_name;
static std::int8_t turn_direction;

static std::vector<std::shared_ptr<event>> queued_events;
static std::recursive_mutex events_lock;

namespace {
	std::chrono::microseconds current_time_microseconds()
	{
		return duration_cast<std::chrono::microseconds>(
			system_clock::now().time_since_epoch());
	}

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

void send_game_job()
{
	constexpr std::chrono::milliseconds HEARTBEAT_INTERVAL { 20 };
	while (true)
	{
		auto start_time = current_time_microseconds();
		{
			client_message msg { session_id, turn_direction, next_expected_event };
			memcpy(msg.player_name, player_name.data(), player_name.size());

			const auto buffer = msg.as_stream();
			// Send heartbeat to 
			ssize_t snd_len = sendto(game_server.socket, (const char*)buffer.data(), buffer.size(), 0,
				(sockaddr*)game_server.addr.get(), game_server.addrlen);
			if ((size_t)snd_len != buffer.size())
				util::fatal("Error sending heartbeat message to server");

		}
		auto elapsed = current_time_microseconds() - start_time;
		if (elapsed < HEARTBEAT_INTERVAL)
		{
			auto remainder = HEARTBEAT_INTERVAL - elapsed;
			std::this_thread::sleep_for(remainder);
		}
	}
}

constexpr int RECV_BUFFER_SIZE = 10000;

void receive_game_job()
{
	char buffer[RECV_BUFFER_SIZE];
	while (true)
	{
		auto read_len = recv(game_server.socket, buffer, RECV_BUFFER_SIZE, 0);

		auto pair = server_message::from(buffer, read_len);
		if (pair.second == false)
			continue;

		// Verify data from the server
		const server_message& msg = pair.first;
		for (auto event : msg.events)
		{
			switch (event->event_type)
			{
			case NEW_GAME:
			{
				game_id = msg.game_id;

				new_game* new_game = static_cast<struct new_game*>(event.get());
				active_player_names = new_game->player_names;
				maxx = new_game->maxx;
				maxy = new_game->maxy;
				break;
			}
			case PIXEL:
			{
				pixel* pixel = static_cast<struct pixel*>(event.get());
				if (pixel->player_number >= active_player_names.size() || pixel->x > maxx || pixel->y > maxy)
					util::fatal("PIXEL event from server contains invalid data, quitting");
				break;
			}
			case PLAYER_ELIMINATED:
			{
				player_eliminated* elim = static_cast<player_eliminated*>(event.get());
				if (elim->player_number >= active_player_names.size())
					util::fatal("PLAYER_ELIMINATED contains invalid player number");
				break;
			}
			default: break;
			}

			if (next_expected_event == event->event_no)
			{
				std::lock_guard<std::recursive_mutex> _lock(events_lock);

				next_expected_event++;
				queued_events.push_back(event);
			}
		}
	}
}

void send_gui_job()
{
	while (true)
	{
		while (queued_events.size() == 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));

		std::lock_guard<std::recursive_mutex> _lock(events_lock);

		constexpr int BUFFER_SIZE = 2000;
		char buffer[BUFFER_SIZE];
		for (const auto& event : queued_events)
		{
			memset(buffer, 0x00, BUFFER_SIZE);
			switch (event->event_type)
			{
			case NEW_GAME:
			{
				new_game* new_game = static_cast<struct new_game*>(event.get());
				sprintf(buffer, "NEW_GAME %u %u ", new_game->maxx, new_game->maxy);
				auto cur_buf = buffer + strlen(buffer);
				for (size_t i = 0; i < new_game->player_names.size(); ++i)
				{
					sprintf(cur_buf, "%s", cur_buf);
					cur_buf += new_game->player_names[i].size();
					// Separate every player name but last
					if (i + 1 < new_game->player_names.size())
					{
						*(cur_buf++) = ' ';
					}
				}
				*cur_buf = '\n';

				active_player_names = new_game->player_names;
				maxx = new_game->maxx;
				maxy = new_game->maxy;
				break;
			}
			case PIXEL:
			{
				pixel* pixel = static_cast<struct pixel*>(event.get());
				sprintf(buffer, "PIXEL %u %u %s\n", pixel->x, pixel->y, active_player_names[pixel->player_number].c_str());
				break;
			}
			case PLAYER_ELIMINATED:
			{
				player_eliminated* elim = static_cast<player_eliminated*>(event.get());
				sprintf(buffer, "PLAYER_ELIMINATED %s\n", active_player_names[elim->player_number].c_str());
				break;
			}
			default: break;
			}

			// TODO: Verify if we need GUI sockaddr for TCP connection
			auto buf_len = strlen(buffer);
			ssize_t snd_len = sendto(gui_server.socket, (const char*)buffer, strlen(buffer), 0,
				(sockaddr*)gui_server.addr.get(), gui_server.addrlen);
			if ((size_t)snd_len != buf_len)
				util::fatal("Could not succesfully send all data to gui_server");
		}
		queued_events.clear();
	}
}

void receive_gui_job()
{
	bool key_pressed[2] = { false, false };
	
	constexpr std::pair<const char*, int> messages[] = {
		std::make_pair("LEFT_KEY_UP", sizeof("LEFT_KEY_UP")),
		std::make_pair("LEFT_KEY_DOWN", sizeof("LEFT_KEY_DOWN")),
		std::make_pair("RIGHT_KEY_UP", sizeof("RIGHT_KEY_UP")),
		std::make_pair("RIGHT_KEY_DOWN", sizeof("RIGHT_KEY_DOWN"))
	};

	constexpr int BUFFER_SIZE = 100000;
	char buffer[BUFFER_SIZE];
	// Since proper messages are newline-separated and TCP operates on stream of data,
	// copy first 16 signs and match the message
	char match_buffer[16] = { 0 };
	size_t msg_len = 0;

	while (true)
	{
		ssize_t read_len = recv(gui_server.socket, buffer, BUFFER_SIZE, 0);
		if (read_len == 0 && errno != 0)
		{
			std::perror("Connection to GUI failed");
			std::exit(1); // TODO: Verify that client exits
		}

		const char* pointer = buffer;
		while (pointer < buffer + read_len)
		{
			if (*pointer != '\n' && msg_len < sizeof(match_buffer))
			{
				match_buffer[msg_len] = *pointer;
				msg_len++;
			}
			// End of message and current message length in valid range (to avoid needless checks), check for message
			if (*pointer == '\n' && msg_len >= sizeof("LEFT_KEY_UP") && msg_len <= sizeof("RIGHT_KEY_DOWN"))
			{
				for (size_t i = 0; i < sizeof(messages) / sizeof(messages[0]); ++i)
				{
					// Directly map messages to the keys and values (pair of LEFT/RIGHT and UP/DOWN)
					if (strncmp(match_buffer, messages[i].first, messages[i].second) == 0)
					{
						key_pressed[i / 2] = (i % 2);
					}
				}
			}
			// Reset matching buffer, since every valid message is separated by newlines
			if (*pointer == '\n')
			{
				memset(match_buffer, 0x00, sizeof(match_buffer));
				msg_len = 0;
			}
			pointer++;
		}
		// Update turn direction after change
		turn_direction = (1 * key_pressed[0]) + (-1 * key_pressed[1]);
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

			server.addr = std::make_unique<sockaddr>(*p->ai_addr);
			server.addrlen = p->ai_addrlen;
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
	std::thread recv_game(receive_game_job);
	std::thread recv_gui(receive_gui_job);
	std::thread send_game(send_game_job);
	std::thread send_gui(send_gui_job);

	recv_game.join();
	recv_gui.join();
	send_game.join();
	send_gui.join();

	return 0;
}
