#include "std_include.hpp"
#include "loader/component_loader.hpp"
#include "game/game.hpp"
#include "engine/console/command.hpp"
#include "engine/console/console.hpp"
#include "engine/patches/xlive.hpp"
#include "utils/scheduler.hpp"
#include "utils/hook.hpp"
#include "steam_identity.hpp"
#include <deque>
#include <map>

#ifdef CSL_STEAMWORKS
#pragma warning(push)
#pragma warning(disable: 4996 4828)
#include <steam/steam_api.h>
#pragma warning(pop)

namespace steam_leaderboards
{
    namespace
    {
        // GFWL SDK xbox.h/xonline.h: local x86 declarations, not a new ABI.
        // QoS LB_Init 1023F4B0 uses 136-byte specs, 24-byte properties and views 10..14.
        struct data { BYTE type; union { int n; long long i64; double d; }; };
        struct property { DWORD id; data value; };
        struct column { WORD id; data value; };
        struct row { unsigned long long xuid; DWORD rank; long long rating; char name[16]; DWORD count; column* columns; };
        struct view { DWORD id, total, count; row* rows; };
        struct results { DWORD count; view* views; };
        struct spec { DWORD id, count; WORD columns[64]; };
        struct view_properties { DWORD id, count; property* properties; };
        struct overlapped
        {
            ULONG_PTR low, high, context; HANDLE event;
            void (WINAPI* callback)(DWORD, DWORD, overlapped*);
            DWORD_PTR callback_context; DWORD error;
        };
        static_assert(sizeof(data) == 16 && offsetof(data, i64) == 8);
        static_assert(sizeof(property) == 24 && sizeof(column) == 24);
        static_assert(sizeof(row) == 48 && offsetof(row, columns) == 44);
        static_assert(sizeof(spec) == 136 && sizeof(view_properties) == 12);
        static_assert(sizeof(overlapped) == 28 && sizeof(results) == 8);
        constexpr DWORD title = 0x41560829, buffer_bytes = 25600;
        constexpr int detail_magic = 0x43534C31;
        using enumerate_fn = DWORD(WINAPI*)(HANDLE, void*, DWORD, DWORD*, overlapped*);
        using cancel_fn = DWORD(WINAPI*)(overlapped*);
        enumerate_fn original_enumerate{};
        cancel_fn original_cancel{};
        using read_fn = DWORD(WINAPI*)(DWORD, DWORD, const unsigned long long*, DWORD, const spec*, DWORD*, results*, overlapped*);
        using rank_fn = DWORD(WINAPI*)(DWORD, DWORD, DWORD, DWORD, const spec*, DWORD*, HANDLE*);
        using pivot_fn = DWORD(WINAPI*)(DWORD, unsigned long long, DWORD, DWORD, const spec*, DWORD*, HANDLE*);
        using write_fn = DWORD(WINAPI*)(HANDLE, unsigned long long, DWORD, const view_properties*, overlapped*);
        read_fn original_read{}; rank_fn original_rank{}; pivot_fn original_pivot{}; write_fn original_write{};

        struct request
        {
            spec query{};
            DWORD first = 1, limit = 50;
            bool friends = false, around = false, upload = false;
            void* output{}; DWORD capacity{}; overlapped* operation{};
            int score{}; std::vector<int32> details;
            SteamAPICall_t call{}; SteamLeaderboard_t board{};
            int phase = 0; ULONGLONG started = 0;
        };
        std::deque<request> requests;
        std::map<HANDLE, request> enumerators;
        std::recursive_mutex mutex;
        std::string last_status = "Idle";
        std::deque<ULONGLONG> upload_times;
        bool was_public_ingame = false;
        bool stopped = false;
        bool flag(const char* name)
        {
            const auto* dvar = game::Dvar_FindVar(name);
            if (dvar && static_cast<unsigned char>(dvar->type) == game::DVAR_TYPE_BOOL) return dvar->current.enabled;
            return dvar && (static_cast<unsigned char>(dvar->type) == game::DVAR_TYPE_STRING
                ? dvar->current.string && std::strcmp(dvar->current.string, "1") == 0 : dvar->current.integer != 0);
        }
        bool ready() { return xlive::is_local_shim_loaded() && SteamUser() && SteamUser()->BLoggedOn() && SteamUserStats() && SteamUtils(); }
        unsigned long long native_self()
        {
            const auto get = reinterpret_cast<DWORD(WINAPI*)(DWORD, unsigned long long*)>(
                GetProcAddress(GetModuleHandleA("xlive.dll"), reinterpret_cast<const char*>(5261)));
            unsigned long long value{};
            return get && !get(0, &value) ? value : 0;
        }
        bool valid(const spec* query, DWORD count)
        {
            if (!query || count != 1 || query->id < 10 || query->id > 14 || query->count > 8) return false;
            for (DWORD i = 0; i < query->count; ++i) if (!query->columns[i] || query->columns[i] > 8) return false;
            return true;
        }
        DWORD finish(overlapped* op, DWORD error, DWORD count = 0)
        {
            if (op)
            {
                op->high = count; op->error = error ? HRESULT_FROM_WIN32(error) : ERROR_SUCCESS; op->low = error;
                if (op->event) SetEvent(op->event);
                if (op->callback) op->callback(error, count, op);
                return ERROR_IO_PENDING;
            }
            return error;
        }
        DWORD enqueue(request job)
        {
            if (!job.operation) return ERROR_INVALID_PARAMETER; // Native QoS reads/writes are asynchronous.
            if (requests.size() >= 32) return finish(job.operation, ERROR_BUSY);
            job.operation->low = ERROR_IO_PENDING;
            job.operation->high = 0; job.operation->error = ERROR_IO_PENDING;
            job.started = GetTickCount64(); requests.push_back(std::move(job));
            return ERROR_IO_PENDING;
        }
        DWORD WINAPI read(DWORD app, DWORD users, const unsigned long long* xuids, DWORD count, const spec* query,
                          DWORD* size, results* output, overlapped* op)
        {
            if (query && (query->id < 10 || query->id > 14)) return original_read(app, users, xuids, count, query, size, output, op);
            if ((app && app != title) || !size || !valid(query, count)) return finish(op, ERROR_INVALID_PARAMETER);
            if (!output || *size < buffer_bytes) { *size = buffer_bytes; return ERROR_INSUFFICIENT_BUFFER; }
            request job; job.query = *query; job.friends = true;
            job.output = output; job.capacity = *size; job.operation = op;
            std::lock_guard lock(mutex); return enqueue(std::move(job));
        }
        DWORD make_enumerator(DWORD app, DWORD first, DWORD rows, DWORD count, const spec* query,
                              DWORD* size, HANDLE* handle, bool around)
        {
            if (size) *size = 0; if (handle) *handle = INVALID_HANDLE_VALUE;
            if ((app && app != title) || !size || !handle || !valid(query, count) || rows == 0 || rows > 100)
                return ERROR_INVALID_PARAMETER;
            std::lock_guard lock(mutex);
            // QoS closes handles in LB_ReadLeaderboardComplete (1023FC30).
            for (auto it = enumerators.begin(); it != enumerators.end();)
            { DWORD flags{}; if (!GetHandleInformation(it->first, &flags)) it = enumerators.erase(it); else ++it; }
            if (enumerators.size() >= 32) return ERROR_BUSY;
            HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            if (!event) return GetLastError();
            request job; job.query = *query; job.first = std::clamp<DWORD>(first, 1, 1000000);
            job.limit = rows; job.around = around;
            enumerators.emplace(event, std::move(job)); *size = buffer_bytes; *handle = event;
            return ERROR_SUCCESS;
        }
        DWORD WINAPI by_rank(DWORD app, DWORD first, DWORD rows, DWORD count, const spec* query, DWORD* size, HANDLE* handle)
        {
            if (query && (query->id < 10 || query->id > 14)) return original_rank(app, first, rows, count, query, size, handle);
            return make_enumerator(app, first, rows, count, query, size, handle, false);
        }
        DWORD WINAPI by_xuid(DWORD app, unsigned long long pivot, DWORD rows, DWORD count, const spec* query, DWORD* size, HANDLE* handle)
        {
            if (query && (query->id < 10 || query->id > 14)) return original_pivot(app, pivot, rows, count, query, size, handle);
            return make_enumerator(app, 1, rows, count, query, size, handle, true);
        }
        DWORD WINAPI enumerate(HANDLE handle, void* output, DWORD size, DWORD* returned, overlapped* op)
        {
            std::unique_lock lock(mutex);
            auto it = enumerators.find(handle);
            if (it == enumerators.end()) { lock.unlock(); return original_enumerate(handle, output, size, returned, op); }
            if (returned) *returned = 0;
            if (!output || size < buffer_bytes) return finish(op, ERROR_INSUFFICIENT_BUFFER);
            auto job = std::move(it->second); enumerators.erase(it);
            job.output = output; job.capacity = size; job.operation = op;
            return enqueue(std::move(job));
        }
        DWORD WINAPI cancel(overlapped* op)
        {
            std::unique_lock lock(mutex);
            const auto it = std::find_if(requests.begin(), requests.end(), [op](const request& r) { return r.operation == op; });
            if (it == requests.end()) { lock.unlock(); return original_cancel(op); }
            requests.erase(it); lock.unlock(); return finish(op, ERROR_OPERATION_ABORTED);
        }
        long long numeric(const data& value) { return value.type == 2 ? value.i64 : value.n; }
        bool append_upload(const view_properties& source)
        {
            if (source.id < 10 || source.id > 14 || !source.properties || source.count < 2 || source.count > 9) return false;
            for (DWORD i = 0; i < source.count; ++i) if (source.properties[i].value.type != 1 && source.properties[i].value.type != 2) return false;
            request job; job.upload = true; job.query.id = source.id;
            job.score = static_cast<int>(std::clamp<long long>(numeric(source.properties[0].value), 0, INT_MAX));
            int32 wide_columns = 0;
            for (DWORD i = 1; i < source.count; ++i) if (source.properties[i].value.type == 2) wide_columns |= 1 << (i - 1);
            job.details = {detail_magic, 2, static_cast<int32>(source.count - 1), wide_columns};
            for (DWORD i = 1; i < source.count; ++i)
            {
                const auto value = static_cast<unsigned long long>(numeric(source.properties[i].value));
                job.details.push_back(static_cast<int32>(value)); job.details.push_back(static_cast<int32>(value >> 32));
            }
            // Coalesce cumulative snapshots rather than uploading per kill.
            for (auto& pending : requests) if (pending.upload && pending.query.id == source.id && !pending.call)
            { pending.score = job.score; pending.details = std::move(job.details); return true; }
            if (requests.size() >= 32) return false;
            requests.push_back(std::move(job)); return true;
        }
        DWORD WINAPI write(HANDLE session, unsigned long long xuid, DWORD count, const view_properties* views, overlapped* op)
        {
            if (views && count && (views[0].id < 10 || views[0].id > 14)) return original_write(session, xuid, count, views, op);
            // Never upload a remote player's claimed native XUID under this account.
            const auto self = native_self();
            if (!self || self != xuid || !views || !count || count > 5)
                return finish(op, ERROR_INVALID_PARAMETER);
            std::lock_guard lock(mutex);
            if (requests.size() + count + (op ? 1u : 0u) > 32) return finish(op, ERROR_BUSY);
            for (DWORD i = 0; i < count; ++i)
            {
                const auto& v = views[i];
                if (v.id < 10 || v.id > 14 || !v.properties || v.count < 2 || v.count > 9) return finish(op, ERROR_INVALID_DATA);
                for (DWORD p = 0; p < v.count; ++p)
                    if (v.properties[p].value.type != 1 && v.properties[p].value.type != 2) return finish(op, ERROR_INVALID_DATA);
            }
            for (DWORD i = 0; i < count; ++i) if (!append_upload(views[i])) return finish(op, ERROR_INVALID_DATA);
            // Network completion must not be falsely reported as persisted success.
            // Native XSession writes are retired only after all queued view uploads.
            if (op)
            {
                request barrier; barrier.phase = 3; barrier.operation = op;
                return enqueue(std::move(barrier));
            }
            return ERROR_IO_PENDING;
        }
        template <typename T> bool completed(SteamAPICall_t call, T& result, bool& failure)
        {
            if (!SteamUtils()->IsAPICallCompleted(call, &failure)) return false;
            if (!failure && !SteamUtils()->GetAPICallResult(call, &result, sizeof(result), T::k_iCallback, &failure)) failure = true;
            return true;
        }
        DWORD pack(request& job, const LeaderboardScoresDownloaded_t& downloaded)
        {
            if (!job.output || job.capacity < buffer_bytes || downloaded.m_cEntryCount < 0) return ERROR_INVALID_DATA;
            const auto count = std::min<int>(downloaded.m_cEntryCount, static_cast<int>(std::min<DWORD>(100, job.limit)));
            auto* bytes = static_cast<unsigned char*>(job.output);
            std::memset(bytes, 0, buffer_bytes);
            auto* header = reinterpret_cast<results*>(bytes);
            auto* table = reinterpret_cast<view*>(bytes + sizeof(results));
            auto* rows = reinterpret_cast<row*>(bytes + sizeof(results) + sizeof(view));
            auto* columns = reinterpret_cast<column*>(reinterpret_cast<unsigned char*>(rows) + sizeof(row) * count);
            const auto required = sizeof(results) + sizeof(view) + count * (sizeof(row) + job.query.count * sizeof(column));
            if (required > buffer_bytes) return ERROR_INSUFFICIENT_BUFFER;
            header->count = 1; header->views = table;
            table->id = job.query.id; table->total = static_cast<DWORD>(std::max(0, SteamUserStats()->GetLeaderboardEntryCount(job.board)));
            table->count = static_cast<DWORD>(count); table->rows = rows;
            for (int i = 0; i < count; ++i)
            {
                LeaderboardEntry_t entry{}; int32 details[32]{};
                if (!SteamUserStats()->GetDownloadedLeaderboardEntry(downloaded.m_hSteamLeaderboardEntries, i, &entry, details, 32)
                    || entry.m_cDetails < 4 || entry.m_cDetails > 32 || details[0] != detail_magic || details[1] != 2
                    || details[2] < 0 || details[2] > 8 || entry.m_cDetails != 4 + 2 * details[2]) return ERROR_INVALID_DATA;
                rows[i].xuid = entry.m_steamIDUser.ConvertToUint64(); rows[i].rank = entry.m_nGlobalRank;
                if (entry.m_steamIDUser == SteamUser()->GetSteamID()) rows[i].xuid = native_self();
                rows[i].rating = entry.m_nScore; rows[i].count = job.query.count;
                rows[i].columns = columns + i * job.query.count;
                const auto name = steam_identity::display_name(SteamFriends()->GetFriendPersonaName(entry.m_steamIDUser));
                std::memcpy(rows[i].name, name.c_str(), std::min(name.size(), sizeof(rows[i].name) - 1));
                for (DWORD c = 0; c < job.query.count; ++c)
                {
                    const auto id = job.query.columns[c]; if (id > details[2]) return ERROR_INVALID_DATA;
                    auto& output = rows[i].columns[c]; output.id = id;
                    output.value.type = (details[3] & (1 << (id - 1))) ? 2 : 1;
                    output.value.i64 = static_cast<long long>(static_cast<unsigned long long>(static_cast<unsigned int>(details[4 + (id - 1) * 2]))
                        | (static_cast<unsigned long long>(static_cast<unsigned int>(details[5 + (id - 1) * 2])) << 32));
                }
            }
            return ERROR_SUCCESS;
        }
        void public_snapshot()
        {
            const bool ingame = flag("csl_public_match") && *reinterpret_cast<const int*>(game::game_offset(0x111F45F8)) == 10;
            const bool ended = was_public_ingame && !ingame; was_public_ingame = ingame;
            if (!ended || !*reinterpret_cast<const BYTE*>(game::game_offset(0x116101C8))) return;
            // Same real playerStatsTable mapping and numeric conversion as LB_UploadPlayerStats,
            // without changing onlinegame or fabricating a LIVE session handle.
            const auto* indices = reinterpret_cast<const unsigned*>(game::game_offset(0x11634E08));
            const auto* native = reinterpret_cast<const view_properties*>(game::game_offset(0x11635140));
            unsigned stat = 0;
            for (unsigned v = 0; v < 5; ++v)
            {
                if (native[v].id != 10 + v || native[v].count > 9 || !native[v].properties) return;
                std::vector<property> properties(native[v].properties, native[v].properties + native[v].count);
                for (auto& prop : properties)
                {
                    if (stat >= 29 || indices[stat] >= 3498) return;
                    const auto index = indices[stat++];
                    const int value = index < 2000 ? *reinterpret_cast<const BYTE*>(game::game_offset(0x1160BFB4) + index)
                        : *reinterpret_cast<const int*>(game::game_offset(0x1160A844) + index * 4);
                    if (prop.value.type == 1) prop.value.n = value; else if (prop.value.type == 2) prop.value.i64 = value; else return;
                }
                view_properties snapshot{native[v].id, native[v].count, properties.data()}; append_upload(snapshot);
            }
        }
        void pump()
        {
            std::lock_guard lock(mutex);
            if (stopped) return;
            if (!ready())
            {
                for (auto it = requests.begin(); it != requests.end();)
                {
                    if (it->operation && it->started && GetTickCount64() - it->started > 60000)
                    {
                        auto* op = it->operation; it = requests.erase(it);
                        last_status = "Steam unavailable; leaderboard request timed out"; finish(op, ERROR_NOT_CONNECTED);
                    }
                    else ++it;
                }
                return;
            }
            public_snapshot();
            if (requests.empty()) return;
            // Native task reset/cancellation retires the caller's buffer ownership.
            for (auto it = requests.begin(); it != requests.end();)
                if (it->operation && it->operation->low != ERROR_IO_PENDING) it = requests.erase(it); else ++it;
            if (requests.empty()) return;
            while (!upload_times.empty() && GetTickCount64() - upload_times.front() >= 600000) upload_times.pop_front();
            // Rate-limited uploads must never hold the browser read queue hostage.
            if (requests.front().upload && !requests.front().call && upload_times.size() >= 10)
            {
                const auto read = std::find_if(requests.begin(), requests.end(), [](const request& r) { return !r.upload && r.phase != 3; });
                if (read == requests.end()) { last_status = "Waiting for Steam upload rate limit"; return; }
                auto priority = std::move(*read); requests.erase(read); requests.push_front(std::move(priority));
            }
            auto& job = requests.front();
            if (job.phase == 3) { auto* op = job.operation; requests.pop_front(); finish(op, ERROR_SUCCESS); return; }
            if (job.started && GetTickCount64() - job.started > 60000)
            {
                auto* op = job.operation; const bool upload = job.upload; requests.pop_front();
                if (upload) for (auto& pending : requests) if (pending.phase == 3 && pending.operation)
                { finish(pending.operation, ERROR_TIMEOUT); pending.operation = nullptr; }
                last_status = "Steam leaderboard request timed out"; finish(op, ERROR_TIMEOUT); return;
            }
            if (!job.call)
            {
                const auto name = "consolation_qos_v1_view_" + std::to_string(job.query.id);
                job.call = SteamUserStats()->FindOrCreateLeaderboard(name.c_str(), k_ELeaderboardSortMethodDescending, k_ELeaderboardDisplayTypeNumeric);
                job.started = GetTickCount64(); job.phase = 0;
                if (!job.call)
                {
                    auto* op = job.operation; const bool upload = job.upload; requests.pop_front();
                    if (upload) for (auto& pending : requests) if (pending.phase == 3 && pending.operation)
                    { finish(pending.operation, ERROR_NOT_CONNECTED); pending.operation = nullptr; }
                    finish(op, ERROR_NOT_CONNECTED);
                }
                return;
            }
            bool failure = false;
            if (job.phase == 0)
            {
                LeaderboardFindResult_t result{}; if (!completed(job.call, result, failure)) return;
                if (!failure && result.m_bLeaderboardFound)
                {
                    job.board = result.m_hSteamLeaderboard;
                    if (job.upload)
                    {
                        job.call = SteamUserStats()->UploadLeaderboardScore(job.board, k_ELeaderboardUploadScoreMethodForceUpdate,
                            job.score, job.details.data(), static_cast<int>(job.details.size()));
                        if (job.call) upload_times.push_back(GetTickCount64()); // At most ten uploads per ten minutes.
                    }
                    else job.call = SteamUserStats()->DownloadLeaderboardEntries(job.board,
                        job.friends ? k_ELeaderboardDataRequestFriends : job.around ? k_ELeaderboardDataRequestGlobalAroundUser : k_ELeaderboardDataRequestGlobal,
                        job.around ? -static_cast<int>(job.limit / 2) : static_cast<int>(job.first),
                        job.around ? static_cast<int>((job.limit - 1) / 2) : static_cast<int>(job.first + job.limit - 1));
                    job.phase = 1; if (job.call) return;
                }
                failure = true;
            }
            DWORD error = ERROR_NOT_CONNECTED;
            if (!failure && job.upload)
            {
                LeaderboardScoreUploaded_t result{}; if (!completed(job.call, result, failure)) return;
                if (!failure && result.m_bSuccess) error = ERROR_SUCCESS;
            }
            else if (!failure)
            {
                LeaderboardScoresDownloaded_t result{}; if (!completed(job.call, result, failure)) return;
                if (!failure) error = pack(job, result);
            }
            last_status = error == ERROR_SUCCESS ? "Steam leaderboard operation completed" : "Steam leaderboard operation failed";
            console::info("[leaderboards] view=%lu error=%lu %s\n", job.query.id, error, last_status.c_str());
            auto* op = job.operation; const bool failed_upload = job.upload && error != ERROR_SUCCESS;
            requests.pop_front();
            if (failed_upload)
                for (auto& pending : requests) if (pending.phase == 3 && pending.operation) { finish(pending.operation, error); pending.operation = nullptr; }
            finish(op, error);
        }
    }
    class component final : public component_interface
    {
        void post_load() override
        {
            // QoS PC 1.1 import slots; SDK stdcall signatures validated against callers.
            scheduler::once([]
            {
                if (!xlive::is_local_shim_loaded()) return;
                original_enumerate = *reinterpret_cast<enumerate_fn*>(game::game_offset(0x104765C0));
                original_cancel = *reinterpret_cast<cancel_fn*>(game::game_offset(0x104764BC));
                original_read = *reinterpret_cast<read_fn*>(game::game_offset(0x10476524));
                original_rank = *reinterpret_cast<rank_fn*>(game::game_offset(0x104765C8));
                original_pivot = *reinterpret_cast<pivot_fn*>(game::game_offset(0x104765BC));
                original_write = *reinterpret_cast<write_fn*>(game::game_offset(0x104764FC));
                utils::hook::set(game::game_offset(0x10476524), &read);
                utils::hook::set(game::game_offset(0x104765C8), &by_rank);
                utils::hook::set(game::game_offset(0x104765BC), &by_xuid);
                utils::hook::set(game::game_offset(0x104765C0), &enumerate);
                utils::hook::set(game::game_offset(0x104764BC), &cancel);
                utils::hook::set(game::game_offset(0x104764FC), &write);
                scheduler::loop(pump, scheduler::main);
            }, scheduler::main);
            command::add("steam_leaderboards_status", []
            {
                std::lock_guard lock(mutex);
                console::info("[leaderboards] %s; queued=%zu; app=%u; %s\n", last_status.c_str(), requests.size(),
                    SteamUtils() ? SteamUtils()->GetAppID() : 0, ready() ? "Steam ready" : "Steam unavailable");
            });
        }
        void pre_destroy() override
        {
            std::lock_guard lock(mutex);
            stopped = true;
            if (!original_enumerate) return;
            utils::hook::set(game::game_offset(0x10476524), original_read);
            utils::hook::set(game::game_offset(0x104765C8), original_rank);
            utils::hook::set(game::game_offset(0x104765BC), original_pivot);
            utils::hook::set(game::game_offset(0x104765C0), original_enumerate);
            utils::hook::set(game::game_offset(0x104764BC), original_cancel);
            utils::hook::set(game::game_offset(0x104764FC), original_write);
            for (const auto& entry : enumerators) CloseHandle(entry.first);
            enumerators.clear(); requests.clear();
        }
    };
}
REGISTER_COMPONENT(steam_leaderboards::component)
#endif
