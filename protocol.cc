#include "protocol.h"

#include <cstdint>
#include <vector>
#include <string>
#include <memory>

event::event(event_type_t type) : event_type(type) {}

event_type_t event::get_event_type() const
{
	return event_type;
}

std::vector<std::uint8_t> event::as_stream() const
{
	throw std::exception();
}

std::vector<std::uint8_t> event::aux_as_stream() const
{
	throw std::exception();
}

/* static */
std::unique_ptr<event> parse_net(const char* buf)
{
	throw std::exception();
}

new_game::new_game() : event(NEW_GAME)
{
}

std::vector<std::uint8_t> new_game::as_stream() const
{
	throw std::exception();
}

std::vector<std::uint8_t> new_game::aux_as_stream() const
{
	throw std::exception();
}

pixel::pixel() : event(PIXEL) {}

std::vector<std::uint8_t> pixel::as_stream() const
{
	throw std::exception();
}

std::vector<std::uint8_t> pixel::aux_as_stream() const
{
	throw std::exception();
}

player_eliminated::player_eliminated() : event(PLAYER_ELIMINATED) {}

std::vector<std::uint8_t> player_eliminated::as_stream() const
{
	throw std::exception();
}

std::vector<std::uint8_t> player_eliminated::aux_as_stream() const
{
	throw std::exception();
}

game_over::game_over() : event(GAME_OVER) {}

std::vector<std::uint8_t> game_over::as_stream() const
{
	throw std::exception();
}

std::vector<std::uint8_t> game_over::aux_as_stream() const
{
	throw std::exception();
}

std::vector<std::uint8_t> client_message::as_stream() const
{

}
std::vector<std::uint8_t> server_message::as_stream() const
{

}

