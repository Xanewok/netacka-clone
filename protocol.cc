#include "protocol.h"
#include "crc32.h"

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <memory>
#include <iterator>
#include <utility>
#include <cassert>
#include <algorithm>

#ifdef _WIN32
#define NOMINMAX
#pragma comment(lib, "Ws2_32.lib")
#include <WinSock2.h> // endianness helpers
#else
#include <arpa/inet.h>
#include <sys/types.h>
#define _BSD_SOURCE
#include <endian.h> // TODO: Verify
#define htonll htobe64
#define ntohll be64toh
#endif

#include "util.h"

namespace
{
	inline std::uint64_t hton(std::uint64_t val) { return htonll(val); }
	inline std::uint32_t hton(std::uint32_t val) { return htonl(val); }
	inline std::uint16_t hton(std::uint16_t val) { return htons(val); }
	inline std::uint8_t hton(std::uint8_t val) { return val; }
	inline std::int8_t hton(std::int8_t val) { return val; }

	inline std::uint64_t ntoh(std::uint64_t val) { return ntohll(val); }
	inline std::uint32_t ntoh(std::uint32_t val) { return ntohl(val); }
	inline std::uint16_t ntoh(std::uint16_t val) { return ntohs(val); }
	inline std::uint8_t ntoh(std::uint8_t val) { return val; }
	inline std::int8_t ntoh(std::int8_t val) { return val; }

	template<typename T>
	bool consume_bytes(const std::uint8_t* stream, size_t len, const std::uint8_t*& pointer, T& destination)
	{
		assert(pointer >= stream && pointer < stream + len);
		// Not enough data in the stream
		if (pointer + sizeof(T) > stream + len)
			return false;

		destination = ntoh(*reinterpret_cast<const T*>(pointer));
		pointer += sizeof(T);
		return true;
	}

	template<typename T>
	auto as_bytes(const T& data)
	{
		return reinterpret_cast<const std::uint8_t(*)[sizeof(data)]>(&data);
	}

	template<typename T>
	void append_bytes(std::vector<std::uint8_t>& stream, T bytes)
	{
		stream.insert(stream.end(), std::begin(*bytes), std::end(*bytes));
	}

	void append_string(std::vector<std::uint8_t>& stream, const char* str)
	{
		stream.insert(stream.end(), str, str + strlen(str));
	}

	std::shared_ptr<event> create_event(event_type_t event_type)
	{
		switch (event_type)
		{
		case NEW_GAME: return std::make_shared<new_game>();
		case PLAYER_ELIMINATED: return std::make_shared<player_eliminated>();
		case PIXEL: return std::make_shared<pixel>();
		case GAME_OVER: return std::make_shared<game_over>();
		default: return nullptr;
		}
	}
}

event::event() : event(UNKNOWN) {}
event::event(event_type_t type) : event_type(type) {}

std::uint32_t event::calculate_len() const
{
	return sizeof(event_type) + sizeof(event_no); // + sizeof(event_data) in subclasses
}

std::uint32_t event::calculate_total_len_with_crc32() const
{
	return calculate_len() + sizeof(crc32);
}

std::vector<std::uint8_t> event::as_stream() const
{
	std::vector<std::uint8_t> stream;

	append_bytes(stream, as_bytes(htonl(calculate_len())));
	append_bytes(stream, as_bytes(event_type));
	append_bytes(stream, as_bytes(htonl(event_no)));

	const auto& aux_data = aux_as_stream();
	stream.insert(stream.end(), aux_data.begin(), aux_data.end());

	auto crc =	xcrc32(stream.data(), stream.size(), 0);
	append_bytes(stream, as_bytes(htonl(crc)));

	stream.shrink_to_fit();
	return stream;
}

std::vector<std::uint8_t> event::aux_as_stream() const
{
	return {};
}

/* static */
std::shared_ptr<event> event::parse(const char* buf, size_t buf_len, bool require_exact_size/* = false*/)
{
	const std::uint8_t* stream = reinterpret_cast<const std::uint8_t*>(buf);
	const std::uint8_t* pointer = stream;

	std::uint32_t len;
	if (!consume_bytes(stream, buf_len, pointer, len))
		return nullptr;

	std::uint8_t event_type;
	if (!consume_bytes(stream, buf_len, pointer, event_type))
		return nullptr;

	std::uint32_t event_no;
	if (!consume_bytes(stream, buf_len, pointer, event_no))
		return nullptr;

	auto event = create_event(static_cast<event_type_t>(event_type));
	// Parsed event type is invalid or couldn't create event
	if (event == nullptr)
		return nullptr;
	
	event->len = len;
	event->event_no;

	pointer = event->parse_event_data(pointer, len - event::HEADER_LEN - sizeof(event::crc32));
	// Could not succesfully parse event_data depending on event type
	if (pointer == nullptr)
		return nullptr;

	if (!consume_bytes(stream, buf_len, pointer, event->crc32))
		return nullptr;

	// Reported message length and parsed len do not match
	if (require_exact_size && len != (pointer - stream) - sizeof(event::crc32) - sizeof(event::len))
		return nullptr;

	// CRC checksum mismatch
	auto crc = xcrc32(stream, (pointer - stream) - sizeof(event::crc32), 0);
	if (crc != event->crc32) {
		fprintf(stderr, "CRC checksum mismatch\n");
		return nullptr;
	}

	// Data was exact, event could be created and crc matches - we're good to go
	return event;
}

const uint8_t* event::parse_event_data(const uint8_t* buf, size_t len)
{
	// Bare event doesn't contain any extra info
	return buf;
}

new_game::new_game() : event(NEW_GAME) {}
new_game::new_game(std::uint32_t width, std::uint32_t height,
	const std::vector<std::string>& names)
: event(NEW_GAME)
, maxx(width)
, maxy(height)
, player_names(names)
{
}

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

const uint8_t* new_game::parse_event_data(const uint8_t* buf, size_t len)
{
	const uint8_t* pointer = buf;
	if (!consume_bytes(buf, len, pointer, this->maxx)) return nullptr;
	if (!consume_bytes(buf, len, pointer, this->maxy)) return nullptr;

	while (pointer < buf + len)
	{
		const char* str = reinterpret_cast<const char*>(pointer);
		int name_len = strlen(str);
		// Invalid name length
		if (name_len <= 0 || name_len > 64)
			return nullptr;
		const std::uint8_t* name_end = pointer + name_len;
		// Name extends message length or isn't 0-separated
		if (name_end >= buf + len || *name_end != '\0')
			return nullptr;

		this->player_names.push_back(std::string(str));
		pointer += name_len + sizeof('\0');
	}

	return pointer;
}

pixel::pixel() : event(PIXEL) {}
pixel::pixel(std::uint8_t player, std::uint32_t px, std::uint32_t py)
: event(PIXEL)
, player_number(player)
, x(px)
, y(py)
{
}

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

const uint8_t* pixel::parse_event_data(const uint8_t* buf, size_t len)
{
	const uint8_t* pointer = buf;
	if (!consume_bytes(buf, len, pointer, this->player_number)) return nullptr;
	if (!consume_bytes(buf, len, pointer, this->x)) return nullptr;
	if (!consume_bytes(buf, len, pointer, this->y)) return nullptr;
	
	return pointer;
}

player_eliminated::player_eliminated() : event(PLAYER_ELIMINATED) {}
player_eliminated::player_eliminated(std::uint8_t player)
: event(PLAYER_ELIMINATED)
, player_number(player)
{
}

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

const uint8_t* player_eliminated::parse_event_data(const uint8_t* buf, size_t len)
{
	const uint8_t* pointer = buf;
	if (!consume_bytes(buf, len, pointer, this->player_number)) return nullptr;

	return pointer;
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
std::pair<server_message, bool> server_message::from(const char* stream, size_t len)
{
	server_message msg;

	const uint8_t* data = reinterpret_cast<const std::uint8_t*>(stream);
	const uint8_t* pointer = data;
	if (!consume_bytes(data, len, pointer, msg.game_id))
		return { msg, false };

	int message_len = sizeof(msg.game_id);
	while (len < MAX_EVENT_PACKET_DATA_SIZE)
	{
		auto event = event::parse((const char*)pointer, len - message_len);
		// One of the events is probably malformed (partial message is acceptable if we have > 0 correct events)
		if (event == nullptr)
			return { msg, msg.events.size() > 0 };

		const auto event_len = event->calculate_total_len_with_crc32();
		if (len + event_len > MAX_EVENT_PACKET_DATA_SIZE)
			break;

		msg.events.push_back(event);
		message_len += event_len;
	}

	return { msg, true };
}

/* static */
std::pair<client_message, bool> client_message::from(const char* stream, size_t len)
{
	constexpr int MIN_MESSAGE_LEN = sizeof(client_message::session_id)
		+ sizeof(client_message::turn_direction)
		+ sizeof(client_message::next_expected_event) + sizeof('\0');

	if (len <= MIN_MESSAGE_LEN)
		return std::make_pair(client_message(), false);

	client_message msg;

	const uint8_t* data = reinterpret_cast<const std::uint8_t*>(stream);
	const uint8_t* pointer = data;
	consume_bytes(data, len, pointer, msg.session_id);
	consume_bytes(data, len, pointer, msg.turn_direction);
	consume_bytes(data, len, pointer, msg.next_expected_event);

	// Only { -1, 0, 1 } are valid turn_directions
	if (std::abs(msg.turn_direction) > 1)
		return std::make_pair(client_message(), false);

	const size_t name_len = std::min(len - MIN_MESSAGE_LEN + 1, sizeof(msg.player_name));
	for (size_t i = 0; i < name_len; ++i)
	{
		msg.player_name[i] = pointer[i];
		if (pointer[i] == '\0')
			break;
	}

	if (!util::is_valid_player_name(msg.player_name, name_len))
		return std::make_pair(client_message(), false);

	return std::make_pair(msg, true);
}
