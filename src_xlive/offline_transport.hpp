#pragma once

// Experimental, plaintext LAN test transport; NOT an XNet security protocol.
// Remove this adapter when the offline session transport is replaced. Engine
// datagrams remain unchanged inside the envelope. Both peers require matching shims.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace offline_transport
{
    // GFWL SDK winxnet.h: packed XNADDR (36 bytes) and XNKID (8 bytes).
    // QoS 1.1 10323410 compares the entire XNADDR when matching party members.
    using address = std::array<std::uint8_t, 36>;
    using session_id = std::array<std::uint8_t, 8>;
    constexpr std::array<std::uint8_t, 8> magic{'C','S','L','N','E','T','0','1'};
    constexpr std::size_t header_size = 56;
    constexpr std::size_t max_payload = 65507 - header_size;
    struct endpoint { std::uint32_t ip{}; std::uint16_t port{}; }; // network byte order
    struct peer
    {
        address advertised{};
        session_id session{};
        endpoint route{};
        bool learned{};
    };

    class peer_table
    {
        std::vector<peer> peers_;
    public:
        static constexpr std::size_t capacity = 64;
        // Return a stable 1-based handle. IP/port are routing data, not identity:
        // two processes on localhost must not collapse into the same peer.
        std::uint32_t remember(const address& value, const session_id& session,
                               endpoint route, bool learned)
        {
            const auto found = std::find_if(peers_.begin(), peers_.end(), [&](const peer& p) {
                return std::equal(value.begin() + 10, value.end(), p.advertised.begin() + 10);
            });
            if (found != peers_.end())
            {
                // Preserve the engine's registered record for reverse lookup;
                // headers may advertise loopback while connectIP normalized the IP.
                if (!learned) found->advertised = value;
                // Discovery/query envelopes have no session yet. Do not erase
                // a registered gameplay session when a browser refresh arrives.
                if (std::any_of(session.begin(), session.end(), [](auto b) { return b != 0; }))
                    found->session = session;
                if (learned || !found->learned) found->route = route;
                found->learned |= learned;
                return static_cast<std::uint32_t>(found - peers_.begin()) + 1;
            }
            if (peers_.size() == capacity) return 0;
            peers_.push_back({value, session, route, learned});
            return static_cast<std::uint32_t>(peers_.size());
        }
        const peer* get(std::uint32_t handle) const
        {
            return handle && handle <= peers_.size() ? &peers_[handle - 1] : nullptr;
        }
        std::size_t size() const { return peers_.size(); }
        void clear() { peers_.clear(); }
    };

    inline std::vector<std::uint8_t> encode(const address& sender, const session_id& session,
                                          std::span<const std::uint8_t> payload)
    {
        if (payload.empty() || payload.size() > max_payload) return {};
        std::vector<std::uint8_t> bytes(header_size + payload.size());
        std::copy(magic.begin(), magic.end(), bytes.begin());
        const auto length = static_cast<std::uint32_t>(payload.size());
        for (unsigned int i = 0; i < 4; ++i) bytes[8 + i] = static_cast<std::uint8_t>(length >> (i * 8));
        std::copy(sender.begin(), sender.end(), bytes.begin() + 12);
        std::copy(session.begin(), session.end(), bytes.begin() + 48);
        std::copy(payload.begin(), payload.end(), bytes.begin() + header_size);
        return bytes;
    }

    inline bool decode(std::span<const std::uint8_t> bytes, address& sender, session_id& session,
                       std::span<const std::uint8_t>& payload)
    {
        if (bytes.size() <= header_size || !std::equal(magic.begin(), magic.end(), bytes.begin())) return false;
        std::uint32_t length = 0;
        for (unsigned int i = 0; i < 4; ++i) length |= std::uint32_t(bytes[8 + i]) << (i * 8);
        if (length > max_payload || length != bytes.size() - header_size) return false;
        std::copy_n(bytes.begin() + 12, sender.size(), sender.begin());
        std::copy_n(bytes.begin() + 48, session.size(), session.begin());
        // Reject anonymous envelopes; this is structural validation, not auth.
        if (std::all_of(sender.begin() + 10, sender.end(), [](auto b) { return b == 0; })) return false;
        payload = bytes.subspan(header_size);
        return true;
    }
}
