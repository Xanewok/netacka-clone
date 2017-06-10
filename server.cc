#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <stdexcept>
#include <limits>
#include <ctime>
#include <set>
#include <memory>
#include <chrono>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>

#include "protocol.h"
#include "util.h"
#include "rand.h"

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

struct player_coords {
	double x;
	double y;
	double rotation; // in degrees, clockwise, 0* = right
};

// Players are identified by (socket, session_id) pair
struct player_connection {
	constexpr static std::uint64_t CONNECTION_TIMEOUT = 2000; // [ms]

	int socket;

	client_message last_message;
	duration<std::uint64_t> last_response_time;
	//std::uint64_t last_response_time;

	bool ready_to_play = false;
	bool is_waiting = false; // set to true if spectating, but wants to play next

	bool is_inactive() const
	{
		auto current = duration_cast<milliseconds>(
			system_clock::now().time_since_epoch());

		return (current - last_response_time).count() > CONNECTION_TIMEOUT;
	}
};

struct name_compare
{
	bool operator() (const player_connection& lhs, const player_connection& rhs) const
	{
		return lhs.last_message.player_name < rhs.last_message.player_name;
	}
};

bool in_map(double x, double y)
{
	std::uint64_t x_rounded = x;
	std::uint64_t y_rounded = y;

	return x_rounded >= 0 && x_rounded < configuration.width
		&& y_rounded >= 0 && y_rounded < configuration.height;
}

struct map {
	std::vector<pixel*> pixels;
};

static struct {
	std::uint32_t game_id;
	bool in_progress = false;
	std::set<player_connection, name_compare> players;
	struct map map;
	std::vector<std::unique_ptr<event>> events;
} game_state;

void handle_client_message(client_message& msg)
{

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
	// Initialize deterministic random generator
	rand_gen = Rand(configuration.seed_provided ? configuration.rand_seed : time(nullptr));

	int ret;
	int sock = socket(AF_INET6, SOCK_DGRAM, 0);
	ensure_with_errno(sock, "socket");

	// Disable IPv6-only option for sockets
	int no = 0;
	setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (void *)&no, sizeof(no));

	struct sockaddr_in6 server_address;
	server_address.sin6_family = AF_INET6;
	server_address.sin6_addr = in6addr_any;
	printf("%d\n", configuration.port_num);
	server_address.sin6_port = htons(configuration.port_num);

	ret = bind(sock, (struct sockaddr *) &server_address, (socklen_t) sizeof(server_address));
	ensure_with_errno(ret, "bind");

	// Receive data from clients
	constexpr size_t BUFFER_SIZE = 100000;
	char buffer[BUFFER_SIZE];
	struct sockaddr_in6 client_address;
	socklen_t snda_len = (socklen_t) sizeof(client_address);
	while (true)
	{
		socklen_t rcva_len = (socklen_t) sizeof(client_address);
		ssize_t len = recvfrom(sock, buffer,sizeof(buffer), 0,
			(struct sockaddr *) &client_address, &rcva_len);
		ensure_with_errno(len, "error on datagram from client socket");

		printf("read from socket: %zd bytes: %.*s\n", len, (int) len, buffer);
	}

	return 0;
}
