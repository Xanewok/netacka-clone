#include <cstdint>
#include <ctime>
#include <limits>
#include "rand.h"

constexpr std::uint32_t MOD = 4294967291;

Rand::Rand() : m_value(static_cast<std::uint32_t>(time(nullptr))) {}

// Exact algorithm is specified in the assignment (used for automatic tests)
std::uint32_t Rand::next()
{
	std::uint64_t previous = m_value;

	static_assert(static_cast<std::uint64_t>(MOD) < std::numeric_limits<std::uint32_t>::max(), "Rand values can't be expressed using std::uint32_t");
	m_value = static_cast<uint32_t>((previous * 279470273) % MOD);
	return static_cast<uint32_t>(previous);
}

