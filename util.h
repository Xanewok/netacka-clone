#pragma once

#include <cstdlib>
#include <cstdint>

namespace util
{
	std::int64_t parse(const char* str);
	std::int64_t parse_bounded(const char* str, std::int64_t min, std::int64_t max);
	bool is_valid_player_name(const char* str, size_t len, bool allow_empty = true);

	// Prints an error message and exists with exit code 1.
	void fatal(const char *fmt, ...);
} // util

