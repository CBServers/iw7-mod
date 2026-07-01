#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "console/console.hpp"
#include "command.hpp"
#include "discord.hpp"
#include "nat.hpp"
#include "network.hpp"
#include "party.hpp"
#include "scheduler.hpp"

#include "game/game.hpp"
#include "game/ui_scripting/execution.hpp"

#include <utils/string.hpp>
#include <utils/cryptography.hpp>
#include <utils/http.hpp>

#include <discord_rpc.h>

#include <atomic>
#include <optional>
#include <utility>

/*
#define DEFAULT_AVATAR "discord_default_avatar"
#define AVATAR "discord_avatar_%s"

#define DEFAULT_AVATAR_URL "https://cdn.discordapp.com/embed/avatars/0.png"
#define AVATAR_URL "https://cdn.discordapp.com/avatars/%s/%s.png?size=128"
*/

namespace discord
{
	namespace
	{
		struct discord_presence_state_t
		{
			int start_timestamp;
			int party_size;
			int party_max;
		};

		struct discord_presence_strings_t
		{
			std::string state;
			std::string details;
			std::string small_image_key;
			std::string small_image_text;
			std::string large_image_key;
			std::string large_image_text;
			std::string party_id;
			std::string join_secret;
		};

		DiscordRichPresence discord_presence{};
		discord_presence_strings_t discord_strings;

		std::mutex avatar_map_mutex;
		std::unordered_map<std::string, game::Material*> avatar_material_map;
		game::Material* default_avatar_material{};

		constexpr auto* JOIN_SECRET_PREFIX = "iw7:1:";

		std::mutex pending_join_mutex;
		std::string pending_join_secret;

		// Invite-driven joins wait here until the game is ready to act on a connect.
		std::mutex pending_route_mutex;
		std::optional<std::pair<std::string, std::string>> pending_route; // (token, address)

		// Set once the engine reaches the menu; connect/network are live only after this.
		std::atomic_bool game_initialized{false};

		// Ownership handoff. wire_* is set from the IPC IO thread; the rest is main-thread only.
		constexpr auto OWNERSHIP_RELEASE_GRACE = 5s;
		std::atomic_bool wire_launcher_owns{false};
		bool effective_launcher_owns = false;
		bool presence_silent = false;
		bool release_pending = false;
		std::chrono::steady_clock::time_point release_deadline{};

		std::string truncate(const std::string& value, const size_t max_length)
		{
			return value.size() <= max_length ? value : value.substr(0, max_length);
		}

		// Splits "ip:port" into its parts.
		bool split_address(const std::string& address, std::string& ip, int& port)
		{
			const auto sep = address.rfind(':');
			if (sep == std::string::npos)
			{
				return false;
			}

			ip = address.substr(0, sep);
			port = atoi(address.substr(sep + 1).data());
			return !ip.empty() && port > 0;
		}

		// Short runtime play-mode key. Safe on the main thread only (calls into the game).
		std::string get_presence_mode()
		{
			switch (game::Com_GameMode_GetActiveGameMode())
			{
			case game::GAME_MODE_SP:
				return "sp";
			case game::GAME_MODE_CP:
				return "zm";
			default:
				return "mp";
			}
		}

		// In a live match (not the pre-network shell / frontend menu).
		bool is_ingame()
		{
			return game::CL_IsGameClientActive(0) && !game::Com_FrontEndScene_IsActive();
		}

		std::string get_join_address()
		{
			if (!is_ingame())
			{
				return {};
			}

			// Host: our reachable endpoint, paired with a real token to hole-punch.
			if (auto endpoint = nat::get_host_endpoint(); !endpoint.empty())
			{
				return endpoint;
			}

			// Client on a directly-reachable server: advertise it (token will be "-").
			const auto& connected = party::get_server_connection_state()->host;
			if (network::is_connectable_address(connected) && !network::is_private_ip(connected))
			{
				return network::address_to_string(connected);
			}

			return {};
		}

		std::string make_join_secret(const std::string& address)
		{
			if (address.empty())
			{
				return {};
			}

			// "iw7:1:<token>:<ip>:<port>"; token "-" means direct-only (no punch).
			const auto token = nat::current_token();
			const auto token_field = token.empty() ? std::string("-") : token;

			const auto secret = std::string(JOIN_SECRET_PREFIX) + token_field + ":" + address;
			if (secret.size() >= 128)
			{
				return {};
			}

			return secret;
		}

		bool parse_join_secret(const std::string& secret, std::string& token, std::string& address)
		{
			if (!utils::string::starts_with(secret, JOIN_SECRET_PREFIX))
			{
				return false;
			}

			// "<token>:<ip>:<port>"
			const auto raw = secret.substr(std::strlen(JOIN_SECRET_PREFIX));
			const auto sep = raw.find(':');
			if (sep == std::string::npos)
			{
				return false;
			}

			token = raw.substr(0, sep);

			const auto raw_address = raw.substr(sep + 1);
			const auto parsed = network::address_from_string(raw_address);
			if (!network::is_connectable_address(parsed))
			{
				return false;
			}

			address = network::address_to_string(parsed);
			return true;
		}

		void process_pending_join()
		{
			std::string secret;
			{
				std::lock_guard<std::mutex> lock(pending_join_mutex);
				secret = std::move(pending_join_secret);
				pending_join_secret.clear();
			}

			if (secret.empty())
			{
				return;
			}

			std::string token;
			std::string address;
			if (!parse_join_secret(secret, token, address))
			{
				// Legacy/raw-address invite (pre-token secrets were just "ip:port").
				const auto parsed = network::address_from_string(secret);
				if (network::is_connectable_address(parsed))
				{
					command::execute("connect " + network::address_to_string(parsed));
				}
				else
				{
					console::error("Discord: invalid join secret\n");
				}
				return;
			}

			route_join(token, address);
		}

		// True once the game can act on a connect (menu reached, online data synced); routing earlier crashes.
		bool join_ready()
		{
			return game_initialized.load() && game::Live_SyncOnlineDataFlags(0) == 0;
		}

		// Drains an invite-driven join, but only once join_ready() (routing mid-load crashes).
		void process_pending_route()
		{
			{
				std::lock_guard<std::mutex> lock(pending_route_mutex);
				if (!pending_route)
				{
					return;
				}
			}

			if (!join_ready())
			{
				return; // engine still coming up; keep waiting
			}

			std::pair<std::string, std::string> route;
			{
				std::lock_guard<std::mutex> lock(pending_route_mutex);
				if (!pending_route)
				{
					return;
				}
				route = std::move(*pending_route);
				pending_route.reset();
			}

			route_join(route.first, route.second);
		}

		// Native<->silent handoff, debounced on release so a launcher restart doesn't flicker the card.
		void ownership_tick()
		{
			if (wire_launcher_owns.load())
			{
				release_pending = false;
				if (!effective_launcher_owns)
				{
					effective_launcher_owns = true;
					presence_silent = true;
					Discord_ClearPresence(); // clear once on entry; keep the connection initialized
				}
				return;
			}

			if (!effective_launcher_owns)
			{
				return;
			}

			const auto now = std::chrono::steady_clock::now();
			if (!release_pending)
			{
				release_pending = true;
				release_deadline = now + OWNERSHIP_RELEASE_GRACE;
				return;
			}

			if (now >= release_deadline)
			{
				effective_launcher_owns = false;
				release_pending = false;
				presence_silent = false; // native RPC resumes on the next update_discord tick
			}
		}

		const char* get_large_image_name()
		{
			const auto mode = game::Com_GameMode_GetActiveGameMode();
			switch (mode)
			{
			case game::GAME_MODE_SP:
				return "menu_singleplayer";
			case game::GAME_MODE_CP:
				return "menu_zombies";
			case game::GAME_MODE_MP:
			default:
				return "menu_multiplayer";
			}
		}

		void update_discord_frontend()
		{
			discord_presence.details = game::G_GAME_MODE_STRINGS_FORMATTED[game::Com_GameMode_GetActiveGameMode()];
			if (game::Com_GameMode_GetActiveGameMode() == game::GAME_MODE_NONE)
			{
				discord_presence.details = game::G_GAME_MODE_STRINGS_FORMATTED[game::GAME_MODE_MP];
			}

			discord_presence.startTimestamp = 0;

			/*
			static const auto in_firing_range = game::Dvar_FindVar("virtualLobbyInFiringRange");
			if (in_firing_range != nullptr && in_firing_range->current.enabled == 1)
			{
				discord_presence.state = "Firing Range";
				discord_presence.largeImageKey = "mp_vlobby_room";
			}
			else
			{
			*/
			discord_presence.state = "Main Menu";
			discord_presence.largeImageKey = get_large_image_name();
			//}

			Discord_UpdatePresence(&discord_presence);
		}

		void update_discord_ingame()
		{
			static const game::dvar_t* mapname_dvar = nullptr;
			static const game::dvar_t* gametype_dvar = nullptr;
			static const game::dvar_t* max_clients_dvar = nullptr;

			if (!mapname_dvar) mapname_dvar = game::Dvar_FindVar("ui_mapname");
			if (!gametype_dvar) gametype_dvar = game::Dvar_FindVar("ui_gametype");
			if (!max_clients_dvar) max_clients_dvar = game::Dvar_FindVar("ui_maxclients");

			static std::string mapname_str = "mp_frontend";
			const char* mapname = mapname_str.c_str();

			if (mapname_dvar && strcmp(mapname, mapname_dvar->current.string) != 0)
			{
				mapname_str = utils::string::copy(mapname_dvar->current.string, std::strlen(mapname_dvar->current.string));
				mapname = mapname_str.c_str();
			}

			discord_strings.large_image_key = mapname;

			const auto mode = game::Com_GameMode_GetActiveGameMode();

			if (mode == game::GAME_MODE_CP || mode == game::GAME_MODE_MP)
			{
				const auto* gametype_ui = game::UI_GetGameTypeDisplayName(gametype_dvar != nullptr ? gametype_dvar->current.string : "");
				const auto* mapname_ui = game::UI_GetMapDisplayName(mapname);

				discord_strings.details = std::format("{} on {}", gametype_ui, mapname_ui);

				discord_presence.partySize = *reinterpret_cast<int*>(0x14434FEF0); // probably numClients from snapshot

				if (game::SV_Loaded() && !game::Com_FrontEnd_IsInFrontEnd())
				{
					discord_strings.state = "Private Match";
					discord_presence.partyMax = (max_clients_dvar ? max_clients_dvar->current.integer : 12);
				}
				else
				{
					auto* server_connection_state = party::get_server_connection_state();

					discord_strings.state = utils::string::strip(server_connection_state->hostname);
					discord_presence.partyMax = server_connection_state->max_clients;
				}

				// Join secret: an open private match (hole-punch) OR a directly reachable server.
				const auto join_address = get_join_address();
				discord_strings.join_secret = make_join_secret(join_address);

				if (!discord_strings.join_secret.empty())
				{
					discord_strings.party_id = utils::cryptography::sha1::compute(join_address, true).substr(0, 8);
					discord_presence.partyPrivacy = DISCORD_PARTY_PUBLIC;
				}
				else
				{
					discord_strings.party_id.clear();
					discord_presence.partyPrivacy = DISCORD_PARTY_PRIVATE;
				}

				auto server_discord_info = party::get_server_discord_info();
				if (server_discord_info.has_value())
				{
					discord_strings.small_image_key = server_discord_info->image;
					discord_strings.small_image_text = server_discord_info->image_text;
				}
			}
			else if (mode == game::GAME_MODE_SP)
			{
				discord_strings.details = mapname;
			}

			if (discord_presence.startTimestamp == 0)
			{
				discord_presence.startTimestamp = std::chrono::duration_cast<std::chrono::seconds>(
					std::chrono::system_clock::now().time_since_epoch()).count();
			}

			discord_presence.state = discord_strings.state.data();
			discord_presence.details = discord_strings.details.data();
			discord_presence.smallImageKey = discord_strings.small_image_key.data();
			discord_presence.smallImageText = discord_strings.small_image_text.data();
			discord_presence.largeImageKey = discord_strings.large_image_key.data();
			discord_presence.largeImageText = discord_strings.large_image_text.data();
			discord_presence.partyId = discord_strings.party_id.empty() ? nullptr : discord_strings.party_id.data();
			discord_presence.joinSecret = discord_strings.join_secret.empty() ? nullptr : discord_strings.join_secret.data();

			Discord_UpdatePresence(&discord_presence);
		}

		void update_discord()
		{
			if (presence_silent)
			{
				return; // launcher owns presence; stay silent but connected
			}

			const auto saved_time = discord_presence.startTimestamp;
			discord_presence = {};
			discord_presence.startTimestamp = saved_time;

			if (!game::CL_IsGameClientActive(0) || game::Com_FrontEndScene_IsActive())
			{
				update_discord_frontend();
			}
			else
			{
				update_discord_ingame();
			}
		}

		/*
		game::Material* create_avatar_material(const std::string& name, const std::string& data)
		{
			const auto material = materials::create_material(name);
			try
			{
				if (!materials::setup_material_image(material, data))
				{
					materials::free_material(material);
					return nullptr;
				}

				{
					std::lock_guard _0(avatar_map_mutex);
					avatar_material_map.insert(std::make_pair(name, material));
				}

				return material;
			}
			catch (const std::exception& e)
			{
				materials::free_material(material);
				console::error("Failed to load user avatar image: %s\n", e.what());
			}

			return nullptr;
		}

		void download_user_avatar(const std::string& id, const std::string& avatar)
		{
			const auto data = utils::http::get_data(
				utils::string::va(AVATAR_URL, id.data(), avatar.data()));
			if (!data.has_value())
			{
				return;
			}

			const auto& value = data.value();
			if (value.code != CURLE_OK)
			{
				return;
			}

			const auto name = utils::string::va(AVATAR, id.data());
			create_avatar_material(name, value.buffer);
		}

		void download_default_avatar()
		{
			const auto data = utils::http::get_data(DEFAULT_AVATAR_URL);
			if (!data.has_value())
			{
				return;
			}

			const auto& value = data.value();
			if (value.code != CURLE_OK)
			{
				return;
			}

			default_avatar_material = create_avatar_material(DEFAULT_AVATAR, value.buffer);
		}
		*/

		void ready(const DiscordUser* request)
		{
			DiscordRichPresence presence{};
			presence.instance = 1;
			presence.state = "";
			console::info("Discord: Ready on %s (%s)\n", request->username, request->userId);

			// Don't prime a card while the launcher owns presence (e.g. a Discord reconnect mid-session).
			if (!presence_silent)
			{
				Discord_UpdatePresence(&presence);
			}
		}

		void errored(const int error_code, const char* message)
		{
			console::error("Discord: %s (%i)\n", message, error_code);
		}

		void join_game(const char* join_secret)
		{
			if (!join_secret || !join_secret[0])
			{
				return;
			}

			console::debug("Discord: join_game called with secret '%s'\n", join_secret);

			// Queue here (Discord callback thread); process_pending_join does the work on main.
			std::lock_guard<std::mutex> lock(pending_join_mutex);
			pending_join_secret = join_secret;
		}

		/*
		std::string get_display_name(const DiscordUser* user)
		{
			if (user->discriminator != nullptr && user->discriminator != "0"s)
			{
				return std::format("{}#{}", user->username, user->discriminator);
			}
			else if (user->globalName[0] != 0)
			{
				return user->globalName;
			}
			else
			{
				return user->username;
			}
		}
		*/

		void join_request(const DiscordUser* request)
		{
			console::debug("Discord: Join request from %s (%s)\n", request->username, request->userId);
			Discord_Respond(request->userId, DISCORD_REPLY_IGNORE);

			/*
			if (game::Com_FrontEnd_IsInFrontEnd() || !ui_scripting::lui_running())
			{
				Discord_Respond(request->userId, DISCORD_REPLY_IGNORE);
				return;
			}

			static std::unordered_map<std::string, std::chrono::high_resolution_clock::time_point> last_requests;

			const std::string user_id = request->userId;
			const std::string avatar = request->avatar;
			const std::string discriminator = request->discriminator;
			const std::string username = request->username;
			const auto display_name = get_display_name(request);

			const auto now = std::chrono::high_resolution_clock::now();
			auto iter = last_requests.find(user_id);
			if (iter != last_requests.end())
			{
				if ((now - iter->second) < 15s)
				{
					return;
				}
				else
				{
					iter->second = now;
				}
			}
			else
			{
				last_requests.insert(std::make_pair(user_id, now));
			}

			// TODO: lui work to support join requests (check h1-mod)
			scheduler::once([=]
			{
				const ui_scripting::table request_table{};
				request_table.set("avatar", avatar);
				request_table.set("discriminator", discriminator);
				request_table.set("userid", user_id);
				request_table.set("username", username);
				request_table.set("displayname", display_name);

				ui_scripting::notify("discord_join_request",
				{
					{"request", request_table}
				});
			}, scheduler::pipeline::lui);

			const auto material_name = utils::string::va(AVATAR, user_id.data());
			if (!avatar.empty() && !avatar_material_map.contains(material_name))
			{
				download_user_avatar(user_id, avatar);
			}
			*/
		}
		
		// TODO
		/*
		void set_default_bindings()
		{
			const auto set_binding = [](const std::string& command, const game::keyNum_t key)
			{
				const auto binding = game::Key_GetBindingForCmd(command.data());
				for (auto i = 0; i < 256; i++)
				{
					if (game::playerKeys[0].keys[i].binding == binding)
					{
						return;
					}
				}

				if (game::playerKeys[0].keys[key].binding == 0)
				{
					game::Key_SetBinding(0, key, binding);
				}
			};

			set_binding("discord_accept", game::K_F1);
			set_binding("discord_deny", game::K_F2);
		}
		*/
	}

	/*
	game::Material* get_avatar_material(const std::string& id)
	{
		const auto material_name = utils::string::va(AVATAR, id.data());
		const auto iter = avatar_material_map.find(material_name);
		if (iter == avatar_material_map.end())
		{
			return default_avatar_material;
		}

		return iter->second;
	}

	void respond(const std::string& id, int reply)
	{
		scheduler::once([=]()
		{
			Discord_Respond(id.data(), reply);
		}, scheduler::pipeline::async);
	}
	*/

	presence_state get_presence_state()
	{
		presence_state state{};

		// Runtime play mode (mp/zm/sp); reported even at the menu so the launcher knows what's running.
		state.mode = get_presence_mode();

		state.in_game = is_ingame();
		if (!state.in_game)
		{
			return state; // menu => empty map
		}

		const auto* mapname_dvar = game::Dvar_FindVar("ui_mapname");
		const auto* gametype_dvar = game::Dvar_FindVar("ui_gametype");
		const std::string mapname = mapname_dvar && mapname_dvar->current.string ? mapname_dvar->current.string : std::string{};
		const std::string gametype = gametype_dvar && gametype_dvar->current.string ? gametype_dvar->current.string : std::string{};

		state.mapname = mapname;
		state.map_display = mapname.empty()
			                    ? std::string{}
			                    : truncate(utils::string::strip(game::UI_GetMapDisplayName(mapname.data())), 128);
		state.gametype = gametype.empty()
			                 ? std::string{}
			                 : truncate(utils::string::strip(game::UI_GetGameTypeDisplayName(gametype.data())), 128);
		state.server_name = truncate(utils::string::strip(party::get_public_server_name()), 128);

		// Player counts only make sense outside SP (the snapshot global is stale there).
		if (state.mode != "sp")
		{
			state.players = *reinterpret_cast<int*>(0x14434FEF0); // numClients from snapshot

			const auto* max_clients_dvar = game::Dvar_FindVar("ui_maxclients");
			if (game::SV_Loaded() && !game::Com_FrontEnd_IsInFrontEnd())
			{
				state.max_players = max_clients_dvar ? max_clients_dvar->current.integer : 0;
			}
			else
			{
				state.max_players = party::get_server_connection_state()->max_clients;
			}
		}

		return state;
	}

	std::optional<join_transport> get_join_transport()
	{
		const auto address = get_join_address();
		if (address.empty())
		{
			return std::nullopt;
		}

		const auto token = nat::current_token();

		join_transport transport{};
		if (token.empty())
		{
			// Directly reachable public server: advertise it as a direct connect.
			if (!split_address(address, transport.ip, transport.port))
			{
				return std::nullopt;
			}
			transport.is_nat = false;
			return transport;
		}

		// Hosting: NAT punch with the reachable endpoint as fallback.
		transport.is_nat = true;
		transport.token = token;
		nat::get_rendezvous(transport.rendezvous_host, transport.rendezvous_port);
		split_address(address, transport.fallback_ip, transport.fallback_port);
		return transport;
	}

	void route_join(const std::string& token, const std::string& address)
	{
		// "-" / empty token => friend is on a directly reachable server.
		if (token.empty() || token == "-")
		{
			command::execute("connect " + address);
			return;
		}

		// Hole-punch toward the host; falls back to `address` (port-forward/VPN).
		nat::begin_join(token, address);
	}

	void queue_join(const std::string& token, const std::string& address)
	{
		std::lock_guard<std::mutex> lock(pending_route_mutex);
		pending_route = std::make_pair(token, address);
	}

	void set_launcher_presence_owner(const bool launcher_owns)
	{
		wire_launcher_owns.store(launcher_owns);
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

			DiscordEventHandlers handlers{};
			handlers.ready = ready;
			handlers.errored = errored;
			handlers.disconnected = errored;
			handlers.spectateGame = nullptr;
			handlers.joinGame = join_game;
			handlers.joinRequest = join_request; // not fully supported yet

			Discord_Initialize("1215500480873103400", &handlers, 1, nullptr);

			scheduler::on_game_initialized([]
			{
				game_initialized = true;
				/*
				scheduler::once(download_default_avatar, scheduler::async);
				set_default_bindings();
				*/
			}, scheduler::pipeline::main);

			scheduler::loop(update_discord, scheduler::pipeline::main, 5s);

			// Hand the Discord card to/from the launcher based on the IPC ownership signal.
			scheduler::loop(ownership_tick, scheduler::pipeline::main, 250ms);

			// Discord callbacks (and the resulting joins) must run on the main thread,
			// since join handling drives the NAT punch and the game's network socket,
			// and reads nat state that is only touched on main.
			scheduler::loop([]
			{
				Discord_RunCallbacks();
				process_pending_join();
			}, scheduler::pipeline::main, 250ms);

			// Invite-driven joins from the launcher wait for join_ready() before routing.
			scheduler::loop(process_pending_route, scheduler::pipeline::main, 250ms);

			initialized_ = true;

			/*
			command::add("discord_accept", []()
			{
				ui_scripting::notify("discord_response", {{"accept", true}});
			});

			command::add("discord_deny", []()
			{
				ui_scripting::notify("discord_response", {{"accept", false}});
			});
			*/
		}

		void pre_destroy() override
		{
			if (!initialized_ || game::environment::is_dedi())
			{
				return;
			}

			Discord_Shutdown();
		}

	private:
		bool initialized_ = false;
	};
}

REGISTER_COMPONENT(discord::component)