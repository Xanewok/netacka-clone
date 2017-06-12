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
#include <thread>

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

using namespace std::chrono;


constexpr const char* usage_msg =
"USAGE:  ./siktacka-server [-W n] [-H n] [-p n] [-s n] [-t n] [-r n]\n"
"  -W n – szerokość planszy w pikselach (domyślnie 800)\n"
"  -H n – wysokość planszy w pikselach (domyślnie 600)\n"
"  -p n – numer portu (domyślnie 12345)\n"
"  -s n – liczba całkowita wyznaczająca szybkość gry (parametr\n"
"          ROUNDS_PER_SEC w opisie protokołu, domyślnie 50)\n"
"  -t n – liczba całkowita wyznaczająca szybkość skrętu (parametr\n"
"          TURNING_SPEED, domyślnie 6)\n"
"  -r n – ziarno generatora liczb losowych (opisanego poniżej)\n";

constexpr int MAX_CLIENTS = 42;
constexpr std::chrono::milliseconds CLIENT_CONNECTION_TIMEOUT = 2000ms;

static Rand rand_gen;

static struct {
	std::uint32_t width = 800;
	std::uint32_t height = 600;
	std::uint16_t port_num = 12345;
	std::uint32_t rounds_per_sec = 50;
	std::uint32_t turning_speed = 6;
	std::uint32_t rand_seed;
	bool seed_provided = false;
} configuration;

static std::chrono::microseconds round_budget_microseconds()
{
	using namespace std::chrono_literals;

	auto budget = std::chrono::milliseconds{ 1s } / configuration.rounds_per_sec;
	return budget;
}

std::chrono::microseconds current_time_microseconds()
{
	return duration_cast<std::chrono::microseconds>(
		system_clock::now().time_since_epoch());
}

std::chrono::milliseconds current_time_ms()
{
	return duration_cast<std::chrono::milliseconds>(
		system_clock::now().time_since_epoch());
}

// Players are identified by (socket, session_id) pair
enum client_state {
	playing, // actively playing now during current game
	waiting, // wants to join next game
	spectating, // only spectates and doesn't want to join
};

struct server_player;

constexpr static std::chrono::milliseconds MIN_MESSAGE_DELAY { 2 };
struct client_connection {
	sockaddr_storage socket;

	client_message last_message;
	std::chrono::milliseconds last_message_time;

	server_player* player = nullptr;

	bool ready_to_play = false; // pressed arrow when waiting for NEW_GAME
	client_state state = client_state::spectating;

	bool is_playing() const
	{
		return player != nullptr;
	}

	bool is_inactive() const
	{
		return (current_time_ms() - last_message_time) > CLIENT_CONNECTION_TIMEOUT;
	}
};

struct player
{
	std::string name;
	std::uint8_t player_id;
	bool eliminated = false;
};

struct server_player : public player
{
	double x = 0;
	double y = 0;
	double rotation = 0; // in degrees, clockwise, 0* = right
	std::uint8_t turn_direction = 0;
	client_connection* connection = nullptr;
};

// Helper struct that allows to compare sockaddr_in6 structures and also to
// convert them to comparable tuples (pair of (addr, port) identifies the struct)
struct in6_addr_port_compare
{
	template<size_t off>
	static std::uint64_t addr_part(const sockaddr_in6& addr)
	{
		return *reinterpret_cast<const std::uint64_t*>(addr.sin6_addr.s6_addr + off);
	}

	static auto as_tuple(const sockaddr_in6& sock)
	{
		return std::make_tuple(addr_part<0>(sock), addr_part<8>(sock), sock.sin6_port);
	}

	static auto as_tuple(const sockaddr_in& sock)
	{
		return std::make_tuple(sock.sin_addr.s_addr, sock.sin_port);
	}

	bool operator()(const sockaddr_storage& lhs, const sockaddr_storage& rhs) const
	{
		if (lhs.ss_family == rhs.ss_family)
		{
			if (lhs.ss_family == AF_INET6)
				return as_tuple(*reinterpret_cast<const sockaddr_in6*>(&lhs))
					 < as_tuple(*reinterpret_cast<const sockaddr_in6*>(&rhs));
			else
				return as_tuple(*reinterpret_cast<const sockaddr_in*>(&lhs))
					 < as_tuple(*reinterpret_cast<const sockaddr_in*>(&rhs));
		}
		else
			return lhs.ss_family < rhs.ss_family;
	}
};

using player_collection_t = std::map<sockaddr_storage, client_connection, in6_addr_port_compare>;
static struct {
	std::uint32_t game_id;
	bool in_progress = false;
	struct map map;
	std::vector<std::shared_ptr<event>> events;

	player_collection_t clients;

	// Cached players with ordering for given game (reinitialized for every game)
	std::vector<server_player> players;

	std::recursive_mutex lock; // TODO: Replace with fair, priority mutex
} game_state;

void prune_inactive_clients()
{
	for (auto it = game_state.clients.begin(); it != game_state.clients.end();)
	{
		client_connection& client = it->second;

		if (client.is_inactive())
		{
			if (client.player)
				client.player->connection = nullptr;

			it = game_state.clients.erase(it);
		}
		else
			++it;
	}
}

// TODO: FACTOR OUT SERVER STATE AND CONTAINING RANDOM GENERATOR, CONFIGURATION, OPEN SOCKET AND PLAYER CONNECTIONS
// + PLAYER GAME STATE
// STRUCTS AND DEFS COULD BE IN SERVER.H AND THIS FILE WOULD ONLY BASICALLY PARSE ARGS, OPEN SOCKET

void cleanup_game()
{
	game_state.in_progress = false;
	game_state.map = map(configuration.width, configuration.height);
	game_state.events.clear(); // TODO: Verify

	for (auto& client_kv : game_state.clients)
	{
		auto& client = client_kv.second;

		if (client.state == client_state::playing)
		{
			client.state = client_state::waiting;
			client.player = nullptr;

			client.ready_to_play = false;
		}
	}
	game_state.players.clear();
}

static int server_socket;

// Returns how many events were sent to client
int broadcast_events(const std::vector<std::shared_ptr<event>>& events, std::uint32_t game_id,
	const sockaddr_storage& client_socket, std::uint32_t next_expected_event, size_t max_send_count = 5)
{
	const auto event_count = events.size();
	if (next_expected_event > event_count)
		return 0;

	auto send_count = std::min(event_count - next_expected_event, max_send_count);
	if (send_count == 0)
		return 0;

	server_message msg;
	msg.game_id = game_id;

	size_t sent = 0;
	while (sent < send_count)
	{
		msg.events.clear();

		// Try to split and pack requested events into different server_messages
		// respecting maximum size of events packet data payload
		int events_size = 0;
		for (; sent < send_count; ++sent)
		{
			const auto& event = events[next_expected_event + sent];
			const auto event_length = event->calculate_total_len_with_crc32();

			if (events_size + event_length > server_message::MAX_EVENTS_LEN)
				break;
			
			events_size += event_length;
			msg.events.push_back(event);
		}

		const auto buffer = msg.as_stream();
		if (buffer.size() != sizeof(msg.game_id) + events_size) {
			fprintf(stderr, "Mismatch between size of serialized and prepared server_message");
		}

		int flags = 0;
#ifdef linux
		flags = MSG_DONTWAIT;
#endif

		ssize_t snd_len = sendto(server_socket, (const char*)buffer.data(), buffer.size(), flags,
			(sockaddr*)&client_socket, sizeof(client_socket));

		const bool would_block = errno == EAGAIN || errno == EWOULDBLOCK;

		if (!would_block && snd_len != static_cast<ssize_t>(buffer.size()))
			fprintf(stderr, "Error sending event: %s\n", buffer.data());
	}

	return sent;
}

void generate_event(std::shared_ptr<event> event)
{
	// First broadcast the event
	event->event_no = game_state.events.size();
	game_state.events.push_back(event);

	auto raw_event = event.get();

	// Then act accordingly
	switch (event->event_type)
	{
	// try_start_game is the only function responsible for generating NEW_GAME
	// and is already initializing game, so don't do anything here
	case NEW_GAME:
		break;
	case PIXEL:
	{
		pixel* pixel_event = static_cast<pixel*>(raw_event);

		game_state.map.pixels.insert(map::make_pos(pixel_event->x, pixel_event->y));
		break;
	}
	case PLAYER_ELIMINATED:
	{
		std::int8_t player_num = static_cast<player_eliminated*>(raw_event)->player_number;
		game_state.players[player_num].eliminated = true;

		int living_count = 0;
		for (const auto& player : game_state.players)
			living_count += player.eliminated ? 0 : 1;

		if (living_count == 1)
			generate_event(std::make_shared<game_over>());
		break;
	}
	case GAME_OVER:
	{
		cleanup_game();
		break;
	}
	default: fprintf(stderr, "Generating unknown message (type: %d)\n", event->event_type);
	}
}

bool try_start_game()
{
	std::vector<client_connection*> ready_clients;
	int player_names_len = 0;
	for (auto& client_kv : game_state.clients)
	{
		client_connection& client = client_kv.second;
		if (client.state == client_state::waiting && client.ready_to_play)
		{
			const int name_len = strlen(client.last_message.player_name) + sizeof('\0');
			if (player_names_len + name_len <= MAX_PLAYER_NAMES_LEN)
			{
				player_names_len += name_len;
				ready_clients.push_back(&client);
			}
		}
	}

	constexpr int MIN_PLAYERS = 2;
	if (ready_clients.size() < MIN_PLAYERS)
	{
		return false;
	}
	// We can start a new game now!
	else
	{
		const auto player_count = ready_clients.size();

		// Initialize
		game_state.in_progress = true;

		game_state.players.resize(player_count);
		for (size_t i = 0; i < player_count; ++i)
		{
			client_connection* client = ready_clients[i];
			client->state = client_state::playing;

			server_player& player = game_state.players[i];
			player.connection = client;
			player.name = std::string(client->last_message.player_name);
		}

		// Sort players based on name so we can give consistent ids for given names
		std::sort(game_state.players.begin(), game_state.players.end(),
			[](const server_player& lhs, const server_player& rhs)
			{
				return lhs.name < rhs.name;
			}
		);

		std::vector<std::string> player_names(player_count);
		for (size_t i = 0; i < player_count; ++i)
		{
			server_player& player = game_state.players[i];
			player.player_id = static_cast<std::uint8_t>(i);

			player_names[i] = player.name;
		}

		// Use exact specified order/algorithm from the assignment
		game_state.game_id = rand_gen.next();

		generate_event(std::make_shared<new_game>(game_state.map.width, game_state.map.height,
			player_names));

		for (auto& player : game_state.players)
		{
			player.x = (rand_gen.next() % game_state.map.width) + 0.5f; // TODO: Verify if width == maxx
			player.y = (rand_gen.next() % game_state.map.height) + 0.5f; // ^ for height
			player.rotation = (rand_gen.next() % 360);

			if (game_state.map.is_occupied(player.x, player.y))
				generate_event(std::make_shared<player_eliminated>(player.player_id));
			else
			{
				const auto pos = map::make_pos(player.x, player.y);
				generate_event(std::make_shared<pixel>(player.player_id, pos.first, pos.second));
			}
		}
	}

	return true;
}

void handle_client_message(const client_message& msg, const struct sockaddr_storage& sock)
{
	const bool wants_to_spectate = (strlen(msg.player_name) == 0);

	auto it = game_state.clients.find(sock);
	if (it != game_state.clients.end())
	{
		auto& client = it->second;

		const auto cur_session_id = client.last_message.session_id;
		// Ignore incoming messages for existing client with lower session_id
		if (msg.session_id < cur_session_id)
			return;
		// If incoming session_id is bigger for existing client then disconnect
		// existing one and replace with the new one
		else if (msg.session_id > cur_session_id)
		{
			client.ready_to_play = false;
			client.player = nullptr; // disconnect from the player in case he's playing now
			client.state = wants_to_spectate ? client_state::spectating : client_state::waiting;
		}
		// Existing client tries to flood us, ignore it
		else if (current_time_ms() - client.last_message_time < MIN_MESSAGE_DELAY)
		{
			return;
		}
	}
	// New client joined
	else
	{
		// Respect limit of connected clients
		if (game_state.clients.size() >= MAX_CLIENTS)
			return;

		it = std::find_if(game_state.clients.begin(), game_state.clients.end(),
			[&msg](const auto& client_kv)
			{
				return strcmp(client_kv.second.last_message.player_name, msg.player_name) == 0;
			}
		);
		// Ignore messages from unknown socket with the same name as existing client (incl. spectators)
		if (it != game_state.clients.end())
			return;

		client_connection client;
		client.socket = sock;	
		client.state = wants_to_spectate ? client_state::spectating : client_state::waiting;

		it = game_state.clients.emplace(sock, client).first;
	}
	// Update last message and timestamp
	client_connection& client = it->second;
	client.last_message = msg;
	client.last_message_time = current_time_ms();

	// Handle possible game state change
	if (wants_to_spectate)
	{
		client.ready_to_play = false;
		client.player = nullptr;
		client.state = client_state::spectating;
	}
	else if (client.is_playing())
	{
		client.player->turn_direction = msg.turn_direction;
	}
	// Wants to play but doesn't yet
	else if (!game_state.in_progress && msg.turn_direction != 0)
	{
		client.ready_to_play = true;

		// If we managed to start a game, we generated NEW_GAME and sent appropriate
		// events to all other players; don't do it now
		if (try_start_game())
			return;
	}
}

void do_game_tick()
{
	for (auto& player : game_state.players)
	{
		player.rotation += player.turn_direction * configuration.turning_speed;
		player.rotation = fmod(player.rotation, 360);

		auto old_pos = map::make_pos(player.x, player.y);
		// Rotation are degrees going clock-wise, so negate deg for (cos deg, sin deg) unit vector
		// Move by a unit in given direction
		player.x += cos(-player.rotation * M_PI / 180);
		player.y += sin(-player.rotation * M_PI / 180);

		auto new_pos = map::make_pos(player.x, player.y);

		if (old_pos == new_pos)
			continue;

		if (!game_state.map.is_inside(new_pos) || game_state.map.is_occupied(new_pos))
			generate_event(std::make_shared<player_eliminated>(player.player_id));
		else
			generate_event(std::make_shared<pixel>(player.player_id, new_pos.first, new_pos.second));

		// After every player update we need to check if the game has finished
		// if so, the players are invalid, so just exit the game
		if (!game_state.in_progress)
			break;
	}
}

void update_game_job()
{
	int tick_count = 1;
	while (true)
	{
		auto start_time = current_time_microseconds();
		{
			std::lock_guard<std::recursive_mutex> _lock(game_state.lock);

			if (game_state.in_progress)
			{
				do_game_tick();
			}

			constexpr int PRUNE_EVERY_TICKS = 15;
			tick_count = (tick_count + 1) % PRUNE_EVERY_TICKS;
			if (tick_count == 0)
			{
				prune_inactive_clients();
			}
		}
		auto elapsed = current_time_microseconds() - start_time;
		if (elapsed < round_budget_microseconds())
		{
			auto remainder = round_budget_microseconds() - elapsed;
			std::this_thread::sleep_for(remainder);
		}
	}
}

void receive_messages_job()
{
	constexpr int MSG_BUFFER_SIZE = 10000;
	char buffer[MSG_BUFFER_SIZE];
	struct sockaddr_storage client_address;

	while (true)
	{
		socklen_t rcva_len = (socklen_t) sizeof(client_address);
		ssize_t len = recvfrom(server_socket, buffer, sizeof(buffer), 0,
			(struct sockaddr *) &client_address, &rcva_len);
		if (len < 0)
		{
			fprintf(stderr, "error on datagram from client socket\n");
			continue;
		}

		if (len > MSG_BUFFER_SIZE) {
			fprintf(stderr, "read from socket message exceeding %d bytes, ignoring\n", MSG_BUFFER_SIZE);
			continue;
		}

		fprintf(stderr, "read from socket: %zd bytes: %.*s\n", len, (int)len, buffer);
		auto parsed_msg = client_message::from(buffer, len);
		if (parsed_msg.second == false) {
			fprintf(stderr, "Error parsing message (hex): ");
			for (int i = 0; i < len; ++i)
				fprintf(stderr, "%02X", buffer[i]);
			fprintf(stderr, "\n");
		}
		else
		{
			// TODO: Replace with fair, low priority lock
			std::lock_guard<std::recursive_mutex> _lock(game_state.lock);

			handle_client_message(parsed_msg.first, client_address);
		}
	}
}

void send_events_job()
{
	constexpr std::chrono::milliseconds SEND_INTERVAL { 5 };

	std::uint32_t game_id;
	std::vector<std::shared_ptr<event>> events;
	std::map<sockaddr_storage, uint32_t, in6_addr_port_compare> clients;
	while (true)
	{
		// We can afford to send stale data, so lock for a short period of time and copy data for sending
		{
			// TODO: Replace with fair, low priority lock
			std::lock_guard<std::recursive_mutex> _lock(game_state.lock);
			game_id = game_state.game_id;
			events = game_state.events;

			clients.clear();
			for (auto& kv : game_state.clients)
				clients[kv.first] = kv.second.last_message.next_expected_event;
		}

		int sent_events = 0;
		for (auto& kv : clients)
			sent_events += broadcast_events(events, game_id, kv.first, kv.second);

		std::this_thread::sleep_for(SEND_INTERVAL);
	}
}

namespace {
	void ensure_with_errno(int value, const char* msg)
	{
		if (value < 0) {
			perror(msg);
			std::exit(1);
		}
	}

	template<typename T>
	T parse(const char* str, T min, T max)
	{
		T value;
		try
		{
			value = static_cast<T>(util::parse_bounded(str, min, max));
		}
		catch (std::exception& e)
		{
			util::fatal("Invalid argument %s (%s)", str, e.what());
			std::exit(1);
		}
		return value;
	}
} // namespace

int main(int argc, char* argv[])
{
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

	// Parse optional arguments
	for (int i = 1; i < argc; i += 2)
	{
		const char* arg = argv[i];
		if (arg[0] != '-' || strlen(arg) != 2 || i + 1 >= argc)
		{
			printf("Bad argument: %s%s\n%s",
				arg, (i + 1 >= argc ? " (missing parameter)" : ""), usage_msg);
			std::exit(1);
		}

		switch (arg[1])
		{
		case 'W':
		{
			configuration.width = parse<std::uint32_t>(argv[i + 1],
				1, std::numeric_limits<std::uint32_t>::max());
			break;
		}
		case 'H':
		{
			configuration.height = parse<std::uint32_t>(argv[i + 1],
				1, std::numeric_limits<std::uint32_t>::max());
			break;
		}
		case 'p':
		{
			configuration.port_num = parse<std::uint16_t>(argv[i + 1], 0, 65535);
			break;
		}
		case 's':
		{
			configuration.rounds_per_sec = parse<std::uint32_t>(argv[i + 1],
				1, std::numeric_limits<uint32_t>::max());
			break;
		}
		case 't':
		{
			configuration.turning_speed = parse<std::uint32_t>(argv[i + 1],
				0, std::numeric_limits<int>::max());
			break;
		}
		case 'r':
		{
			configuration.rand_seed = parse<std::uint32_t>(argv[i + 1],
				std::numeric_limits<std::uint32_t>::min(),
				std::numeric_limits<std::uint32_t>::max());
			configuration.seed_provided = true;
			break;
		}
		default:
		{
			printf("Bad argument: %s\n%s", arg, usage_msg);
			std::exit(1);
		}
		}
	}
	// Initialize map
	game_state.map = map(configuration.width, configuration.height);

	// Initialize deterministic random generator
	rand_gen = Rand(configuration.seed_provided
		? configuration.rand_seed
		: static_cast<std::uint32_t>(time(nullptr)));

	// Initialized IPv6 UPD socket for client-connection
	server_socket = socket(AF_INET6, SOCK_DGRAM, 0);
#ifdef _WIN32
	fprintf(stderr, "socket: WSAGetLastError: %d\n", WSAGetLastError());
#endif
	fprintf(stderr, "errno: %d\n", errno);
	ensure_with_errno(server_socket, "socket");

	// Disable IPv6-only option for sockets
	int no = 0;
	setsockopt(server_socket, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&no, sizeof(no));

	struct sockaddr_in6 server_address;
	memset(&server_address, 0x00, sizeof(server_address));
	server_address.sin6_family = AF_INET6;
	server_address.sin6_addr = in6addr_any;
	server_address.sin6_port = htons(configuration.port_num);

	int ret = bind(server_socket, (struct sockaddr *) &server_address, (socklen_t) sizeof(server_address));
#ifdef _WIN32
	fprintf(stderr, "bind: WSAGetLastError: %d\n", WSAGetLastError());
#endif
	fprintf(stderr, "errno: %d\n", errno);
	ensure_with_errno(ret, "bind");

	std::thread recv(receive_messages_job);
	std::thread send(send_events_job);
	std::thread update(update_game_job);

	recv.join();
	send.join();
	update.join();

	return 0;
}
