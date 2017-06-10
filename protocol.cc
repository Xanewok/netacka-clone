#include "protocol.h"

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <memory>
#include <iterator>
#include <utility>

#ifdef _WIN32
#pragma comment(lib, "Ws2_32.lib")
#include <WinSock2.h> // endianness helpers
#else
#include <arpa/inet.h>
#include <sys/types.h>
#define _BSD_SOURCE
#include <endian.h> // TODO: Verify
#define htonll htobe64
#endif

namespace
{
	template<typename T>
	auto as_bytes(const T& data)
	{
		return reinterpret_cast<const std::uint8_t(*)[sizeof(data)]>(&data);
	}

	template<typename T>
	void append_bytes(std::vector<std::uint8_t>& stream, T bytes)
	{
		stream.insert(stream.end(), std::rbegin(*bytes), std::rend(*bytes));
	}

	void append_string(std::vector<std::uint8_t>& stream, const char* str)
	{
		stream.insert(stream.end(), str, str + strlen(str));
	}

	std::uint32_t calculate_crc(const std::vector<std::uint8_t> stream)
	{
		// TODO
		return 0;
	}
}

event::event() : event(UNKNOWN) {}
event::event(event_type_t type) : event_type(type) {}

std::uint32_t event::calculate_len() const
{
	return sizeof(event_type) + sizeof(event_no); // + sizeof(event_data) in subclasses
}

std::vector<std::uint8_t> event::as_stream() const
{
	std::vector<std::uint8_t> stream;

	append_bytes(stream, as_bytes(htonl(calculate_len())));
	append_bytes(stream, as_bytes(event_type));
	append_bytes(stream, as_bytes(htonl(event_no)));

	const auto& aux_data = aux_as_stream();
	stream.insert(stream.end(), aux_data.begin(), aux_data.end());

	auto crc = calculate_crc(stream);
	append_bytes(stream, as_bytes(htonl(crc)));

	stream.shrink_to_fit();
	return stream;
}

std::vector<std::uint8_t> event::aux_as_stream() const
{
	return {};
}

new_game::new_game() : event(NEW_GAME) {}

std::uint32_t new_game::calculate_len() const
{
	auto event_data_len = sizeof(maxx) + sizeof(maxy);

	for (const auto& name : player_names)
		event_data_len += name.length() + sizeof('\0');

	return event::calculate_len() + event_data_len;
}

std::vector<std::uint8_t> new_game::aux_as_stream() const
{
	std::vector<std::uint8_t> stream;

	append_bytes(stream, as_bytes(htonl(maxx)));
	append_bytes(stream, as_bytes(htonl(maxy)));
	for (const auto& str : player_names)
	{
		append_string(stream, str.c_str());
		stream.push_back('\0');
	}

	stream.shrink_to_fit();
	return stream;
}

pixel::pixel() : event(PIXEL) {}

std::uint32_t pixel::calculate_len() const
{
	auto event_data_len = sizeof(player_number) + sizeof(x) + sizeof(y);
	return event::calculate_len() + event_data_len;
}

std::vector<std::uint8_t> pixel::aux_as_stream() const
{
	std::vector<std::uint8_t> stream;

	append_bytes(stream, as_bytes(player_number));
	append_bytes(stream, as_bytes(htonl(x)));
	append_bytes(stream, as_bytes(htonl(y)));

	stream.shrink_to_fit();
	return stream;
}

player_eliminated::player_eliminated() : event(PLAYER_ELIMINATED) {}

std::uint32_t player_eliminated::calculate_len() const
{
	auto event_data_len = sizeof(player_number);
	return event::calculate_len() + event_data_len;
}

std::vector<std::uint8_t> player_eliminated::aux_as_stream() const
{
	std::vector<std::uint8_t> stream;
	stream.push_back(player_number);
	return stream;
}

game_over::game_over() : event(GAME_OVER) {}

std::vector<std::uint8_t> game_over::aux_as_stream() const
{
	return {};
}

std::vector<std::uint8_t> client_message::as_stream() const
{
	std::vector<std::uint8_t> stream;

	append_bytes(stream, as_bytes(htonll(session_id)));
	append_bytes(stream, as_bytes(turn_direction));
	append_bytes(stream, as_bytes(htonl(next_expected_event)));
	append_string(stream, player_name);

	stream.shrink_to_fit();
	return stream;
}

std::vector<std::uint8_t> server_message::as_stream() const
{
	std::vector<std::uint8_t> stream;

	append_bytes(stream, as_bytes(htonl(game_id)));
	for (const auto& event : events)
	{
		auto event_stream = event->as_stream();
		stream.insert(stream.end(), event_stream.begin(), event_stream.end());
	}

	stream.shrink_to_fit();
	return stream;
}

/* static */
std::pair<client_message, bool> client_message::from(const char* stream, size_t len)
{
	if (len <= sizeof(client_message::session_id) + sizeof(client_message::turn_direction)
		+ sizeof(client_message::next_expected_event) + sizeof('\0'))
		return std::make_pair(client_message(), false);

	client_message msg;

	msg.session_id = be64toh(*reinterpret_cast<const std::uint64_t*>(stream));
	stream += sizeof(msg.session_id);
	msg.turn_direction = *stream;
	stream += sizeof(msg.turn_direction);
	msg.next_expected_event = be32toh(*reinterpret_cast<const std::uint32_t*>(stream));
	stream += sizeof(msg.next_expected_event);

	for (size_t i = 0; i < sizeof(msg.player_name); ++i)
	{
		msg.player_name[i] = stream[i];
		if (stream[i] == '\0')
			break;
	}

	return std::make_pair(msg, true);
}
