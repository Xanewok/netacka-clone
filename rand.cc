#include <cstdint>
#include <ctime>
#include "rand.h"

Rand::Rand() : m_value(time(nullptr)) {}

// Exact algorithm is specified in the assignment (used for automatic tests)
std::int64_t Rand::next()
{
	std::int64_t previous = m_value;
	m_value = (previous * 279470273) % 4294967291;
	return previous;
}

