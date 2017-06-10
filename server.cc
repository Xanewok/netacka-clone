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

#include "protocol.h"
#include "util.h"
#include "rand.h"

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
	int port_num = 12345;
	int rounds_per_sec = 50;
	int turning_speed = 6;
	std::int64_t rand_seed;
	bool seed_provided = false;
} configuration;

struct player_coords {
	double x;
	double y;
	double rotation; // in degrees, clockwise, 0* = right
};

// Logically identified by (socket, session_id) pair
struct player_connection {
	constexpr static std::uint64_t CONNECTION_TIMEOUT = 2000; // [ms]

	int socket;
	std::uint64_t session_id;
	std::string player_name;
	bool ready_to_play = false;
	bool is_waiting = false; // set to true if spectating, but wants to play next
	std::uint64_t last_response_time;
};

struct name_compare
{
	bool operator() (const player_connection& lhs, const player_connection& rhs) const
	{
		return lhs.player_name < rhs.player_name;
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
	std::set<player_connection, name_compare> players;
	struct map map;
} game_state;

void handle_client_message(client_message& msg)
{

}

namespace {

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
			configuration.rounds_per_sec = util::parse_bounded(argv[i + 1], 0, std::numeric_limits<int>::max());
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
}

