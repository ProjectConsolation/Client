#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/game.hpp"
#include "game/protocol.hpp"
#include "command.hpp"
#include "console.hpp"
#include "ip_endpoint.hpp"
#include "direct_connect.hpp"
#include "game/dvars.hpp"
#include <utils/hook.hpp>
#include "../../../../src_xlive/offline_transport.hpp"
#include "component/utils/scheduler.hpp"
#include "component/engine/patches/xlive.hpp"
#include <utils/flags.hpp>

#include <array>
#include <charconv>
#include <memory>
#include <string_view>

namespace direct_connect
{
    namespace
    {
        struct pending_query
        {
            SOCKET socket = INVALID_SOCKET;
            sockaddr_in target{};
            std::string challenge;
            ULONGLONG started = 0;
            ULONGLONG last_send = 0;
            offline_transport::address sender{};

            ~pending_query()
            {
                if (socket != INVALID_SOCKET) closesocket(socket);
            }
        };

        std::unique_ptr<pending_query> pending;

        bool parse_number(const std::string_view text, unsigned int& value, const int base = 10)
        {
            if (text.empty()) return false;
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
            return result.ec == std::errc{} && result.ptr == text.data() + text.size();
        }

        template <std::size_t N>
        bool parse_words(const std::string_view text, std::array<unsigned int, N>& words)
        {
            if (text.size() != N * 8) return false;
            for (std::size_t i = 0; i < N; ++i)
            {
                if (!parse_number(text.substr(i * 8, 8), words[i], 16)) return false;
            }
            return true;
        }

        bool parse_info(std::string_view text, std::map<std::string, std::string>& fields)
        {
            if (!text.empty() && text.back() == '\0') text.remove_suffix(1);
            if (text.empty() || text.front() != '\\') return false;
            text.remove_prefix(1);
            while (!text.empty())
            {
                const auto separator = text.find('\\');
                if (separator == std::string_view::npos || separator == 0) return false;
                const std::string key(text.substr(0, separator));
                text.remove_prefix(separator + 1);
                const auto end = text.find('\\');
                const auto value = text.substr(0, end);
                if (key.find('\0') != std::string::npos || value.find('\0') != std::string_view::npos
                    || !fields.emplace(key, value).second) return false;
                if (end == std::string_view::npos) break;
                text.remove_prefix(end + 1);
            }
            return true;
        }

        bool join_response(const std::map<std::string, std::string>& fields, const sockaddr_in& source)
        {
            const auto get = [&fields](const char* key) -> std::string_view
            {
                const auto it = fields.find(key);
                return it == fields.end() ? std::string_view{} : it->second;
            };

            std::array<unsigned int, 9> address{};
            std::array<unsigned int, 4> key{};
            std::array<unsigned int, 2> id{}, nonce{};
            unsigned int slots = 0, private_slots = 0, protocol = 0;
            if (!parse_number(get("protocol"), protocol) || protocol != game::consolation_protocol
                || !parse_words(get("xnaddr"), address) || !parse_words(get("xnkey"), key)
                || !parse_words(get("xnkid"), id) || !parse_words(get("nonce"), nonce)
                || !parse_number(get("pslots"), slots) || !parse_number(get("prslots"), private_slots)
                || slots == 0 || slots > 18 || private_slots > slots)
            {
                console::error("[connectIP] host returned invalid or missing QoS session information\n");
                return false;
            }

            // QoS 1.1: 0x102F91D0 emits nine DWORDs of XNADDR, four of XNKEY,
            // two of XNKID and two of nonce. 0x103115E0 parses that format and
            // calls 0x10311330 with XSESSION_INFO and total/private slot counts.
            // Use the responding IP because the offline host can advertise loopback.
            address[0] = source.sin_addr.s_addr;
            address[1] = source.sin_addr.s_addr;
            // SDK XNADDR.wPortOnline is at byte 8, in network byte order.
            // Register the queried endpoint, not the host's default 3074 port.
            memcpy(reinterpret_cast<unsigned char*>(address.data()) + 8, &source.sin_port, sizeof(source.sin_port));
            std::array<unsigned int, 15> session{};
            std::copy(id.begin(), id.end(), session.begin());
            std::copy(address.begin(), address.end(), session.begin() + 2);
            std::copy(key.begin(), key.end(), session.begin() + 11);

            const auto entry = game::game_offset(0x10311330);
            constexpr unsigned char expected[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x51, 0xA1};
            if (memcmp(reinterpret_cast<const void*>(entry), expected, sizeof(expected)) != 0)
            {
                console::error("[connectIP] unsupported QoS connection routine\n");
                return false;
            }

            using native_connect = unsigned int(__cdecl*)(int, const void*, unsigned int,
                unsigned int, int, int);
            reinterpret_cast<native_connect>(entry)(0, session.data(), nonce[0], nonce[1],
                static_cast<int>(slots), static_cast<int>(private_slots));
            // Native connect selects port 1000 when unset. Preserve an explicit
            // endpoint after it initializes the remote netadr (0x111F460C).
            *reinterpret_cast<unsigned short*>(game::game_offset(0x111F4614)) = source.sin_port;
            console::info("[connectIP] session passed to native connection; waiting for host\n");
            return true;
        }

        void poll_query()
        {
            if (!pending) return;
            const auto now = GetTickCount64();
            if (now - pending->started >= 5000)
            {
                console::error("[connectIP] timed out waiting for host session information\n");
                pending.reset();
                return;
            }

            if (pending->last_send == 0 || now - pending->last_send >= 500)
            {
                // QoS 10244480: VDP plaintext OOB has a two-byte zero header.
                // The envelope lets the host learn this ephemeral reply route;
                // its native send routine otherwise always selects port 1000.
                const auto request = std::string(2, '\0') + std::string(4, '\xFF') + "getinfo " + pending->challenge + '\0';
                const auto wire = offline_transport::encode(pending->sender, {},
                    {reinterpret_cast<const std::uint8_t*>(request.data()), request.size()});
                const auto result = sendto(pending->socket, reinterpret_cast<const char*>(wire.data()), static_cast<int>(wire.size()),
                    0, reinterpret_cast<const sockaddr*>(&pending->target), sizeof(pending->target));
                if (result == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
                {
                    console::error("[connectIP] query send failed: %d\n", WSAGetLastError());
                    pending.reset();
                    return;
                }
                pending->last_send = now;
            }

            for (unsigned int i = 0; pending && i < 8; ++i)
            {
                std::array<char, 2048> buffer{};
                sockaddr_in source{};
                int source_size = sizeof(source);
                const auto size = recvfrom(pending->socket, buffer.data(), static_cast<int>(buffer.size()),
                    0, reinterpret_cast<sockaddr*>(&source), &source_size);
                if (size == SOCKET_ERROR)
                {
                    const auto error = WSAGetLastError();
                    if (error == WSAEWOULDBLOCK) return;
                    if (error == WSAEMSGSIZE || error == WSAECONNRESET) continue;
                    console::error("[connectIP] query receive failed: %d\n", error);
                    pending.reset();
                    return;
                }
                if (source_size != sizeof(source) || source.sin_family != AF_INET
                    || source.sin_addr.s_addr != pending->target.sin_addr.s_addr
                    || source.sin_port != pending->target.sin_port) continue;
                constexpr std::string_view prefix = "\xFF\xFF\xFF\xFF" "infoResponse\n";
                offline_transport::address sender{};
                offline_transport::session_id session{};
                std::span<const std::uint8_t> payload;
                if (!offline_transport::decode({reinterpret_cast<const std::uint8_t*>(buffer.data()),
                    static_cast<std::size_t>(size)}, sender, session, payload)) continue;
                if (payload.size() < 2 || payload[0] || payload[1]) continue;
                std::string_view packet(reinterpret_cast<const char*>(payload.data() + 2), payload.size() - 2);
                if (!packet.starts_with(prefix)) continue;
                packet.remove_prefix(prefix.size());
                std::map<std::string, std::string> fields;
                if (!parse_info(packet, fields)) continue;
                const auto challenge = fields.find("challenge");
                if (challenge == fields.end() || challenge->second != pending->challenge) continue;
                pending.reset();
                join_response(fields, source);
            }
        }

        void start_query(const command::params& args)
        {
            if (args.size() == 2 && std::string_view(args[1]) == "cancel")
            {
                pending.reset();
                console::info("[connectIP] query cancelled\n");
                return;
            }
            if (args.size() < 2 || args.size() > 3)
            {
                console::info("connectIP <IPv4>:<port> (default port 1000); connectIP cancel\n");
                return;
            }
            const bool offline = utils::flags::has_flag("offline")
                || utils::flags::has_flag("local_offline") || utils::flags::has_flag("local-offline");
            if (offline && !xlive::is_local_shim_loaded())
            {
                console::error("[connectIP] offline connections require Consolation's local xlive.dll shim, not Microsoft's runtime\n");
                return;
            }
            if (xlive::is_local_shim_loaded())
            {
                // GFWL SDK xbox.h: XUserGetXUID(DWORD, PXUID), WINAPI.
                // QoS imports ordinal 5261; use the shim's same signed-in identity
                // as the native session/party path, never a second synthetic GUID.
                using get_xuid = DWORD(WINAPI*)(DWORD, std::uint64_t*);
                const auto function = reinterpret_cast<get_xuid>(GetProcAddress(
                    GetModuleHandleA("xlive.dll"), MAKEINTRESOURCEA(5261)));
                std::uint64_t identity = 0;
                if (!function || function(0, &identity) != ERROR_SUCCESS || !identity)
                {
                    console::error("[connectIP] no valid local offline identity; refusing to start a session\n");
                    return;
                }
            }
            auto query = std::make_unique<pending_query>();
            using transport_version = DWORD(WINAPI*)();
            using title_address = DWORD(WINAPI*)(void*);
            const auto shim = GetModuleHandleA("xlive.dll");
            const auto version = xlive::is_local_shim_loaded() ? reinterpret_cast<transport_version>(
                GetProcAddress(shim, "ConsolationOfflineTransportVersion")) : nullptr;
            const auto get_address = reinterpret_cast<title_address>(GetProcAddress(shim, MAKEINTRESOURCEA(73)));
            if (!version || version() != 1 || !get_address || !get_address(query->sender.data()))
            {
                console::error("[connectIP] rebuild/deploy the matching Consolation LAN shim on both peers\n");
                return;
            }
            query->target.sin_family = AF_INET;
            unsigned int port = 1000;
            std::string_view ip;
            if (!parse_endpoint(args[1], args.size() == 3 ? args[2] : "", ip, port)
                || InetPtonA(AF_INET, std::string(ip).c_str(), &query->target.sin_addr) != 1
                || query->target.sin_addr.s_addr == INADDR_ANY
                || query->target.sin_addr.s_addr == INADDR_BROADCAST)
            {
                console::error("[connectIP] specify an IPv4 address and port 1-65535\n");
                return;
            }
            query->target.sin_port = htons(static_cast<unsigned short>(port));
            query->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            u_long nonblocking = 1;
            if (query->socket == INVALID_SOCKET || ioctlsocket(query->socket, FIONBIO, &nonblocking))
            {
                console::error("[connectIP] cannot open query socket: %d\n", WSAGetLastError());
                return;
            }
            query->started = GetTickCount64();
            query->challenge = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(query->started);
            pending = std::move(query);
            console::info("[connectIP] querying %.*s:%u\n", static_cast<int>(ip.size()), ip.data(), port);
        }
    }

    bool prepare_dedicated_launch()
    {
        if (!utils::flags::has_flag("dedicated")) return true;
        static bool requested = false;
        static bool ready = false;
        if (ready) return true;
        auto* dedicated = game::Dvar_FindVar("dedicated");
        auto* systemlink = game::Dvar_FindVar("systemlink");
        auto* online = game::Dvar_FindVar("onlinegame");
        const auto* running = game::Dvar_FindVar("sv_running");
        using transport_version = DWORD(WINAPI*)();
        const auto shim = GetModuleHandleA("xlive.dll");
        const auto version = xlive::is_local_shim_loaded() ? reinterpret_cast<transport_version>(
            GetProcAddress(shim, "ConsolationOfflineTransportVersion")) : nullptr;
        const auto call = game::game_offset(0x102F7084);
        constexpr unsigned char expected[] = {0xE8, 0x57, 0x2A, 0xF5, 0xFF};
        if (!version || version() != 1 || !dedicated || !systemlink || !online || !running
            || static_cast<int>(dedicated->type) != game::DVAR_TYPE_INT
            || static_cast<int>(systemlink->type) != game::DVAR_TYPE_BOOL
            || static_cast<int>(online->type) != game::DVAR_TYPE_BOOL
            || memcmp(reinterpret_cast<const void*>(call), expected, sizeof(expected)))
        {
            console::error("[dedicated] local shim/native startup prerequisites missing; map commands not forwarded\n");
            return false;
        }
        if (!requested)
        {
            if (running->current.enabled || dedicated->current.integer
                || (dedicated->flags & game::dvar_flags::read_only))
            {
                console::error("[dedicated] expected initialized playable startup with no running server; refusing transition\n");
                return false;
            }
            // QoS Com_CheckDedicated (103F7470) applies the native latch,
            // shuts down local client state (1031E980), and installs server chat.
            // Keep normal initial registration: cold dedicated init skips
            // CL_Init, leaving the session gate dvar pointers uninitialized.
            dedicated->latched.integer = 1;
            dedicated->modified = true;
            requested = true;
            return false;
        }
        if (!dedicated->current.integer)
        {
            console::error("[dedicated] native dedicated latch was not applied; refusing map startup\n");
            return false;
        }
        dvars::replace_dvar(dvars::make_bool("systemlink", "Current game is a system link game", true,
            game::dvar_flags::none), false, false);
        dvars::replace_dvar(dvars::make_bool("onlinegame", "Current game is an online game", false,
            game::dvar_flags::none), false, false);
        // SV_SpawnServer creates/processes its real System Link host session.
        // Suppress only its local membership call, retaining caller stack cleanup.
        // QoS 1.1, rollback: restore expected bytes; runtime proof: zero local
        // roster entry, native active session and remote gameplay after map load.
        utils::hook::nop(call, sizeof(expected));
        if (const auto window = *game::main_window; window && IsWindow(window)) ShowWindow(window, SW_HIDE);
        ready = true;
        console::info("[dedicated] native dedicated processing enabled; no local session member (startup still initializes graphics)\n");
        return true;
    }

    class component final : public component_interface
    {
    public:
        void post_load() override
        {
            scheduler::once([] {
                command::add("connectIP", start_query);
                command::add("connect_direct", start_query); // compatibility for existing configs
                command::add("offlineNetStatus", [] {
                    using status_function = DWORD(WINAPI*)(char*, DWORD);
                    const auto module = GetModuleHandleA("xlive.dll");
                    const auto status = xlive::is_local_shim_loaded() ? reinterpret_cast<status_function>(
                        GetProcAddress(module, "ConsolationOfflineNetStatus")) : nullptr;
                    std::array<char, 2048> text{};
                    if (!status || status(text.data(), static_cast<DWORD>(text.size())) != ERROR_SUCCESS)
                        console::error("[offlineNetStatus] matching local test shim not loaded\n");
                    else {
                        console::info("[offlineNetStatus] %s\n", text.data());
                        const auto* dedicated = game::Dvar_FindVar("dedicated");
                        const auto* running = game::Dvar_FindVar("sv_running");
                        console::info("[offlineNetStatus] dedicated=%d sv_running=%d native_session_handle=%d native_session_active=%d\n",
                            dedicated ? dedicated->current.integer : -1,
                            running ? static_cast<int>(running->current.enabled) : -1,
                            *reinterpret_cast<const std::uint32_t*>(game::game_offset(0x115F607C)) != 0,
                            *reinterpret_cast<const unsigned char*>(game::game_offset(0x115F60BC)) != 0);
                    }
                });
            }, scheduler::main);
            scheduler::loop(poll_query, scheduler::main, 50ms);
        }

        void pre_destroy() override
        {
            pending.reset();
        }
    };
}

REGISTER_COMPONENT(direct_connect::component)
