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

using namespace std::chrono;
using namespace std::chrono_literals;

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

constexpr std::chrono::milliseconds CLIENT_CONNECTION_TIMEOUT = 2000ms;

std::chrono::milliseconds current_time_ms()
{
	 return duration_cast<std::chrono::milliseconds>(
		 system_clock::now().time_since_epoch());
}

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

static float get_cached_round_time_ms()
{
	static float round_time = 1000.f / configuration.rounds_per_sec;
	return round_time;
}

// Players are identified by (socket, session_id) pair
enum client_state {
	playing, // actively playing now during current game
	waiting, // wants to join next game
	spectating, // only spectates and doesn't want to join
};

struct server_player;

struct client_connection {
	sockaddr_in6 socket;

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
		return *reinterpret_cast<const std::uint64_t*>(addr.sin6_addr.s6_addr[off]);
	}

	static auto as_tuple(const sockaddr_in6& sock)
	{
		return std::make_tuple(addr_part<0>(sock), addr_part<4>(sock), sock.sin6_port);
	}

	bool operator()(const sockaddr_in6& lhs, const sockaddr_in6& rhs) const
	{
		return as_tuple(lhs) < as_tuple(rhs);
	}
};

using player_collection_t = std::map<sockaddr_in6, client_connection, in6_addr_port_compare>;
static struct {
	std::uint32_t game_id;
	bool in_progress = false;
	struct map map;
	std::vector<std::shared_ptr<event>> events;

	player_collection_t clients;

	// Cached players with ordering for given game (reinitialized for every game)
	std::vector<server_player> players;

	std::chrono::milliseconds next_game_tick = std::chrono::milliseconds(0);
} game_state;

// TODO: Use it
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
		}
	}
	game_state.players.clear();
}

static int server_socket;

// TODO: Send series of messages up to next_expected_event for each player individually
// TODO: bundle up net messages
void broadcast_event(struct event* event)
{
	const auto buffer = event->as_stream();

	for (const auto& client_kv : game_state.clients)
	{
		const sockaddr_in6& client_address = client_kv.first;

		ssize_t snd_len = sendto(server_socket, (const char*)buffer.data(), buffer.size(), 0,
			(sockaddr*)&client_address, sizeof(client_address));

		if (snd_len != static_cast<ssize_t>(buffer.size()))
			fprintf(stderr, "Error sending event: %s\n", buffer.data());
	}
}

// Returns how many events were sent to client
int broadcast_events(const client_connection& client, size_t max_send_count = 5)
{
	const auto next_expected = client.last_message.next_expected_event;
	const auto event_count = game_state.events.size();
	if (next_expected > event_count)
		return 0;

	auto send_count = std::min(event_count - next_expected, max_send_count);
	if (send_count == 0)
		return 0;

	size_t sent = 0;
	while (sent < send_count)
	{
		server_message msg;
		msg.game_id = game_state.game_id;

		// Try to split and pack requested events into different server_messages
		// respecting maximum size of events packet data payload
		int events_size = 0;
		for (; sent < send_count; ++sent)
		{
			const event& event = *game_state.events[next_expected + sent];
			const auto event_length = event.calculate_total_len_with_crc32();

			if (events_size + event_length > server_message::MAX_EVENTS_LEN)
				break;
			else
				events_size += event_length;
		}

		const auto buffer = msg.as_stream();

		int flags = 0;
#ifdef linux
		flags = MSG_DONTWAIT;
#endif
		const sockaddr_in6& client_address = client.socket;
		ssize_t snd_len = sendto(server_socket, (const char*)buffer.data(), buffer.size(), flags,
			(sockaddr*)&client_address, sizeof(client_address));

		const bool would_block = errno == EAGAIN || errno == EWOULDBLOCK;

		if (!would_block && snd_len != static_cast<ssize_t>(buffer.size()))
			fprintf(stderr, "Error sending event: %s\n", buffer.data());
	}

	return sent;
}

void generate_event(std::shared_ptr<event> event)
{
	// First broadcast the event
	event->event_no = game_state.events.size() + 1;

	// TODO: Send series of messages up to next_expected_event for each player individually
	auto raw_event = event.get();
	broadcast_event(raw_event);

	game_state.events.push_back(std::move(event));

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
			player.x = (rand_gen.next() % game_state.map.height) + 0.5f; // ^ for height
			player.turn_direction = static_cast<std::uint8_t>(rand_gen.next() % 360);

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

void handle_client_message(const client_message& msg, const struct sockaddr_in6& sock)
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
	}
	// New client joined
	else
	{
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

	// TODO: Send series of messeges up to next_expected_event
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
} // namespace

int main(int argc, char* argv[])
{
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
			configuration.width = static_cast<std::uint32_t>(
				util::parse_bounded(argv[i + 1],
				1, std::numeric_limits<std::uint32_t>::max()));
			break;
		}
		case 'H':
		{
			configuration.height = static_cast<std::uint32_t>(
				util::parse_bounded(argv[i + 1],
				1, std::numeric_limits<std::uint32_t>::max()));
			break;
		}
		case 'p':
		{
			configuration.port_num = static_cast<std::uint16_t>(
				util::parse_bounded(argv[i + 1], 0, 65535));
			break;
		}
		case 's':
		{
			configuration.rounds_per_sec = static_cast<std::uint32_t>(
				util::parse_bounded(argv[i + 1],
				1, std::numeric_limits<int>::max()));
			break;
		}
		case 't':
		{
			configuration.turning_speed = static_cast<std::uint32_t>(
				util::parse_bounded(argv[i + 1],
				0, std::numeric_limits<int>::max()));
			break;
		}
		case 'r':
		{
			configuration.rand_seed = static_cast<std::uint32_t>(
				util::parse_bounded(argv[i + 1],
				std::numeric_limits<std::uint32_t>::min(),
				std::numeric_limits<std::uint32_t>::max()));
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
	ensure_with_errno(server_socket, "socket");

	// Disable IPv6-only option for sockets
	int no = 0;
	setsockopt(server_socket, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&no, sizeof(no));

	struct sockaddr_in6 server_address;
	server_address.sin6_family = AF_INET6;
	server_address.sin6_addr = in6addr_any;
	printf("%d\n", configuration.port_num);
	server_address.sin6_port = htons(configuration.port_num);

	int ret = bind(server_socket, (struct sockaddr *) &server_address, (socklen_t) sizeof(server_address));
	ensure_with_errno(ret, "bind");

	// Receive data from clients
	constexpr int BUFFER_SIZE = 100000;
	char buffer[BUFFER_SIZE];
	struct sockaddr_in6 client_address;
	//socklen_t snda_len = (socklen_t) sizeof(client_address);
	while (true)
	{
		socklen_t rcva_len = (socklen_t) sizeof(client_address);
		ssize_t len = recvfrom(server_socket, buffer,sizeof(buffer), 0,
			(struct sockaddr *) &client_address, &rcva_len);
		ensure_with_errno(len, "error on datagram from client socket");

		if (len > BUFFER_SIZE) {
			fprintf(stderr, "read from socket message exceeding %d bytes, ignoring\n", BUFFER_SIZE);
			continue;
		}

		fprintf(stderr, "read from socket: %zd bytes: %.*s\n", len, (int) len, buffer);
		auto parsed_msg = client_message::from(buffer, len);
		if (parsed_msg.second == false) {
			fprintf(stderr, "Received malformed message: %s\n", buffer);
		}

		handle_client_message(parsed_msg.first, client_address);
	}

	return 0;
}
