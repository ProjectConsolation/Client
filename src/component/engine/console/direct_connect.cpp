#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/game.hpp"
#include "command.hpp"
#include "console.hpp"
#include "component/utils/scheduler.hpp"

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
            if (!parse_number(get("protocol"), protocol) || protocol != 47
                || !parse_words(get("xnaddr"), address) || !parse_words(get("xnkey"), key)
                || !parse_words(get("xnkid"), id) || !parse_words(get("nonce"), nonce)
                || !parse_number(get("pslots"), slots) || !parse_number(get("prslots"), private_slots)
                || slots == 0 || slots > 18 || private_slots > slots)
            {
                console::error("[connect_direct] host returned invalid or missing QoS session information\n");
                return false;
            }

            // QoS 1.1: 0x102F91D0 emits nine DWORDs of XNADDR, four of XNKEY,
            // two of XNKID and two of nonce. 0x103115E0 parses that format and
            // calls 0x10311330 with XSESSION_INFO and total/private slot counts.
            // Use the responding IP because the offline host can advertise loopback.
            address[0] = source.sin_addr.s_addr;
            address[1] = source.sin_addr.s_addr;
            std::array<unsigned int, 15> session{};
            std::copy(id.begin(), id.end(), session.begin());
            std::copy(address.begin(), address.end(), session.begin() + 2);
            std::copy(key.begin(), key.end(), session.begin() + 11);

            const auto entry = game::game_offset(0x10311330);
            constexpr unsigned char expected[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x51, 0xA1};
            if (memcmp(reinterpret_cast<const void*>(entry), expected, sizeof(expected)) != 0)
            {
                console::error("[connect_direct] unsupported QoS connection routine\n");
                return false;
            }

            using native_connect = unsigned int(__cdecl*)(int, const void*, unsigned int,
                unsigned int, int, int);
            reinterpret_cast<native_connect>(entry)(0, session.data(), nonce[0], nonce[1],
                static_cast<int>(slots), static_cast<int>(private_slots));
            // Native connect selects port 1000 when unset. Preserve an explicit
            // endpoint after it initializes the remote netadr (0x111F460C).
            *reinterpret_cast<unsigned short*>(game::game_offset(0x111F4614)) = source.sin_port;
            console::info("[connect_direct] session passed to native connection; waiting for host\n");
            return true;
        }

        void poll_query()
        {
            if (!pending) return;
            const auto now = GetTickCount64();
            if (now - pending->started >= 5000)
            {
                console::error("[connect_direct] timed out waiting for host session information\n");
                pending.reset();
                return;
            }

            if (pending->last_send == 0 || now - pending->last_send >= 500)
            {
                const auto request = std::string(4, '\xFF') + "getinfo " + pending->challenge;
                const auto result = sendto(pending->socket, request.c_str(), static_cast<int>(request.size() + 1),
                    0, reinterpret_cast<const sockaddr*>(&pending->target), sizeof(pending->target));
                if (result == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
                {
                    console::error("[connect_direct] query send failed: %d\n", WSAGetLastError());
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
                    console::error("[connect_direct] query receive failed: %d\n", error);
                    pending.reset();
                    return;
                }
                if (source_size != sizeof(source) || source.sin_family != AF_INET
                    || source.sin_addr.s_addr != pending->target.sin_addr.s_addr
                    || source.sin_port != pending->target.sin_port) continue;
                constexpr std::string_view prefix = "\xFF\xFF\xFF\xFF" "infoResponse\n";
                std::string_view packet(buffer.data(), static_cast<std::size_t>(size));
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
                console::info("[connect_direct] query cancelled\n");
                return;
            }
            if (args.size() < 2 || args.size() > 3)
            {
                console::info("connect_direct <IPv4> [port] (default 1000); connect_direct cancel\n");
                return;
            }
            auto query = std::make_unique<pending_query>();
            query->target.sin_family = AF_INET;
            unsigned int port = 1000;
            if (InetPtonA(AF_INET, args[1], &query->target.sin_addr) != 1
                || query->target.sin_addr.s_addr == INADDR_ANY
                || query->target.sin_addr.s_addr == INADDR_BROADCAST
                || (args.size() == 3 && (!parse_number(args[2], port) || port == 0 || port > 65535)))
            {
                console::error("[connect_direct] specify an IPv4 address and port 1-65535\n");
                return;
            }
            query->target.sin_port = htons(static_cast<unsigned short>(port));
            query->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            u_long nonblocking = 1;
            if (query->socket == INVALID_SOCKET || ioctlsocket(query->socket, FIONBIO, &nonblocking))
            {
                console::error("[connect_direct] cannot open query socket: %d\n", WSAGetLastError());
                return;
            }
            query->started = GetTickCount64();
            query->challenge = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(query->started);
            pending = std::move(query);
            console::info("[connect_direct] querying %s:%u\n", args[1], port);
        }
    }

    class component final : public component_interface
    {
    public:
        void post_load() override
        {
            scheduler::once([] { command::add("connect_direct", start_query); }, scheduler::main);
            scheduler::loop(poll_query, scheduler::main, 50ms);
        }

        void pre_destroy() override
        {
            pending.reset();
        }
    };
}

REGISTER_COMPONENT(direct_connect::component)
