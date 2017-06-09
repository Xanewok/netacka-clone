#include <cstdint>

class Rand
{
	std::int64_t m_value;
public:
	Rand();
	Rand(std::int64_t seed) : m_value(seed) {}
	std::int64_t next();
};

