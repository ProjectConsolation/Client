#ifndef CONSOLATION_RPC_PROTOCOL_TEST
#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "component/utils/scheduler.hpp"
#include "component/engine/console/command.hpp"
#include "game/dvars.hpp"
#endif
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <ctime>
#include <cstdint>

namespace discord_rpc
{
	// Discord's documented v1 local IPC protocol. No token, OAuth session or
	// redistributable DLL is needed for SET_ACTIVITY. Keep all pipe IO off-game.
	class component final : public component_interface
	{
		friend struct protocol_tests;
		struct snapshot
		{
			bool enabled{};
			std::string app, image, details, state;
		};
		std::mutex mutex_;
		snapshot pending_;
		std::string last_error_;
		std::atomic_bool stop_{false};
		std::atomic_int status_{0}; // disconnected, READY, activity acknowledged, error
		std::thread worker_;
		game::dvar_s *enabled_{}, *application_{}, *image_{};

		static bool valid_id(const std::string& id)
		{
			return id.size() >= 16 && id.size() <= 20
				&& id.find_first_not_of("0123456789") == std::string::npos;
		}
		static std::string text_dvar(const char* name)
		{
			const auto* dvar = game::Dvar_FindVar(name);
			return dvar && dvar->current.string ? std::string(dvar->current.string).substr(0, 128) : "";
		}
		// Overlapped transfers have a deadline. Cancellation completes before
		// stack buffers/events go away; the worker exclusively owns its handle.
		static bool transfer(HANDLE pipe, void* data, DWORD size, bool writing)
		{
			while (size)
			{
				OVERLAPPED operation{};
				operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
				if (!operation.hEvent) return false;
				DWORD done{};
				bool ok = (writing ? WriteFile(pipe, data, size, &done, &operation)
					: ReadFile(pipe, data, size, &done, &operation)) != FALSE;
				if (!ok && GetLastError() == ERROR_IO_PENDING)
				{
					if (WaitForSingleObject(operation.hEvent, 100) == WAIT_OBJECT_0)
						ok = GetOverlappedResult(pipe, &operation, &done, FALSE) != FALSE;
					else
					{
						CancelIoEx(pipe, &operation);
						GetOverlappedResult(pipe, &operation, &done, TRUE);
					}
				}
				CloseHandle(operation.hEvent);
				if (!ok || !done || done > size) return false;
				data = static_cast<char*>(data) + done;
				size -= done;
			}
			return true;
		}
		static bool send(HANDLE pipe, unsigned int opcode, const std::string& json)
		{
			const unsigned int header[2]{opcode, static_cast<unsigned int>(json.size())};
			std::string frame(reinterpret_cast<const char*>(header), sizeof(header));
			frame += json;
			return transfer(pipe, frame.data(), static_cast<DWORD>(frame.size()), true);
		}
		static std::string handshake(const std::string& app)
		{
			return "{\"v\":1,\"client_id\":\"" + app + "\"}";
		}
		static std::string activity(const snapshot& value, const std::string& nonce, std::time_t start)
		{
			rapidjson::StringBuffer buffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
			writer.StartObject(); writer.Key("cmd"); writer.String("SET_ACTIVITY");
			writer.Key("args"); writer.StartObject(); writer.Key("pid"); writer.Uint(GetCurrentProcessId());
			writer.Key("activity");
			if (!value.enabled) writer.Null();
			else
			{
				writer.StartObject(); writer.Key("details"); writer.String(value.details.c_str());
				writer.Key("state"); writer.String(value.state.c_str());
				writer.Key("timestamps"); writer.StartObject(); writer.Key("start");
				writer.Int64(static_cast<int64_t>(start)); writer.EndObject();
				if (!value.image.empty())
				{
					writer.Key("assets"); writer.StartObject(); writer.Key("large_image"); writer.String(value.image.c_str());
					writer.Key("large_text"); writer.String("Project: Consolation"); writer.EndObject();
				}
				writer.EndObject();
			}
			writer.EndObject(); writer.Key("nonce"); writer.String(nonce.c_str()); writer.EndObject();
			return {buffer.GetString(), buffer.GetSize()};
		}
		void run()
		{
			HANDLE pipe = INVALID_HANDLE_VALUE;
			std::string app, nonce, last;
			std::uint64_t sequence{};
			const auto start = std::time(nullptr);
			auto retry = std::chrono::steady_clock::time_point{};
			auto sent = retry, connected = retry;
			const auto disconnect = [&]
			{
				if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
				pipe = INVALID_HANDLE_VALUE; status_ = 0; last.clear(); nonce.clear();
				retry = std::chrono::steady_clock::now() + 5s;
			};
			try
			{
				while (!stop_)
				{
					snapshot value;
					{ std::lock_guard lock(mutex_); value = pending_; }
					const auto now = std::chrono::steady_clock::now();
					if (!value.enabled || !valid_id(value.app) || (pipe != INVALID_HANDLE_VALUE && app != value.app))
					{
						if (pipe != INVALID_HANDLE_VALUE)
						{
							value.enabled = false; send(pipe, 1, activity(value, "clear", start)); disconnect();
						}
					}
					else if (pipe == INVALID_HANDLE_VALUE && now >= retry)
					{
						for (int index = 0; index < 10; ++index)
						{
							const auto path = std::format("\\\\?\\pipe\\discord-ipc-{}", index);
							pipe = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
								OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
							if (pipe != INVALID_HANDLE_VALUE) break;
						}
						app = value.app; connected = now; status_ = 0;
						if (pipe == INVALID_HANDLE_VALUE || !send(pipe, 0, handshake(app))) disconnect();
					}
					if (pipe != INVALID_HANDLE_VALUE)
					{
						DWORD available{};
						if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) disconnect();
						else if (available >= 8)
						{
							unsigned int header[2]{}; DWORD peeked{};
							if (!PeekNamedPipe(pipe, header, 8, &peeked, nullptr, nullptr) || peeked != 8 || header[1] > 65536)
								disconnect();
							else if (available >= 8 + header[1])
							{
								std::string body(header[1], '\0');
								if (!transfer(pipe, header, 8, false) || !transfer(pipe, body.data(), header[1], false)) disconnect();
								else if (header[0] == 3) { if (!send(pipe, 4, body)) disconnect(); }
								else if (header[0] == 2) disconnect();
								else if (header[0] == 1)
								{
									rapidjson::Document message;
									message.Parse(body.data(), body.size());
									if (message.HasParseError() || !message.IsObject()) disconnect();
									else
									{
										const auto event = message.FindMember("evt");
										if (event != message.MemberEnd() && event->value.IsString())
										{
											if (!strcmp(event->value.GetString(), "READY")) status_ = 1;
											else if (!strcmp(event->value.GetString(), "ERROR"))
											{
												status_ = 3;
												const auto data = message.FindMember("data");
												if (data != message.MemberEnd() && data->value.IsObject())
												{
													const auto error = data->value.FindMember("message");
													if (error != data->value.MemberEnd() && error->value.IsString())
													{ std::lock_guard lock(mutex_); last_error_ = std::string(error->value.GetString()).substr(0, 256); }
												}
											}
										}
										const auto response = message.FindMember("nonce");
										const auto cmd = message.FindMember("cmd");
										if (status_ != 3 && response != message.MemberEnd() && response->value.IsString()
											&& nonce == response->value.GetString() && cmd != message.MemberEnd()
											&& cmd->value.IsString() && !strcmp(cmd->value.GetString(), "SET_ACTIVITY"))
										{ status_ = 2; nonce.clear(); }
									}
								}
							}
						}
						if (status_ == 3) { disconnect(); status_ = 3; }
						if (pipe != INVALID_HANDLE_VALUE)
						{
							const auto key = value.details + '\n' + value.state + '\n' + value.image;
							if (status_ > 0 && nonce.empty() && (key != last || now - sent >= 60s) && now - sent >= 15s)
							{
								nonce = std::to_string(++sequence); sent = now; last = key;
								if (!send(pipe, 1, activity(value, nonce, start))) disconnect();
							}
							if ((!status_ && now - connected > 10s) || (!nonce.empty() && now - sent > 10s)) disconnect();
						}
					}
					std::this_thread::sleep_for(100ms);
				}
			}
			catch (...) { status_ = 3; }
			if (pipe != INVALID_HANDLE_VALUE)
			{
				snapshot clear; send(pipe, 1, activity(clear, "shutdown", start)); CloseHandle(pipe);
			}
		}
	public:
		void post_load() override
		{
			worker_ = std::thread([this] { run(); });
			scheduler::once([this]
			{
				enabled_ = dvars::Dvar_RegisterBool("cl_discordRichPresence", 1, "Publish local Discord Rich Presence", game::dvar_flags::saved);
				application_ = dvars::Dvar_RegisterString("cl_discordApplicationId", "1469023860073693278", "Discord public application ID", game::dvar_flags::saved);
				image_ = dvars::Dvar_RegisterString("cl_discordImage", "consolation", "Discord uploaded large image key (empty disables)", game::dvar_flags::saved);
				command::add("discordRpcStatus", [this]
				{
					const char* states[]{"disconnected/disabled", "Discord READY", "activity acknowledged", "Discord error"};
					game::Com_Printf(0, "[Discord] %s\n", states[status_.load()]);
					std::lock_guard lock(mutex_);
					if (!last_error_.empty()) game::Com_Printf(0, "[Discord] Last error: %s\n", last_error_.c_str());
				});
			}, scheduler::main);
			scheduler::schedule([this]
			{
				if (stop_) return scheduler::cond_end;
				if (!enabled_ || !application_ || !image_) return scheduler::cond_continue;
				snapshot value; value.enabled = enabled_->current.enabled;
				value.app = application_->current.string ? application_->current.string : "";
				value.image = image_->current.string ? std::string(image_->current.string).substr(0, 128) : "";
				const auto* ingame = game::Dvar_FindVar("cl_ingame");
				value.details = ingame && ingame->current.enabled ? "Playing Quantum of Solace" : "In the menus";
				value.state = ingame && ingame->current.enabled ? text_dvar("mapname") + " / " + text_dvar("g_gametype") : "Project: Consolation";
				{ std::lock_guard lock(mutex_); pending_ = std::move(value); }
				return scheduler::cond_continue;
			}, scheduler::main, 1s);
		}
		void pre_destroy() override
		{
			stop_ = true;
			if (worker_.joinable()) worker_.join();
		}
	};
}

REGISTER_COMPONENT(discord_rpc::component)
