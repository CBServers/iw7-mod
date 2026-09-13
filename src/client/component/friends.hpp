#pragma once

#include <string>
#include <vector>

namespace friends
{
	// One CB-Launcher friend. steam_id_bits is a synthetic, session-sticky id derived from the discord id
	struct friend_record
	{
		unsigned long long steam_id_bits{0};
		std::string discord_id;
		std::string name;
		int persona_state{0}; // steam persona: 0 offline, 1 online, 2 busy, 3 away
		bool in_game{false};  // running iw7-mod (launcher saw a party with our game id)
		bool joinable{false};
		bool same_match{false}; // in the match we're already in; never joinable
		std::string mode;     // "mp" / "zm" / "sp"
		std::string map;      // raw map name, e.g. "mp_terminal_cls"
		std::string gametype; // raw gametype name, e.g. "war"
	};

	// Parsed entry of the launcher's "friends" IPC snapshot, before steam id assignment.
	struct snapshot_entry
	{
		std::string discord_id;
		std::string name;
		std::string status; // "online" / "idle" / "dnd" / "offline"
		bool in_launcher{false};
		bool has_game{false};
		std::string game_id;
		std::string mode;
		std::string map;
		std::string gametype;
		bool joinable{false};
		bool same_match{false};
	};

	// Thread-safe; swaps the store atomically and asks the native list to rebuild.
	void apply_snapshot(const std::vector<snapshot_entry>& entries);

	// Bumped on every apply_snapshot.
	int snapshot_version();

	std::vector<friend_record> get_friends();
	bool find_friend(unsigned long long steam_id_bits, friend_record& out);
	bool is_joinable(unsigned long long steam_id_bits);

	// Presence line shown under the friend's name in the native list.
	std::string get_presence_text(const friend_record& record);

	// Ask the launcher to join / invite a friend (sends over the IPC pipe).
	bool request_join(unsigned long long steam_id_bits);
	bool request_invite(unsigned long long steam_id_bits);
}
