#pragma once
#include <utility>
#include <cstdint>
#include <set>

struct map {
	using position_t = std::pair<std::uint32_t, std::uint32_t>;

	std::uint32_t width = 800;
	std::uint32_t height = 600;
	std::set<position_t> pixels;

	map() = default;
	map(std::uint32_t _width, std::uint32_t _height);
	bool is_inside(const position_t& pos) const;
	bool is_inside(double x, double y) const;

	bool is_occupied(const position_t& pos) const;
	bool is_occupied(double x, double y) const;

    static position_t make_pos(double x, double y);
};
