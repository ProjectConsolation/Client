#include "../src_xlive/offline_transport.hpp"
#include "../src/component/engine/console/ip_endpoint.hpp"
#include <cassert>
#include <cstdio>

int main()
{
    using namespace offline_transport;
    std::string_view ip;
    unsigned int port;
    assert(direct_connect::parse_endpoint("127.0.0.1:1000", "", ip, port) && ip == "127.0.0.1" && port == 1000);
    assert(direct_connect::parse_endpoint("127.0.0.1", "", ip, port) && port == 1000);
    assert(direct_connect::parse_endpoint("127.0.0.1", "1002", ip, port) && port == 1002);
    for (auto bad : {"127.0.0.1:", ":1000", "127.0.0.1:0", "127.0.0.1:65536", "127.0.0.1:-1", "127.0.0.1:1000:1", "127.0.0.1:1junk"})
        assert(!direct_connect::parse_endpoint(bad, "", ip, port));
    assert(!direct_connect::parse_endpoint("127.0.0.1:1000", "1002", ip, port));
    address first{}, second{};
    first[16] = 1; second[16] = 2;
    session_id session{}; session[0] = 7;
    const std::vector<std::uint8_t> data{0xff, 0, 3, 4, 0};
    auto bytes = encode(first, session, data);
    address decoded{}; session_id decoded_session{};
    std::span<const std::uint8_t> payload;
    assert(decode(bytes, decoded, decoded_session, payload));
    assert(decoded == first && decoded_session == session);
    assert(std::equal(payload.begin(), payload.end(), data.begin(), data.end()));
    for (std::size_t length = 0; length < bytes.size(); ++length)
        assert(!decode(std::span(bytes).first(length), decoded, decoded_session, payload));
    bytes[8]++;
    assert(!decode(bytes, decoded, decoded_session, payload));
    assert(!decode(encode(address{}, session, data), decoded, decoded_session, payload));
    assert(encode(first, session, {}).empty());
    assert(encode(first, session, std::vector<std::uint8_t>(max_payload + 1)).empty());
    assert(decode(encode(first, session, std::vector<std::uint8_t>(max_payload)), decoded, decoded_session, payload));
    peer_table table;
    const auto a = table.remember(first, session, {1, 1000}, false);
    const auto b = table.remember(second, session, {1, 1001}, false);
    assert(a != b && table.size() == 2);
    auto normalized = first; normalized[0] = 9;
    assert(table.remember(normalized, session, {2, 1002}, true) == a);
    assert(table.get(a)->advertised == first);
    assert(table.remember(first, session, {1, 1000}, false) == a);
    assert(table.get(a)->route.ip == 2 && table.get(a)->route.port == 1002);
    assert(table.remember(first, {}, {2, 1002}, true) == a);
    assert(table.get(a)->session == session);
    assert(!table.get(0) && !table.get(65));
    for (unsigned int i = 3; i <= 64; ++i) {
        auto next = first; next[16] = static_cast<std::uint8_t>(i);
        assert(table.remember(next, session, {1, 1000}, false) == i);
    }
    auto next = first; next[16] = 65;
    assert(table.remember(next, session, {1, 1000}, false) == 0);
    table.clear(); assert(table.size() == 0 && !table.get(a));
    std::puts("Offline transport framing and peer identity tests passed");
}
