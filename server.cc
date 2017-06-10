#include <cstring>
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

static Rand rand;

static struct {
	int width = 800;
	int height = 600;
	int port_num = 12345;
	int rounds_per_sec = 50;
	int turning_speed = 6;
	int rand_seed;
	bool seed_provided = false;
} configuration;

struct map {
	std::vector<pixel*> pixels;
};

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

static struct {
	std::uint32_t game_id;
	std::set<player_connection, name_compare> players;

} game_state;

void handle_client_message(client_message& msg)
{

}

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

namespace {
	std::int64_t parse_param(const char* s,
		std::int64_t min = std::numeric_limits<std::int64_t>::min(),
		std::int64_t max = std::numeric_limits<std::int64_t>::max())
	{
		std::int64_t param;
		try
		{
			param = util::parse_bounded(s, min, max);
		}
		catch (const std::exception& e)
		{
			printf("Bad argument: %s\n%s", s, usage_msg);
			std::exit(1);
		}
		return param;
	}
} // namespace

// TODO: Catch exception, display message and show usage

int main(int argc, char* argv[])
{
	// Parse optional arguments
	for (int i = 1; i < argc; i += 2)
	{
		const char* arg = argv[i];
		printf("strlen(%s) = %d\n", arg, strlen(arg));
		printf("arg[0] = %c\n", arg[0]);
		printf("arg[1] = %c\n", arg[1]);
		if (arg[0] != '-' || strlen(arg) != 2 || i + 1 >= argc)
		{
			printf("Bad argument: %s%s\n%s",
				arg, (i + 1 >= argc ? " (missing parameter)" : ""), usage_msg);
			std::exit(1);
		}

		std::int64_t param = 0;
		if (arg[1] != 'p')
			std::uint64_t param = parse_param(argv[i + 1]);
		printf("Parsed param: %lld\n", param);
		switch (arg[1])
		{
		case 'W': configuration.width = param; break;
		case 'H': configuration.height = param; break;
		case 'p': configuration.port_num = util::parse_bounded(argv[i + 1], 0, 65535); break;
		case 's': configuration.rounds_per_sec = param; break;
		case 't': configuration.turning_speed = param; break;
		case 'r': configuration.rand_seed = param; configuration.seed_provided = true; break;
		default:
		{
			printf("Bad argument: %s\n%s", arg, usage_msg);
			std::exit(1);
		}
		}
	}
	// Initialize deterministic random generator
	rand = Rand(configuration.seed_provided ? configuration.rand_seed : time(nullptr));
}

