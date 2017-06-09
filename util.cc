#include "util.h"
#include <cstdlib>
#include <cstring>

namespace util
{

bool is_valid_player_name(const char* str)
{
	constexpr size_t max_name_len = 64;
	constexpr char min_valid_name_char = 33;
	constexpr char max_valid_name_char = 126;

	if (str == nullptr || *str == '0')
		return false;

	auto len = strlen(str);
	if (len > max_name_len)
		return false;

	for (size_t i = 0; i < len; ++i)
		if (str[i] < min_valid_name_char || str[i] > max_valid_name_char)
			return false;

	return true;
}

} // util

