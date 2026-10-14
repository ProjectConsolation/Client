#pragma once
#include "offline_transport.hpp"

namespace steam_ingress
{
    // Client/shim-owned protocol, not an XLive ABI. SDK-owned member metadata
    // establishes the binding; neither a UDP header nor lobby-owner text does.
    struct identity { std::uint64_t steam{}; offline_transport::address native{}; };
    inline bool same(const offline_transport::address& a, const offline_transport::address& b)
    { return std::equal(a.begin() + 10, a.end(), b.begin() + 10); }
    inline bool same_xuid(const offline_transport::address& a, const offline_transport::address& b)
    { return std::equal(a.begin() + 16, a.begin() + 24, b.begin() + 16); }
    class policy
    {
        struct route { std::uint64_t steam; std::uint16_t port; offline_transport::address native; };
        std::vector<route> routes_;
        std::uint64_t lobby_ = 0;
    public:
        bool enabled = false;
        void configure(std::uint64_t lobby)
        {
            enabled = true; // Sticky: leaving Steam must not reopen a running UDP socket.
            if (lobby != lobby_) { routes_.clear(); lobby_ = lobby; }
        }
        bool authorize(std::uint64_t steam, std::uint16_t port, const offline_transport::address& native)
        {
            if (!enabled || !lobby_ || !steam || !port) return false;
            for (const auto& r : routes_)
            {
                if (r.port == port && r.steam != steam) return false;
                if (r.steam == steam && !same(r.native, native)) return false;
                if (r.steam != steam && (same(r.native, native) || same_xuid(r.native, native))) return false;
                if (r.port == port) return true;
            }
            if (routes_.size() >= 72) return false;
            routes_.push_back({steam, port, native});
            return true;
        }
        void revoke(std::uint16_t port)
        { std::erase_if(routes_, [port](const route& r) { return r.port == port; }); }
        bool accepts(std::uint32_t host_order_ip, std::uint16_t host_order_port,
                     const offline_transport::address* native) const
        {
            if (!enabled) return true; // Explicit offline/LAN startup retains its old protocol.
            if (!lobby_ || host_order_ip != 0x7f000001 || !native) return false;
            return std::any_of(routes_.begin(), routes_.end(), [&](const route& r) {
                return r.port == host_order_port && same(r.native, *native);
            });
        }
        bool matches_join(std::uint32_t ip, std::uint16_t port,
                          const offline_transport::address& native, std::uint64_t claimed_xuid) const
        {
            if (!enabled) return true;
            std::uint64_t expected = 0;
            // SDK XNADDR online-id starts at +16; this shim stores its stable
            // profile XUID there. QoS memberJoin reads the same eight-byte XUID.
            std::memcpy(&expected, native.data() + 16, sizeof(expected));
            return claimed_xuid && claimed_xuid == expected && accepts(ip, port, &native);
        }
    };
}
