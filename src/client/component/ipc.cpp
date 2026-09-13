#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/game.hpp"

#include "discord.hpp"
#include "friends.hpp"
#include "ipc.hpp"
#include "nat.hpp"
#include "scheduler.hpp"

#include "game/ui_scripting/execution.hpp"
#include "ui_scripting.hpp"

#include <utils/concurrency.hpp>
#include <utils/thread.hpp>

#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

#include "version.hpp"

#include <atomic>
#include <deque>
#include <thread>

// Streams live presence to the launcher over a named pipe and routes launcher-driven joins.
namespace ipc
{
	namespace
	{
		constexpr auto* PIPE_NAME = L"\\\\.\\pipe\\cbservers-launcher";

		std::atomic_bool stop_io{false};
		std::atomic_bool force_resend{false};
		std::atomic_bool pipe_connected{false};
		std::thread io_thread;

		utils::concurrency::container<std::deque<std::string>>& get_queue()
		{
			static utils::concurrency::container<std::deque<std::string>> queue;
			return queue;
		}

		void enqueue(std::string line)
		{
			get_queue().access([&](std::deque<std::string>& queue)
			{
				queue.push_back(std::move(line));
			});
		}

		std::string serialize_presence(const discord::presence_state& state)
		{
			rapidjson::Document doc;
			doc.SetObject();
			auto& allocator = doc.GetAllocator();

			doc.AddMember(rapidjson::StringRef("type"), rapidjson::StringRef("presence"), allocator);

			const auto add = [&](const char* key, const std::string& value)
			{
				rapidjson::Value json_value;
				json_value.SetString(value.data(), static_cast<rapidjson::SizeType>(value.size()), allocator);
				doc.AddMember(rapidjson::StringRef(key), json_value, allocator);
			};

			add("map", state.mapname);
			add("mapDisplay", state.map_display);
			add("mode", state.mode);
			add("gametype", state.gametype);
			add("gametypeRaw", state.gametype_raw);
			add("serverName", state.server_name);
			add("matchId", state.match_id);
			doc.AddMember(rapidjson::StringRef("players"), state.players, allocator);
			doc.AddMember(rapidjson::StringRef("maxPlayers"), state.max_players, allocator);
			doc.AddMember(rapidjson::StringRef("openable"), state.openable, allocator);

			// Optional join transport: omitted when not joinable.
			if (const auto transport = discord::get_join_transport())
			{
				const auto obj_str = [&](rapidjson::Value& obj, const char* key, const std::string& value)
				{
					rapidjson::Value v;
					v.SetString(value.data(), static_cast<rapidjson::SizeType>(value.size()), allocator);
					obj.AddMember(rapidjson::StringRef(key), v, allocator);
				};

				rapidjson::Value t(rapidjson::kObjectType);
				if (transport->is_nat)
				{
					obj_str(t, "kind", "nat");
					obj_str(t, "token", transport->token);

					rapidjson::Value rv(rapidjson::kObjectType);
					obj_str(rv, "host", transport->rendezvous_host);
					rv.AddMember(rapidjson::StringRef("port"), transport->rendezvous_port, allocator);
					t.AddMember(rapidjson::StringRef("rendezvous"), rv, allocator);

					rapidjson::Value fb(rapidjson::kObjectType);
					obj_str(fb, "ip", transport->fallback_ip);
					fb.AddMember(rapidjson::StringRef("port"), transport->fallback_port, allocator);
					t.AddMember(rapidjson::StringRef("fallback"), fb, allocator);
				}
				else
				{
					obj_str(t, "kind", "direct");
					obj_str(t, "ip", transport->ip);
					t.AddMember(rapidjson::StringRef("port"), transport->port, allocator);
				}

				doc.AddMember(rapidjson::StringRef("transport"), t, allocator);
			}

			rapidjson::StringBuffer buffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
			doc.Accept(writer);

			return std::string(buffer.GetString(), buffer.GetSize()) + "\n";
		}

		// Runs on the main pipeline (touches game state). Throttled to changes in presence.
		void send_presence()
		{
			static std::string last_sent;

			if (force_resend.exchange(false))
			{
				last_sent.clear();
			}

			auto line = serialize_presence(discord::get_presence_state());
			if (line == last_sent)
			{
				return;
			}

			last_sent = line;
			enqueue(std::move(line));
		}

		bool write_all(const HANDLE pipe, const std::string& data)
		{
			DWORD written = 0;
			return WriteFile(pipe, data.data(), static_cast<DWORD>(data.size()), &written, nullptr)
				&& written == static_cast<DWORD>(data.size());
		}

		std::string jstr(const rapidjson::Value& value, const char* key)
		{
			return (value.HasMember(key) && value[key].IsString()) ? value[key].GetString() : std::string();
		}

		int jint(const rapidjson::Value& value, const char* key)
		{
			return (value.HasMember(key) && value[key].IsInt()) ? value[key].GetInt() : 0;
		}

		bool jbool(const rapidjson::Value& value, const char* key)
		{
			return value.HasMember(key) && value[key].IsBool() && value[key].GetBool();
		}

		// Full friends snapshot from the launcher; parsed on the io thread, the store swap is thread-safe.
		void handle_friends(const rapidjson::Value& doc)
		{
			if (!doc.HasMember("friends") || !doc["friends"].IsArray())
			{
				return;
			}

			std::vector<friends::snapshot_entry> entries;
			for (const auto& item : doc["friends"].GetArray())
			{
				if (!item.IsObject())
				{
					continue;
				}

				friends::snapshot_entry entry{};
				entry.discord_id = jstr(item, "id");
				entry.name = jstr(item, "name");
				entry.status = jstr(item, "status");
				entry.in_launcher = jbool(item, "inLauncher");

				if (item.HasMember("game") && item["game"].IsObject())
				{
					const auto& g = item["game"];
					entry.has_game = true;
					entry.game_id = jstr(g, "id");
					entry.mode = jstr(g, "mode");
					entry.map = jstr(g, "map");
					entry.gametype = jstr(g, "gametype");
					entry.joinable = jbool(g, "joinable");
					entry.same_match = jbool(g, "sameMatch");
				}

				entries.push_back(std::move(entry));
			}

			friends::apply_snapshot(entries);
		}

		// Raised on the LUI root by ui_scripts/CBToast; waits out a Lua VM restart (map load) for a few seconds.
		void show_toast(const std::string& kicker, const std::string& title, const std::string& body)
		{
			const auto deadline = std::chrono::steady_clock::now() + 10s;
			scheduler::schedule([=]
			{
				if (std::chrono::steady_clock::now() > deadline)
				{
					return scheduler::cond_end;
				}

				if (!ui_scripting::lui_running())
				{
					return scheduler::cond_continue;
				}

				ui_scripting::notify("cb_toast", {{"kicker", kicker}, {"title", title}, {"body", body}});
				return scheduler::cond_end;
			}, scheduler::pipeline::lui, 250ms);
		}

		// Passive heads-up for an incoming invite; accepting still happens in the launcher.
		void handle_notify(const rapidjson::Value& doc)
		{
			if (jstr(doc, "kind") != "invite")
			{
				return;
			}

			// Remote-controlled text going into LUI: printable ASCII, no color codes, capped.
			std::string from;
			for (const auto c : jstr(doc, "from"))
			{
				if ((c > 0x20 && c < 0x7F && c != '^') || (c == ' ' && !from.empty()))
				{
					from.push_back(c);
				}
			}

			from.resize(std::min<size_t>(from.size(), 32));
			while (!from.empty() && from.back() == ' ')
			{
				from.pop_back();
			}

			if (from.empty())
			{
				from = "A friend";
			}

			// Io thread only. Keeps a burst of invites from stacking LUI calls.
			static std::chrono::steady_clock::time_point last_toast{};
			static std::string last_from;

			const auto now = std::chrono::steady_clock::now();
			if (now - last_toast < 3s || (from == last_from && now - last_toast < 15s))
			{
				return;
			}

			last_toast = now;
			last_from = from;

			show_toast("INVITE", from + " invited you", "Accept in CB Launcher");
		}

		// Route an invite's structured transport (data, never a console string) like a native join, then ack.
		void handle_connect(const rapidjson::Value& doc)
		{
			const auto id = jstr(doc, "id");

			std::string token = "-";
			std::string address;

			if (doc.HasMember("transport") && doc["transport"].IsObject())
			{
				const auto& t = doc["transport"];
				const auto kind = jstr(t, "kind");
				if (kind == "direct")
				{
					const auto ip = jstr(t, "ip");
					const auto port = jint(t, "port");
					if (!ip.empty() && port > 0)
					{
						address = ip + ":" + std::to_string(port);
					}
				}
				else if (kind == "nat")
				{
					token = jstr(t, "token");
					if (t.HasMember("fallback") && t["fallback"].IsObject())
					{
						const auto& fb = t["fallback"];
						const auto ip = jstr(fb, "ip");
						const auto port = jint(fb, "port");
						if (!ip.empty() && port > 0)
						{
							address = ip + ":" + std::to_string(port);
						}
					}
				}
			}

			const bool accepted = !address.empty() || (token != "-" && !token.empty());
			if (accepted)
			{
				// Defer until the game is ready; a cold-launched invite arrives mid-load and would crash.
				discord::queue_join(token, address);
			}

			// Through rapidjson so a hostile/odd id can't splice or break the line protocol.
			rapidjson::Document ack;
			ack.SetObject();
			auto& allocator = ack.GetAllocator();
			ack.AddMember(rapidjson::StringRef("type"), rapidjson::StringRef("connect-ack"), allocator);
			rapidjson::Value id_value;
			id_value.SetString(id.data(), static_cast<rapidjson::SizeType>(id.size()), allocator);
			ack.AddMember(rapidjson::StringRef("id"), id_value, allocator);
			ack.AddMember(rapidjson::StringRef("accepted"), accepted, allocator);

			rapidjson::StringBuffer buffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
			ack.Accept(writer);
			enqueue(std::string(buffer.GetString(), buffer.GetSize()) + "\n");
		}

		// Launcher->client messages: hello-ack and presence-owner carry the ownership signal; connect joins.
		void handle_incoming_line(const std::string& line)
		{
			rapidjson::Document doc;
			doc.Parse(line.data(), line.size());
			if (doc.HasParseError() || !doc.IsObject())
			{
				return;
			}

			if (!doc.HasMember("type") || !doc["type"].IsString())
			{
				return;
			}

			const std::string type = doc["type"].GetString();
			if (type == "hello-ack" || type == "presence-owner")
			{
				if (doc.HasMember("presenceOwner") && doc["presenceOwner"].IsString())
				{
					const std::string owner = doc["presenceOwner"].GetString();
					discord::set_launcher_presence_owner(owner == "launcher");
				}
			}
			else if (type == "connect")
			{
				handle_connect(doc);
			}
			else if (type == "friends")
			{
				handle_friends(doc);
			}
			else if (type == "notify")
			{
				handle_notify(doc);
			}
			else if (type == "open-match")
			{
				// Launcher-approved knock/invite: open the match, ack, and push the transport immediately.
				scheduler::once([]
				{
					const auto opened = nat::open_to_friends();
					enqueue(std::string(R"({"type":"open-match-ack","opened":)")
						+ (opened ? "true" : "false") + "}\n");
					send_presence();
				}, scheduler::pipeline::main);
			}
		}

		void interruptible_sleep(const std::chrono::milliseconds total)
		{
			constexpr auto step = 100ms;
			for (auto elapsed = 0ms; elapsed < total && !stop_io; elapsed += step)
			{
				std::this_thread::sleep_for(step);
			}
		}

		HANDLE connect_with_retry()
		{
			while (!stop_io)
			{
				const auto pipe = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
				                              OPEN_EXISTING, 0, nullptr);
				if (pipe != INVALID_HANDLE_VALUE)
				{
					return pipe;
				}

				// Launcher not running yet (or no free instance) — retry.
				interruptible_sleep(2s);
			}
			return INVALID_HANDLE_VALUE;
		}

		void io_loop()
		{
			while (!stop_io)
			{
				const auto pipe = connect_with_retry();
				if (pipe == INVALID_HANDLE_VALUE)
				{
					break;
				}

				// Constant: hello fires off-thread pre-unpack (no game calls); the live mode rides in presence.
				const auto hello = std::string(
						R"({"type":"hello","protocolVersion":1,"game":"iw7-mod","clientVersion":")")
					+ VERSION + R"(","mode":"mp"})" + "\n";

				pipe_connected = true;
				bool alive = write_all(pipe, hello);
				force_resend = true; // make the next tick re-stream presence for this connection

				std::string inbuf;

				while (alive && !stop_io)
				{
					std::deque<std::string> pending;
					get_queue().access([&](std::deque<std::string>& queue)
					{
						pending.swap(queue);
					});

					for (const auto& line : pending)
					{
						if (!write_all(pipe, line))
						{
							alive = false;
							break;
						}
					}

					// Detect a broken pipe (launcher gone) and read any incoming messages.
					DWORD available = 0;
					if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
					{
						alive = false;
						break;
					}
					if (available > 0)
					{
						char scratch[512];
						DWORD read = 0;
						if (ReadFile(pipe, scratch, sizeof(scratch), &read, nullptr) && read > 0)
						{
							inbuf.append(scratch, read);

							size_t newline;
							while ((newline = inbuf.find('\n')) != std::string::npos)
							{
								auto line = inbuf.substr(0, newline);
								inbuf.erase(0, newline + 1);
								if (!line.empty() && line.back() == '\r')
								{
									line.pop_back();
								}
								if (!line.empty())
								{
									handle_incoming_line(line);
								}
							}
						}
					}

					if (alive)
					{
						std::this_thread::sleep_for(200ms);
					}
				}

				pipe_connected = false;
				CloseHandle(pipe);
				// Pipe lost: hand presence back to native RPC and drop the launcher-fed friends before they go stale.
				discord::set_launcher_presence_owner(false);
				friends::apply_snapshot({});
				get_queue().access([](std::deque<std::string>& queue) { queue.clear(); });
			}
		}
	}

	void send_message(std::string line)
	{
		if (!pipe_connected)
		{
			return;
		}

		if (line.empty() || line.back() != '\n')
		{
			line.push_back('\n');
		}

		enqueue(std::move(line));
	}

	void flush_presence()
	{
		scheduler::once(send_presence, scheduler::pipeline::main);
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			// Dedicated servers don't stream presence.
			if (game::environment::is_dedi())
			{
				return;
			}

			stop_io = false;
			io_thread = utils::thread::create_named_thread("IPC", io_loop);
			scheduler::loop(send_presence, scheduler::pipeline::main, 2s);
		}

		void pre_destroy() override
		{
			stop_io = true;
			if (io_thread.joinable())
			{
				// Nudge any WriteFile/ReadFile blocked on a wedged launcher until the thread observes stop_io.
				while (WaitForSingleObject(io_thread.native_handle(), 100) == WAIT_TIMEOUT)
				{
					CancelSynchronousIo(io_thread.native_handle());
				}
				io_thread.join();
			}
		}
	};
}

REGISTER_COMPONENT(ipc::component)
