#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace direct_connect
{
    // QoS PC 1.1: 10244600 passes the entire OOB message as plaintext to
    // 10244480, which prefixes its length as a little-endian WORD. This is
    // title VDP framing, not the CSLNET01 envelope or COD4's bare UDP OOB.
    inline std::string frame_query_oob(std::string_view message)
    {
        if (message.empty() || message.size() > 65535) return {};
        std::string packet(2, '\0');
        packet[0] = static_cast<char>(message.size() & 255);
        packet[1] = static_cast<char>((message.size() >> 8) & 255);
        packet.append(message);
        return packet;
    }

    inline bool query_oob_payload(std::span<const std::uint8_t> packet, std::string_view& message)
    {
        message = {};
        if (packet.size() < 3) return false;
        const auto plaintext_size = static_cast<std::size_t>(packet[0])
            | (static_cast<std::size_t>(packet[1]) << 8);
        if (plaintext_size != packet.size() - 2) return false;
        message = {reinterpret_cast<const char*>(packet.data() + 2), plaintext_size};
        return true;
    }
}
