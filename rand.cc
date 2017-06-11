#include <cstdint>
#include <ctime>
#include <limits>
#include "rand.h"

Rand::Rand() : m_value(static_cast<std::uint32_t>(time(nullptr))) {}

// Exact algorithm is specified in the assignment (used for automatic tests)
std::uint32_t Rand::next()
{
	constexpr std::uint32_t MOD = 4294967291;
	static_assert(static_cast<std::uint64_t>(MOD) < std::numeric_limits<std::uint32_t>::max(), "Rand values can't be expressed using std::uint32_t");
	std::uint32_t previous = m_value;
	m_value = (previous * 279470273) % 4294967291;
	return previous;
}

