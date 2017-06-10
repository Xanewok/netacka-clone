#include "map.h"

map::map(std::uint32_t _width, std::uint32_t _height)
: width(_width)
, height(_height)
{
}

bool map::is_inside(const position_t& pos) const
{
    return pos.first < width && pos.second < height;
}

bool map::is_inside(double x, double y) const
{
    return is_inside(make_pos(x, y));
}

bool map::is_occupied(const position_t& pos) const
{
    return pixels.find(pos) != pixels.end();
}

bool map::is_occupied(double x, double y) const
{
    return is_occupied(make_pos(x, y));
}
/* static */
map::position_t map::make_pos(double x, double y)
{
    return position_t(static_cast<std::uint64_t>(x),
        static_cast<std::uint64_t>(y));
}
