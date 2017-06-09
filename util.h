#pragma once

#include <cstdint>

namespace util
{
	std::int64_t parse(const char* str);
	std::int64_t parse_bounded(const char* str, std::int64_t min, std::int64_t max);
	bool is_valid_player_name(const char* str);
} // util

