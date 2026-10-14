#pragma once
#include <winsock2.h>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace steam_transport
{
    // Private protocol, not an XNet structure. Ports are host order on this API.
    struct datagram { std::uint64_t lobby{}; std::uint16_t source{}, destination{}; std::span<const std::uint8_t> payload; };
    std::vector<std::uint8_t> encode(const datagram& packet);
    bool decode(std::span<const std::uint8_t> bytes, datagram& packet);
    class bridge
    {
        struct local_route { std::uint16_t port{}; ULONGLONG seen{}; };
        struct route { std::uint64_t peer{}; std::uint16_t remote_port{}, bound_port{}; SOCKET socket = INVALID_SOCKET; std::vector<local_route> locals; ULONGLONG used{}; };
        std::vector<route> routes_;
        std::vector<std::uint64_t> members_;
        std::uint64_t lobby_ = 0;
        std::uint16_t game_port_ = 1000;
        bool winsock_ = false;
        route* find(std::uint64_t peer, std::uint16_t remote_port);
    public:
        std::uint64_t sent = 0, received = 0, rejected = 0, failed = 0;
        ~bridge() { shutdown(); }
        bridge() = default;
        bridge(const bridge&) = delete;
        bridge& operator=(const bridge&) = delete;
        void configure(std::uint64_t lobby, std::uint16_t game_port, std::vector<std::uint64_t> members);
        std::uint16_t endpoint(std::uint64_t peer, std::uint16_t port);
        void pump(const std::function<bool(std::uint64_t, std::span<const std::uint8_t>)>& send);
        void receive(std::uint64_t peer, std::span<const std::uint8_t> bytes);
        void shutdown();
    };
}
