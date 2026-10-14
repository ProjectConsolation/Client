#include <std_include.hpp>
#include "steam_transport.hpp"
#include <algorithm>
#include <array>
#include <cstring>

namespace steam_transport
{
    // QoS PC 1.1 sends game/discovery UDP through 10244420/10244360 and
    // receives through 102447E0 (GFWL SDK XSocketSendTo/RecvFrom ordinals
    // 24/20). KisakCOD win_net.cpp confirms datagram boundaries and native
    // sequence/fragment ownership. No native packets or XNet ABI are replaced.
    namespace
    {
        constexpr std::array<std::uint8_t, 8> magic{'C','S','L','P','2','P','0','1'};
        constexpr std::size_t header = 24, max_payload = 65507;
        constexpr std::size_t max_routes = 72; // 18 members, at most four routes each.
        using configure_fn = BOOL(WINAPI*)(std::uint64_t);
        using authorize_fn = BOOL(WINAPI*)(std::uint64_t, WORD, const void*);
        using revoke_fn = void(WINAPI*)(WORD);
        template<class T> T shim(const char* name)
        { return reinterpret_cast<T>(GetProcAddress(GetModuleHandleA("xlive.dll"), name)); }
        void revoke(WORD port)
        { if (const auto fn = shim<revoke_fn>("ConsolationSteamIngressRevoke")) fn(port); }
    }
    std::vector<std::uint8_t> encode(const datagram& packet)
    {
        if (!packet.lobby || !packet.source || !packet.destination || packet.payload.empty() || packet.payload.size() > max_payload) return {};
        std::vector<std::uint8_t> out(header + packet.payload.size());
        std::copy(magic.begin(), magic.end(), out.begin());
        std::memcpy(out.data() + 8, &packet.lobby, 8);
        std::memcpy(out.data() + 16, &packet.source, 2);
        std::memcpy(out.data() + 18, &packet.destination, 2);
        const auto size = static_cast<std::uint32_t>(packet.payload.size());
        std::memcpy(out.data() + 20, &size, 4);
        std::memcpy(out.data() + header, packet.payload.data(), size);
        return out;
    }
    bool decode(const std::span<const std::uint8_t> bytes, datagram& packet)
    {
        if (bytes.size() <= header || bytes.size() > header + max_payload || !std::equal(magic.begin(), magic.end(), bytes.begin())) return false;
        std::uint32_t size = 0;
        datagram parsed{};
        std::memcpy(&parsed.lobby, bytes.data() + 8, 8);
        std::memcpy(&parsed.source, bytes.data() + 16, 2);
        std::memcpy(&parsed.destination, bytes.data() + 18, 2);
        std::memcpy(&size, bytes.data() + 20, 4);
        if (!parsed.lobby || !parsed.source || !parsed.destination || size != bytes.size() - header) return false;
        parsed.payload = bytes.subspan(header);
        packet = parsed;
        return true;
    }
    bridge::route* bridge::find(const std::uint64_t peer, const std::uint16_t remote_port)
    {
        if (!lobby_ || !remote_port || (self_ != host_ && peer != host_) || std::find(members_.begin(), members_.end(), peer) == members_.end()) return nullptr;
        const auto identity = std::find_if(identities_.begin(), identities_.end(), [peer](const auto& id) { return id.steam == peer; });
        if (identity == identities_.end()) return nullptr;
        auto found = std::find_if(routes_.begin(), routes_.end(), [&](const route& r) { return r.peer == peer && r.remote_port == remote_port; });
        if (found != routes_.end()) { found->used = GetTickCount64(); return &*found; }
        if (routes_.size() >= max_routes) return nullptr;
        if (std::count_if(routes_.begin(), routes_.end(), [&](const route& r) { return r.peer == peer; }) >= 4) return nullptr;
        if (!winsock_)
        {
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data)) return nullptr;
            winsock_ = true;
        }
        const auto socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket == INVALID_SOCKET) return nullptr;
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        u_long nonblocking = 1;
        int size = sizeof(address);
        if (bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ||
            ioctlsocket(socket, FIONBIO, &nonblocking) || getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size))
        { closesocket(socket); return nullptr; }
        const auto authorize = shim<authorize_fn>("ConsolationSteamIngressAuthorize");
        if (!authorize || !authorize(peer, ntohs(address.sin_port), identity->native.data()))
        { closesocket(socket); return nullptr; }
        routes_.push_back({peer, remote_port, ntohs(address.sin_port), socket, {}, GetTickCount64()});
        return &routes_.back();
    }
    bool bridge::secure_ingress(const std::uint64_t lobby)
    {
        const auto version = shim<DWORD(WINAPI*)()>("ConsolationSteamIngressVersion");
        const auto configure = shim<configure_fn>("ConsolationSteamIngressConfigure");
        return version && version() == 1 && configure &&
            shim<authorize_fn>("ConsolationSteamIngressAuthorize") && shim<revoke_fn>("ConsolationSteamIngressRevoke") &&
            shim<BOOL(WINAPI*)(std::uint64_t, std::uint64_t)>("ConsolationSteamValidateJoin") && configure(lobby);
    }
    bool bridge::configure(const std::uint64_t lobby, const std::uint16_t game_port, std::vector<std::uint64_t> members,
                           const std::uint64_t self, const std::uint64_t host, std::vector<steam_ingress::identity> identities)
    {
        if (lobby != lobby_) shutdown();
        if (!secure_ingress(lobby)) { shutdown(); return false; }
        lobby_ = lobby; game_port_ = game_port ? game_port : 1000;
        self_ = self; host_ = host;
        if (members.size() > 18) members.resize(18);
        members_ = std::move(members);
        // Metadata is read by SteamID, not a claimed packet identity. Bind once,
        // reject later edits and collisions (including with the local host).
        identities_.clear();
        if (identities.size() > 18) identities.resize(18);
        for (const auto& id : identities)
        {
            const auto prior = std::find_if(pinned_.begin(), pinned_.end(), [&](const auto& p) { return p.steam == id.steam; });
            if (!id.steam || (prior != pinned_.end() && !steam_ingress::same(prior->native, id.native))) continue;
            if (std::any_of(identities.begin(), identities.end(), [&](const auto& p) {
                return p.steam != id.steam && (steam_ingress::same(p.native, id.native) || steam_ingress::same_xuid(p.native, id.native));
            })) continue;
            if (std::any_of(pinned_.begin(), pinned_.end(), [&](const auto& p) {
                return p.steam != id.steam && (steam_ingress::same(p.native, id.native) || steam_ingress::same_xuid(p.native, id.native));
            })) continue;
            if (prior == pinned_.end()) { if (pinned_.size() >= 18) continue; pinned_.push_back(id); }
            identities_.push_back(id);
        }
        for (auto it = routes_.begin(); it != routes_.end();)
        {
            if ((self_ != host_ && it->peer != host_) ||
                std::none_of(identities_.begin(), identities_.end(), [&](const auto& id) { return id.steam == it->peer; }) ||
                std::find(members_.begin(), members_.end(), it->peer) == members_.end() || GetTickCount64() - it->used > 60000)
            { revoke(it->bound_port); closesocket(it->socket); it = routes_.erase(it); }
            else ++it;
        }
        return true;
    }
    std::uint16_t bridge::endpoint(const std::uint64_t peer, const std::uint16_t port)
    {
        const auto* route = find(peer, port);
        return route ? route->bound_port : 0;
    }
    void bridge::pump(const std::function<bool(std::uint64_t, std::span<const std::uint8_t>)>& send)
    {
        std::array<std::uint8_t, max_payload> bytes{};
        std::size_t budget = 128;
        const auto now = GetTickCount64();
        for (auto& route : routes_)
        {
            while (budget)
            {
                sockaddr_in source{}; int size = sizeof(source);
                const int count = recvfrom(route.socket, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
                    reinterpret_cast<sockaddr*>(&source), &size);
                if (count == SOCKET_ERROR) break;
                --budget;
                route.used = now;
                if (count <= 0 || source.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) { ++rejected; continue; }
                const auto port = ntohs(source.sin_port);
                auto local = std::find_if(route.locals.begin(), route.locals.end(), [&](const local_route& r) { return r.port == port; });
                if (local != route.locals.end()) local->seen = now;
                else
                {
                    std::erase_if(route.locals, [&](const local_route& r) { return now - r.seen > 15000; });
                    if (route.locals.size() >= 4) { ++rejected; continue; }
                    route.locals.push_back({port, now});
                }
                const auto frame = encode({lobby_, port, route.remote_port, {bytes.data(), static_cast<std::size_t>(count)}});
                if (send(route.peer, frame)) ++sent;
                else ++failed;
            }
        }
    }
    void bridge::receive(const std::uint64_t peer, const std::span<const std::uint8_t> bytes)
    {
        datagram packet{};
        if (!decode(bytes, packet) || packet.lobby != lobby_ ||
            (self_ != host_ && peer != host_) ||
            std::find(members_.begin(), members_.end(), peer) == members_.end()) { ++rejected; return; }
        offline_transport::address native{}; offline_transport::session_id session{};
        std::span<const std::uint8_t> payload;
        const auto identity = std::find_if(identities_.begin(), identities_.end(), [peer](const auto& id) { return id.steam == peer; });
        if (identity == identities_.end() || !offline_transport::decode(packet.payload, native, session, payload)
            || !steam_ingress::same(native, identity->native)) { ++rejected; return; }
        const auto now = GetTickCount64();
        // A peer may reach the game's socket or an ephemeral query socket that
        // actually sent to that peer recently. Never expose arbitrary localhost ports.
        bool allowed = packet.destination == game_port_;
        for (const auto& route : routes_) if (route.peer == peer)
            for (const auto& local : route.locals)
                allowed |= local.port == packet.destination && now - local.seen <= 15000;
        if (!allowed) { ++rejected; return; }
        auto* route = find(peer, packet.source);
        if (!route) { ++rejected; return; }
        sockaddr_in destination{}; destination.sin_family = AF_INET;
        destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK); destination.sin_port = htons(packet.destination);
        const auto count = sendto(route->socket, reinterpret_cast<const char*>(packet.payload.data()), static_cast<int>(packet.payload.size()), 0,
            reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
        if (count == static_cast<int>(packet.payload.size())) ++received;
        else ++failed;
    }
    void bridge::shutdown()
    {
        for (const auto& route : routes_) { revoke(route.bound_port); closesocket(route.socket); }
        routes_.clear(); members_.clear(); identities_.clear(); pinned_.clear(); lobby_ = 0;
        // Never fall back to LAN ingress on teardown of an active Steam match.
        secure_ingress(0);
        if (winsock_) WSACleanup();
        winsock_ = false;
    }
}
