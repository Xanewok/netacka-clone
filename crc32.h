#pragma once

#include <cstdint>

std::uint32_t xcrc32(const unsigned char *buf, int len, std::uint32_t init);