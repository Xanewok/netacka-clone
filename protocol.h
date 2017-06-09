#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <memory>

enum event_type_t : std::uint8_t
{
	NEW_GAME = 0,
	PIXEL = 1,
	PLAYER_ELIMINATED = 2,
	GAME_OVER = 3,
	UNKNOWN = static_cast<std::uint8_t>(~0u),
};

struct event
{
	std::uint32_t len; // sizeof(event_* members data)
	const event_type_t event_type;
	std::uint32_t event_no; // consecutive values for each game session,
	std::uint32_t crc32; // crc checksum from len to, including, event_type
    event(event_type_t type);
	virtual ~event() = default;

	event_type_t get_event_type() const;
	virtual std::vector<std::uint8_t> as_stream() const;
	// Overridable for extending subclasses
	virtual std::vector<std::uint8_t> aux_as_stream() const;

	static std::unique_ptr<event> parse_net(const char* buf);
};

struct new_game : public event
{
    std::uint32_t maxx;
    std::uint32_t maxy;
    std::vector<std::string> player_names; // each name is up to 64 chars and
    // ends with '\0'
	new_game();
	virtual ~new_game() = default;
	virtual std::vector<std::uint8_t> as_stream() const override;
	virtual std::vector<std::uint8_t> aux_as_stream() const override;
};

struct pixel : public event
{
    std::uint8_t player_number;
    std::uint32_t x;
    std::uint32_t y;
	pixel();
	virtual ~pixel() = default;
	virtual std::vector<std::uint8_t> as_stream() const override;
	virtual std::vector<std::uint8_t> aux_as_stream() const override;
};

struct player_eliminated : public event
{
    std::uint8_t player_number;
	player_eliminated();
	virtual ~player_eliminated() = default;
	virtual std::vector<std::uint8_t> as_stream() const override;
	virtual std::vector<std::uint8_t> aux_as_stream() const override;
};

struct game_over : public event
{
	game_over();
	virtual ~game_over() = default;
	virtual std::vector<std::uint8_t> as_stream() const override;
	virtual std::vector<std::uint8_t> aux_as_stream() const override;
};

struct client_message
{
	std::uint64_t session_id;
	std::int8_t turn_direction;
	std::uint32_t next_expected_event;
	char player_name[64] = { 0 };
	std::vector<std::uint8_t> as_stream() const;
};

struct server_message
{
	std::uint32_t game_id;
	std::vector<event*> events;
	std::vector<std::uint8_t> as_stream() const;
};
