#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/game.hpp"

#include "console/console.hpp"
#include "discord.hpp"
#include "dvars.hpp"
#include "friends.hpp"
#include "ipc.hpp"
#include "nat.hpp"
#include "scheduler.hpp"

#include <utils/concurrency.hpp>
#include <utils/hook.hpp>
#include <utils/string.hpp>

// Store for CB-Launcher friends fed over IPC; the ISteamFriends stubs render it into the native FriendsList UI
namespace friends
{
	namespace
	{
		// Native per-controller friend list (2056 bytes each). The refresh pass runs while +2040 is set,
		// resuming from +2048 and clearing +2040 when the whole Steam list has been walked.
		constexpr auto NATIVE_LIST_BASE = 0x14811C6B0;
		constexpr auto NATIVE_LIST_STRIDE = 2056;

		struct store_t
		{
			std::vector<friend_record> list;
			std::unordered_map<std::string, unsigned int> account_ids; // discord id -> sticky account id
			unsigned int next_account_id{1000001};
			int version{0};
		};

		utils::concurrency::container<store_t>& get_store()
		{
			static utils::concurrency::container<store_t> store;
			return store;
		}

		// steam_id with account_instance=1, account_type=1 (individual), universe=1 (public)
		unsigned long long make_steam_id_bits(const unsigned int account_id)
		{
			return (1ull << 56) | (1ull << 52) | (1ull << 32) | account_id;
		}

		int map_persona_state(const std::string& status)
		{
			if (status == "online") return 1;
			if (status == "dnd") return 2;
			if (status == "idle") return 3;
			return 0;
		}

		// Main thread only. Forces the native list to re-walk ISteamFriends on the next frame.
		void poke_native_refresh()
		{
			auto* list = reinterpret_cast<std::uint8_t*>(NATIVE_LIST_BASE);
			*reinterpret_cast<int*>(list + 2048) = 0;
			*reinterpret_cast<int*>(list + 2044) = 0;
			list[2040] = 1;
		}

		bool can_invite(const friend_record& record)
		{
			if (record.same_match)
			{
				return false;
			}

			return discord::get_join_transport().has_value() || nat::can_open_to_friends();
		}

		utils::hook::detour is_friend_joinable_hook;
		utils::hook::detour join_online_friend_hook;
		utils::hook::detour invite_online_friend_hook;
		utils::hook::detour is_friend_invitable_hook;

		// Backend of Friends.IsFriendJoinable; drives the JOINABLE label and the JOIN GAME button.
		char is_friend_joinable_stub(const unsigned int controller, const unsigned long long xuid, const char check_full)
		{
			friend_record record{};
			if (find_friend(xuid, record))
			{
				return record.joinable ? 1 : 0;
			}

			return is_friend_joinable_hook.invoke<char>(controller, xuid, check_full);
		}

		// Backend of Friends.JoinOnlineFriend; the native path needs a real Steam lobby our friends don't have.
		void* join_online_friend_stub(const unsigned int controller, const unsigned long long xuid)
		{
			friend_record record{};
			if (find_friend(xuid, record))
			{
				if (record.joinable)
				{
					request_join(xuid);
				}
				return nullptr;
			}

			return join_online_friend_hook.invoke<void*>(controller, xuid);
		}

		// Backend of Friends.InviteOnlineFriend; mirrors the native "invite sent" popup.
		std::int64_t invite_online_friend_stub(const unsigned int controller, const unsigned long long xuid)
		{
			friend_record record{};
			if (find_friend(xuid, record))
			{
				if (can_invite(record) && request_invite(xuid))
				{
					game::LUI_OpenMenu(0, "popup_friend_invite_sent", 1, 0, 1);
				}
				return 0;
			}

			return invite_online_friend_hook.invoke<std::int64_t>(controller, xuid);
		}

		// Backend of Friends.IsFriendInvitable; only offer an invite when the launcher could deliver it.
		bool is_friend_invitable_stub(const int controller, const unsigned long long xuid)
		{
			friend_record record{};
			if (find_friend(xuid, record))
			{
				return can_invite(record);
			}

			return is_friend_invitable_hook.invoke<bool>(controller, xuid);
		}
	}

	void apply_snapshot(const std::vector<snapshot_entry>& entries)
	{
		get_store().access([&](store_t& store)
		{
			std::vector<friend_record> list;
			list.reserve(entries.size());

			for (const auto& entry : entries)
			{
				if (entry.discord_id.empty())
				{
					continue;
				}

				auto id_it = store.account_ids.find(entry.discord_id);
				if (id_it == store.account_ids.end())
				{
					id_it = store.account_ids.emplace(entry.discord_id, store.next_account_id++).first;
				}

				friend_record record{};
				record.steam_id_bits = make_steam_id_bits(id_it->second);
				record.discord_id = entry.discord_id;
				record.name = entry.name.empty() ? entry.discord_id : entry.name;
				record.persona_state = map_persona_state(entry.status);
				record.in_game = entry.has_game && entry.game_id == "iw7-mod";
				record.same_match = record.in_game && entry.same_match;
				// Joining someone already in our match would just reconnect us to it.
				record.joinable = record.in_game && entry.joinable && !record.same_match;
				record.mode = entry.mode;
				record.map = entry.map;
				record.gametype = entry.gametype;
				list.push_back(std::move(record));
			}

			store.list = std::move(list);
			store.version++;
		});

		console::info("[friends] snapshot applied: %zu friends\n", entries.size());
		scheduler::once(poke_native_refresh, scheduler::pipeline::main);
	}

	int snapshot_version()
	{
		return get_store().access<int>([](const store_t& store)
		{
			return store.version;
		});
	}

	std::vector<friend_record> get_friends()
	{
		return get_store().access<std::vector<friend_record>>([](const store_t& store)
		{
			return store.list;
		});
	}

	bool find_friend(const unsigned long long steam_id_bits, friend_record& out)
	{
		return get_store().access<bool>([&](const store_t& store)
		{
			for (const auto& record : store.list)
			{
				if (record.steam_id_bits == steam_id_bits)
				{
					out = record;
					return true;
				}
			}
			return false;
		});
	}

	bool is_joinable(const unsigned long long steam_id_bits)
	{
		friend_record record{};
		return find_friend(steam_id_bits, record) && record.joinable;
	}

	std::string get_presence_text(const friend_record& record)
	{
		if (!record.in_game)
		{
			return {};
		}

		if (record.same_match)
		{
			return "In your match";
		}

		if (record.map.empty())
		{
			return "In Lobby";
		}

		const auto display_or_raw = [](const char* display, const std::string& raw)
		{
			const auto stripped = display ? utils::string::strip(display) : std::string{};
			return stripped.empty() ? raw : stripped;
		};

		const auto map_name = display_or_raw(game::UI_GetMapDisplayName(record.map.data()), record.map);

		if (record.mode == "zm")
		{
			return "Playing Zombies on " + map_name;
		}

		if (record.mode == "sp")
		{
			return "Playing Campaign";
		}

		const auto gametype_name = record.gametype.empty()
			                           ? std::string{}
			                           : display_or_raw(game::UI_GetGameTypeDisplayName(record.gametype.data()), record.gametype);

		return gametype_name.empty() ? "Playing on " + map_name : "Playing " + gametype_name + " on " + map_name;
	}

	bool request_join(const unsigned long long steam_id_bits)
	{
		friend_record record{};
		if (!find_friend(steam_id_bits, record) || record.discord_id.empty())
		{
			console::warn("[friends] request_join: no friend for %llX\n", steam_id_bits);
			return false;
		}

		ipc::send_message(utils::string::va(R"({"type":"join-friend","friendId":"%s"})", record.discord_id.data()));
		console::info("[friends] join-friend sent for %s (%s)\n", record.name.data(), record.discord_id.data());
		return true;
	}

	bool request_invite(const unsigned long long steam_id_bits)
	{
		friend_record record{};
		if (!find_friend(steam_id_bits, record) || record.discord_id.empty())
		{
			console::warn("[friends] request_invite: no friend for %llX\n", steam_id_bits);
			return false;
		}

		// Inviting is host consent: open a closed private match so the invite can carry a real transport.
		scheduler::once([]
		{
			if (nat::can_open_to_friends() && nat::open_to_friends())
			{
				ipc::flush_presence();
			}
		}, scheduler::pipeline::main);

		ipc::send_message(utils::string::va(R"({"type":"invite","friendId":"%s"})", record.discord_id.data()));
		console::info("[friends] invite sent for %s (%s)\n", record.name.data(), record.discord_id.data());
		return true;
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			if (game::environment::is_dedi())
			{
				return;
			}

			// The native cache never re-reads a cached friend's presence; our proxy is cheap, so query live.
			dvars::override::register_bool("friendsCacheSteamFriends", false, game::DVAR_FLAG_NONE);

			is_friend_joinable_hook.create(0x140DBA500, is_friend_joinable_stub);
			join_online_friend_hook.create(0x140DBA760, join_online_friend_stub);
			invite_online_friend_hook.create(0x140DBA5E0, invite_online_friend_stub);
			is_friend_invitable_hook.create(0x140DBA450, is_friend_invitable_stub);
		}
	};
}

REGISTER_COMPONENT(friends::component)
