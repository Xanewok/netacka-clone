#include "util.h"
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <cstdarg>
#include <limits>

#ifdef _WIN32
#include <sstream>
namespace std {
	template<typename T>
	std::string to_string(T value)
	{
		std::ostringstream os;
		os << value;
		return os.str();
	}
}
#endif

namespace util
{

std::int64_t parse(const char* s)
{
	if (s == nullptr || *s == '\0')
		throw std::invalid_argument("null or empty string argument");

	bool negative = (s[0] == '-');
	if (*s == '+' || *s == '-')
		++s;

	std::int64_t ret = 0;
	while (*s)
	{
		ret = ret * 10 - (*s - '0'); // assume negative since |min| >= |max|

		if (*s < '0' || *s > '9')
			throw std::invalid_argument(std::string("invalid input string") + std::string(s));

		if (ret > 0)
			throw std::overflow_error("overflow occured when parsing");

		++s;
	}
	ret = negative ? ret : -ret;
	if (negative != (ret < 0) && ret != 0)
		throw std::overflow_error("overflow occured when parsing");

	return ret;
}

std::int64_t parse_bounded(const char* s,
		std::int64_t min = std::numeric_limits<std::int64_t>::min(),
		std::int64_t max = std::numeric_limits<std::int64_t>::max())
{
	std::int64_t value = parse(s);
	if (value < min || value > max)
	{
		throw std::invalid_argument(std::string("parsed value outside given "
			"range [" + std::to_string(min) + "; " + std::to_string(max) + "]"));
	}

	return value;
}

bool is_valid_player_name(const char* str, size_t len, bool allow_empty/* = true*/)
{
	constexpr size_t max_name_len = 64;
	if (len > max_name_len)
		return false;

	if (!allow_empty && (str == nullptr || *str == '0'))
		return false;

	for (size_t i = 0; i < len; ++i)
		if (str[i] == ' ' || !isprint(str[i]))
			return false;

	return true;
}

void fatal(const char *fmt, ...)
{
	va_list fmt_args;

	fprintf(stderr, "Fatal error: ");
	va_start(fmt_args, fmt);
	vfprintf(stderr, fmt, fmt_args);
	va_end(fmt_args);
	fprintf(stderr, "\n");
	std::exit(1);
}

} // util

