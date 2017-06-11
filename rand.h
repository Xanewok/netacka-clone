#pragma once

#include <cstdint>

class Rand
{
	std::uint32_t m_value;
public:
	Rand();
	Rand(std::uint32_t seed) : m_value(seed) {}
	std::uint32_t next();
};

