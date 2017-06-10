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
#define _USE_MATH_DEFINES
#include <cmath>
#include <algorithm>
#include <array>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>

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
	int rounds_per_sec = 50;
	int turning_speed = 6;
	std::int64_t rand_seed;
	bool seed_provided = false;
} configuration;

static std::uint64_t get_round_time_ms()
{
	static std::uint64_t round_time = 1000 / (float)configuration.rounds_per_sec;
	return round_time;
}

struct player_transform {
	double x;
	double y;
	double rotation; // in degrees, clockwise, 0* = right
};

// Players are identified by (socket, session_id) pair
struct player_connection {
	constexpr static std::uint64_t CONNECTION_TIMEOUT = 2000; // [ms]

	sockaddr_in6 socket;
	duration<std::uint64_t> last_response_time;

	client_message last_message;

	player_transform transform;
	bool ready_to_play = false; // pressed arrow when waiting for NEW_GAME
	bool waiting = false; // set to true if spectating, but wants to play next
	bool eliminated = false;

	bool is_inactive() const
	{
		return (current_time_ms() - last_response_time).count() > CONNECTION_TIMEOUT;
	}
};

// Helper struct that allows to compare sockaddr_in6 structures and also to
// convert them to comparable tuples (pair of (addr, port) identifies the struct)
struct in6_compare
{
	template<size_t off>
	std::uint64_t addr_part(const sockaddr_in6& addr)
	{
		return *reinterpret_cast<const std::uint64_t*>(addr.sin6_addr.s6_addr[off]);
	}

	auto as_tuple(const sockaddr_in6& sock)
	{
		return std::make_tuple(addr_part<0>(sock), addr_part<4>(sock), sock.sin6_port);
	}

	bool operator() (const sockaddr_in6& lhs, const sockaddr_in6& rhs)
	{
		return as_tuple(lhs) < as_tuple(rhs);
	}
};

using player_collection_t = std::map<sockaddr_in6, player_connection, in6_compare>;
static struct {
	std::uint32_t game_id = 0;
	bool in_progress = false;
	struct map map;
	std::vector<std::unique_ptr<event>> events;

	player_collection_t active_players;
	player_collection_t spectators_or_waiting;

	// Cached players with ordering for given game (reinitialized for every game)
	std::vector<player_connection*> playing_players;

	// TODO: USE ONLY ONE PLAYER CONNECTION COLLECTION (MAP) WITH KEY SOCKET -> STRUCT WITH PLAYER INFO
	// ALSO HAVE SORTED VECTORS FOR ACTIVE PLAYERS DURING THAT GAME
	// WE WILL USE THAT TO GET PLAYER NUM (OR PREFERABLY STORE THAT WITH SET-PLAYER NAME? ON GAME STARTUP

	// OR EVEN STH LIKE STRUCT { X, Y, ROT, NAME, NUMBER , POINTER TO PLAYER_CONNECTION } BUT UNSURE WHERE
	// TO PUT READINESS/SPECTATOR STATE (SPECTATOR MUST BE IN CONNECTION PART AT LEAST, BUT GAME READINESS
	// IS BASICALLY CONNECTION/GAME STARTUP RELATED)

	// THIS WAY WE CAN USE ONLY ONE PLAYER CONNECTION FOR FINDING/ITERATORS

	// MOVING PLAYING->SPECTATING WOULD RETAIN PLAYING PLAYER STRUCT BUT CHANGE SPECTATING FLAG IN CONNECTION
	// MOVING SPECTATING->PLAYING WOULD ONLY HAPPEN ON GAME START, CHANGE SPECTATING FLAG TO FALSE AND INITIALIZE
	// PLAYING PLAYER STRUCT

	std::chrono::milliseconds next_game_tick = std::chrono::milliseconds(0);
} game_state;

// TODO: FACTOR OUT SERVER STATE AND CONTAINING RANDOM GENERATOR, CONFIGURATION, OPEN SOCKET AND PLAYER CONNECTIONS
// + PLAYER GAME STATE
// STRUCTS AND DEFS COULD BE IN SERVER.H AND THIS FILE WOULD ONLY BASICALLY PARSE ARGS, OPEN SOCKET

std::pair<player_collection_t::iterator, bool> find_player(const struct sockaddr_in6& sock)
{
	const std::array<player_collection_t*, 2> players = {
		&game_state.active_players, &game_state.spectators_or_waiting
	};

	for (const auto& collection : players)
	{
		auto it = collection->find(sock);
		if (it != collection->end())
			return std::make_pair(it, true);
	}

	return std::make_pair(player_collection_t::iterator(), false);
}

int get_living_players_num()
{
	int living_players = 0;
	for (const auto& player : game_state.playing_players)
		if (player->eliminated == false)
			living_players++;

	return living_players;
}

void cleanup_game()
{
	game_state.in_progress = false;
	game_state.game_id++;
	game_state.map = map(configuration.width, configuration.height);
	game_state.events.clear(); // TODO: Verify
}

void generate_event(std::unique_ptr<event> event);

void start_game()
{
	game_state.in_progress = true;

	game_state.playing_players.clear();
	for (auto& kv : game_state.active_players)
	{
		kv.second.eliminated = false;
		kv.second.waiting = false;
		game_state.playing_players.push_back(&kv.second);
	}

	std::sort(game_state.playing_players.begin(), game_state.playing_players.end(),
		[](const player_connection* lhs, const player_connection* rhs)
		{
			return lhs->last_message.player_name < rhs->last_message.player_name;
		}
	);

	std::vector<std::string> names;
	for (const auto& player : game_state.playing_players)
		names.push_back(player->last_message.player_name); // TODO: MAKE SURE player_name is not null (catch player name change)

	auto event = std::unique_ptr<new_game>(new new_game(configuration.width, configuration.height, names));
	generate_event(std::move(event));
}

static int server_socket;

void broadcast_event(struct event* event)
{
	auto buffer = event->as_stream();

	const std::array<const player_collection_t*, 2> players = {
		&game_state.active_players, &game_state.spectators_or_waiting
	};

	for (const auto& collection_pointer : players)
	{
		for (const auto& kv : (player_collection_t&) *collection_pointer)
		{
			const sockaddr_in6& client_address = kv.first;

			ssize_t snd_len = sendto(server_socket, buffer.data(), buffer.size(), 0,
				(sockaddr*) &client_address, sizeof(client_address));

			if (snd_len != static_cast<ssize_t>(buffer.size()))
				fprintf(stderr, "Error sending event: %s\n", buffer.data());
		}
	}
}

void generate_event(std::unique_ptr<event> event)
{
	// First broadcast the event
	event->event_no = game_state.events.size() + 1;

	auto raw_event = event.get();
	broadcast_event(raw_event);

	game_state.events.push_back(std::move(event));

	// Then act accordingly
	switch (event->event_type)
	{
	case NEW_GAME:
	{
		start_game();
		break;
	}
	case PIXEL:
	{
		pixel* pixel_event = static_cast<pixel*>(raw_event);

		game_state.map.pixels.insert(map::make_pos(pixel_event->x, pixel_event->y));
		break;
	}
	case PLAYER_ELIMINATED:
	{
		std::int8_t player_num = static_cast<player_eliminated*>(raw_event)->player_number;
		game_state.playing_players[player_num]->eliminated = true;

		if (get_living_players_num() == 1) {
			generate_event(std::make_unique<game_over>());
		}
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

void handle_client_message(const client_message& msg, const struct sockaddr_in6& sock)
{
	auto option_it = find_player(sock);
	const bool player_found = option_it.second;

	if (player_found)
	{
		const auto& player = option_it.first->second;

		const auto cur_session_id = player.last_message.session_id;
		// Ignore incoming messages for existing client with lower session_id
		if (msg.session_id < cur_session_id)
			return;
		// If incoming session_id is bigger for existing client then disconnect
		// existing one and replace with the new one
		else if (msg.session_id > cur_session_id)
		{

			game_state.spectators_or_waiting[sock] = player;
			game_state.active_players
			// TODO: Implement me

		}
	}

	// Handle player spectator<>active movement
	if (game_state.in_progress == false) {

	}
	player_connection player;
	player.socket = sock;
	// Empty player name means it's only a spectator
	if (strlen(msg.player_name) == 0)
	{

	}
	// TODO:
}

void do_game_tick()
{
	for (size_t i = 0; i < game_state.playing_players.size(); ++i)
	{
		auto& player = *game_state.playing_players[i];
		std::uint8_t player_num = static_cast<std::uint8_t>(i);

		player.transform.rotation += player.last_message.turn_direction * configuration.turning_speed;
		player.transform.rotation = fmod(player.transform.rotation, 360);

		auto old_pos = map::make_pos(player.transform.x, player.transform.y);
		// Rotation are degrees going clock-wise, so negate deg for (cos deg, sin deg) unit vector
		// Move by a unit in given direction
		player.transform.x += cos(-player.transform.rotation * M_PI / 180);
		player.transform.y += sin(-player.transform.rotation * M_PI / 180);

		auto new_pos = map::make_pos(player.transform.x, player.transform.y);

		if (old_pos == new_pos)
			continue;

		if (!game_state.map.is_inside(new_pos) || game_state.map.is_occupied(new_pos))
			generate_event(std::make_unique<player_eliminated>(player_num));
		else
			generate_event(std::make_unique<pixel>(player_num, new_pos.first, new_pos.second));
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
			configuration.width = util::parse_bounded(argv[i + 1], 0, std::numeric_limits<std::uint32_t>::max());
			break;
		}
		case 'H':
		{
			configuration.height = util::parse_bounded(argv[i + 1], 0, std::numeric_limits<std::uint32_t>::max());
			break;
		}
		case 'p': configuration.port_num = util::parse_bounded(argv[i + 1], 0, 65535); break;
		case 's':
		{
			configuration.rounds_per_sec = util::parse_bounded(argv[i + 1], 1, std::numeric_limits<int>::max());
			break;
		}
		case 't':
		{
			configuration.turning_speed = util::parse_bounded(argv[i + 1], 0, std::numeric_limits<int>::max());
			break;
		}
		case 'r':
		{
			configuration.rand_seed = util::parse_bounded(argv[i + 1],
				std::numeric_limits<std::int64_t>::min(),
				std::numeric_limits<std::int64_t>::max());
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
	rand_gen = Rand(configuration.seed_provided ? configuration.rand_seed : time(nullptr));

	// Initialized IPv6 UPD socket for client-connection
	server_socket = socket(AF_INET6, SOCK_DGRAM, 0);
	ensure_with_errno(server_socket, "socket");

	// Disable IPv6-only option for sockets
	int no = 0;
	setsockopt(server_socket, IPPROTO_IPV6, IPV6_V6ONLY, (void *)&no, sizeof(no));

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
