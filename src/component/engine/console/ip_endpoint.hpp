#pragma once
#include <charconv>
#include <string_view>

namespace direct_connect
{
    // Split syntax only; InetPton validates the IPv4 part at the call site.
    inline bool parse_endpoint(std::string_view text, std::string_view legacy_port,
                               std::string_view& ip, unsigned int& port)
    {
        port = 1000;
        const auto colon = text.find(':');
        ip = text.substr(0, colon);
        if (ip.empty()) return false;
        auto value = legacy_port;
        if (colon != std::string_view::npos)
        {
            if (!legacy_port.empty()) return false;
            value = text.substr(colon + 1);
            if (value.empty()) return false;
        }
        if (value.empty()) return true;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), port);
        return result.ec == std::errc{} && result.ptr == value.data() + value.size()
            && port > 0 && port <= 65535;
    }
}
