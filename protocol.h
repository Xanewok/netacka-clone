#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <utility>

constexpr int MAX_EVENT_PACKET_DATA_SIZE = 512;

struct event;

struct client_message
{
	std::uint64_t session_id;
	std::int8_t turn_direction;
	std::uint32_t next_expected_event;
	char player_name[64] = { 0 };
	std::vector<std::uint8_t> as_stream() const;

	static std::pair<client_message, bool> from(const char* stream, size_t len);
};

struct server_message
{
	std::uint32_t game_id;
	std::vector<event*> events;

	constexpr static int HEADER_LEN = sizeof(server_message::game_id);
	constexpr static int MAX_EVENTS_LEN = MAX_EVENT_PACKET_DATA_SIZE
		- sizeof(server_message::game_id);

	std::vector<std::uint8_t> as_stream() const;
};


enum event_type_t : std::uint8_t
{
	NEW_GAME = 0,
	PIXEL = 1,
	PLAYER_ELIMINATED = 2,
	GAME_OVER = 3,
	UNKNOWN = 0xFF,
};

struct event
{
	std::uint32_t len; // sizeof(event_* members data)
	const event_type_t event_type;
	std::uint32_t event_no; // consecutive values for each game session,
	std::uint32_t crc32; // crc checksum from len to, including, event_type

	constexpr static int HEADER_LEN = sizeof(event::len) +
		sizeof(event::event_type) + sizeof(event::event_no);

	event();
    event(event_type_t type);
	virtual ~event() = default;

	virtual std::uint32_t calculate_len() const;
	std::uint32_t calculate_total_len_with_crc32() const;
	virtual std::vector<std::uint8_t> as_stream() const;
	// Overridable for extending subclasses
	virtual std::vector<std::uint8_t> aux_as_stream() const;

	static std::shared_ptr<event> parse(const char* buf, size_t len);
	virtual const uint8_t* parse_event_data(const uint8_t* buf, size_t len);
};

struct new_game : public event
{
    std::uint32_t maxx;
    std::uint32_t maxy;
    std::vector<std::string> player_names; // each name is up to 64 chars and
    // ends with '\0'
	new_game();
	new_game(std::uint32_t width, std::uint32_t height,
		const std::vector<std::string>& names);
	virtual ~new_game() = default;
	virtual std::uint32_t calculate_len() const override;
	virtual std::vector<std::uint8_t> aux_as_stream() const override;
	virtual const uint8_t* parse_event_data(const uint8_t* buf, size_t len) override;
};

constexpr int MAX_PLAYER_NAMES_LEN = MAX_EVENT_PACKET_DATA_SIZE -
(sizeof(new_game) - sizeof(new_game::player_names))
- server_message::HEADER_LEN; // account for server_message header which will contain new_game event

struct pixel : public event
{
    std::uint8_t player_number;
    std::uint32_t x;
    std::uint32_t y;
	pixel();
	pixel(std::uint8_t player, std::uint32_t px, std::uint32_t py);
	virtual ~pixel() = default;
	virtual std::uint32_t calculate_len() const override;
	virtual std::vector<std::uint8_t> aux_as_stream() const override;
	virtual const uint8_t* parse_event_data(const uint8_t* buf, size_t len) override;
};

struct player_eliminated : public event
{
    std::uint8_t player_number;
	player_eliminated();
	player_eliminated(std::uint8_t player);
	virtual ~player_eliminated() = default;
	virtual std::uint32_t calculate_len() const override;
	virtual std::vector<std::uint8_t> aux_as_stream() const override;
	virtual const uint8_t* parse_event_data(const uint8_t* buf, size_t len) override;
};

struct game_over : public event
{
	game_over();
	virtual ~game_over() = default;
	virtual std::vector<std::uint8_t> aux_as_stream() const override;
};
