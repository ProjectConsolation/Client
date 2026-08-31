I’m using the `are-specify` skill because this request is specifically to reconstruct a project specification from AGENTS.md documentation.
# 1. Project Overview

ConsolationClient is a Windows x86 DLL-based multiplayer modification for *007: Quantum of Solace*, supporting game integration, offline Windows LIVE compatibility, controller input, runtime patches, bot behavior, console tooling, custom assets, and ultrawide rendering.

Core outputs:

- `d3d9.dll`: proxy and runtime modification DLL.
- `xlive.dll`: offline Windows LIVE compatibility shim.
- Generated version metadata and build artifacts.
- Optional controller calibration and offline profile/title-storage data.

Technology stack:

- C++20.
- Win32/x86 (`Win32` platform).
- Visual Studio toolset `v143`.
- Latest installed Windows SDK.
- Premake `v5.0.0-beta8`.
- MSBuild.
- Wine/MSVC-Wine for Linux builds.
- Winsock2, Direct3D 9, GDI+, Windows HID, XInput, MinHook, AsmJit, DbgHelp, GSL, TomMath.
- Target game version: Patch `1.1`, English or French multiplayer installation.

The project does not support single-player launches, repacks, unsupported regions, unpatched version `1.0`, unsupported game versions, or non-multiplayer launches.

# 2. Architecture

The system consists of these concern-oriented boundaries:

1. Bootstrap and DLL proxying  
   `DllMain` invokes startup validation and component lifecycle operations. `sdllp` lazily loads system libraries and forwards exported Direct3D functions.

2. Component lifecycle  
   `component_interface` defines lifecycle hooks. `component_loader` owns registered components, dispatches startup/load/shutdown operations, filters unsupported components, and resolves imports.

3. Game ABI bridge  
   `game_offset` rebases fixed IDA addresses against `jb_mp_s.dll`. `symbol<T>` exposes fixed engine functions and globals. ABI structures preserve exact x86 layouts.

4. Runtime patching and utilities  
   Hook, memory, PE, filesystem, thread, string, scheduler, exception, and resource modules provide shared infrastructure.

5. Engine components  
   Console, mouse input, filesystem overrides, gametype loading, GSC loading, fastfiles, sounds, renderer extensions, profile patches, security patches, and bot logic integrate with fixed game callsites.

6. Offline LIVE shim  
   `xlive_shim.cpp` forwards Winsock APIs, provides deterministic offline identity, profile persistence, title storage, session stubs, notification stubs, enumerators, and overlapped-operation completion.

7. Controller stack  
   The controller pipeline is:

   `transport → discovery → registry → driver set → raw_sample → calibration → canonical_sample → frame_ring → aim/key/view engine integration`

   Device-specific drivers support Xbox/XInput, DualShock 4, DualSense, and DualSense Edge.

8. Controller input processing  
   Mapping converts physical buttons and axes into engine keys and commands. Aim processing applies deadzones, curves, calibration, slowdown, lock-on, and turn integration. Engine integration emits key transitions, movement, and view changes.

Major design decisions:

- All engine interaction is version-sensitive and 32-bit.
- Component registration is static and lifecycle-driven.
- Controller acquisition is non-allocating and `noexcept` across the engine ABI.
- Offline identity and storage are deterministic.
- HID polling uses bounded buffers and bounded report draining.
- Controller frame transport uses a single-producer/single-consumer ring.
- Hook teardown restores original bytes and window procedures.
- Unsupported or malformed controller reports produce no guessed input.

# 3. Public API Surface

## Bootstrap and proxy

```cpp
bool main();
BOOL DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved);
FARPROC sdllp::get_export(const char* library, const char* function);
void sdllp::load_library(const char* library);
bool sdllp::is_loaded(const char* library);
```

Resource constants:

```cpp
constexpr int ID_ICON = 102;
constexpr int IMAGE_SPLASH = 300;
```

## Component lifecycle

```cpp
class component_interface {
public:
    virtual ~component_interface();
    virtual void post_start();
    virtual void post_load();
    virtual void pre_destroy();
    virtual void* load_import(
        const std::string& library,
        const std::string& function);
    virtual bool is_supported();
    virtual game::XAssetType get_type();
};

class component_loader {
public:
    static void register_component(
        std::unique_ptr<component_interface>&& component);
    static bool post_start();
    static bool post_load();
    static void pre_destroy();
    static void clean();
    static void* load_import(
        const std::string& library,
        const std::string& function);
    static void trigger_premature_shutdown();
    static std::vector<std::unique_ptr<component_interface>>&
        get_components();
};

template <typename T>
T* get();

template <typename T>
class installer;
```

`REGISTER_COMPONENT(name)` statically installs an `installer<name>`.

## Game and dvar APIs

```cpp
uintptr_t game::game_offset(uintptr_t ida_address);

game::dvar_s* dvars::Dvar_RegisterFloat(
    const char* name, const char* description,
    double value, double min, double max, uint16_t flags);

game::dvar_s* dvars::Dvar_RegisterVec4(
    const char* name, const char* description,
    float x, float y, float z, float w,
    float min, float max, uint16_t flags);

game::dvar_s* dvars::Dvar_RegisterBool(
    const char* name, int default_value,
    const char* description, uint16_t flags);

game::dvar_s* dvars::Dvar_RegisterInt(
    const char* name, const char* description,
    int default_value, int min, int max, uint16_t flags);

game::dvar_s* dvars::Dvar_RegisterString(
    const char* name, const char* default_value,
    const char* description, uint16_t flags);

char* dvars::Dvar_ValueToString(
    game::dvar_s* dvar, game::DvarValue value);

dvars::dvar_spec dvars::make_bool(
    const char* name, const char* description,
    bool value, uint16_t flags);

dvars::dvar_spec dvars::make_int(
    const char* name, const char* description,
    int value, int min, int max, uint16_t flags);

dvars::dvar_spec dvars::make_float(
    const char* name, const char* description,
    float value, float min, float max, uint16_t flags);

dvars::dvar_spec dvars::make_string(
    const char* name, const char* description,
    const char* value, uint16_t flags);

game::dvar_s* dvars::replace_dvar(
    const dvars::dvar_spec& spec, bool log);

game::dvar_s* dvars::replace_dvar_at(
    uintptr_t nop_address, size_t nop_size,
    game::dvar_s** target,
    const dvars::dvar_spec& spec, bool log);

std::string dvars::dvar_get_domain(
    game::dvar_type type, const game::DvarLimits& domain);

std::string dvars::dvar_get_vector_domain(
    int components, const game::DvarLimits& domain);
```

Override registration:

```cpp
void dvars::overrides::register_bool(
    const std::string&, bool, unsigned int);

void dvars::overrides::register_int(
    const std::string&, int, int, int, unsigned int);

void dvars::overrides::register_float(
    const std::string&, float, float, float, unsigned int);

void dvars::overrides::register_enum(
    const std::string&, const char* const*, int, unsigned int);

void dvars::overrides::register_string(
    const std::string&, const std::string&, unsigned int);
```

## Runtime component APIs

```cpp
void patches::enforce_ads_sprint_interrupt(game::usercmd_t* cmd);
void xlive::apply_early();

std::string Entities::Build();
void Entities::parse(std::string text);
std::vector<std::string> Entities::GetModels();
bool Entities::ConvertVehicles();
bool Entities::ConvertTurrets();
void Entities::AddCarePackages();
void Entities::AddRemovedSModels();
void Entities::DeleteOldSchoolPickups();

bool gamepad::is_controller_active();
bool gamepad::should_hide_cursor();
void gamepad::note_mouse_activity();

void filesystem::register_path(const std::filesystem::path&);
void filesystem::unregister_path(const std::filesystem::path&);
std::string filesystem::read_file(const std::string&);
std::string filesystem::find_file(const std::string&);
bool filesystem::exists(const std::string&);
```

## Console and renderer

```cpp
void command::add_raw(
    const std::string&, std::function<void(command::params&)>);

void command::execute(const std::string&);

bool game_console::is_active();
void game_console::toggle();
void game_console::append_output(const std::string&);

void console::print(const char*, ...);
void console::error(const char*, ...);
void console::debug(const char*, ...);
void console::warn(const char*, ...);
void console::info(const char*, ...);

void set_custom_resolution_command();
void reset_custom_resolution_command();
void dump_ultrawide_command();
void apply_ultrawide_now_command();
```

## Controller timing, diagnostics, and errors

```cpp
using clock::timestamp = ...;
using clock::seconds = ...;
using clock::milliseconds = ...;

clock::timestamp clock::now();
clock::timestamp clock::since_epoch();
clock::milliseconds latency_span::latency() const;

std::string to_string(facility);
std::string to_string(severity);
std::string to_string(errc);
std::ostream& operator<<(std::ostream&, errc);

void report(
    diagnostic_sink&, facility, severity,
    std::string_view message,
    errc code = errc::none,
    std::string_view device = {});
```

```cpp
class context {
public:
    static context make_context(...);
    diagnostic_sink& diagnostics() const;
    bool developer() const;
    void report(...);
    void report(..., device_id);
};
```

## Device and transport

```cpp
bool same_binding(
    const transport_binding&, const transport_binding&);

family classify(const device_identity&);
capabilities capabilities_for(family);

std::optional<user_index> user_index::try_from(int);

connection classify_link(size_t report_length);

std::vector<hid_enumeration_entry> enumerate(...);
std::unique_ptr<hid_device> open(...);

class registry;
class discovery;
class device_notifier;
class xinput_module;
```

## Drivers and decoding

```cpp
uint8_t rd_u8(std::span<const uint8_t>, size_t);
uint16_t rd_le16(std::span<const uint8_t>, size_t);
int16_t rd_le16s(std::span<const uint8_t>, size_t);
uint32_t rd_le32(std::span<const uint8_t>, size_t);

uint32_t crc32_le(std::span<const uint8_t>);
bool verify_ps_crc32(std::span<const uint8_t>);

ps_touch_point decode_touch_point(...);
void apply_hat(...);
stick_vector normalize_ps_stick(...);

bool decode_xinput(...);
bool decode_dualshock4(...);
bool decode_dualsense(...);
bool decode_dualsense_edge(...);

std::vector<uint8_t> encode_dualshock4_output(...);
std::vector<uint8_t> encode_dualsense_output(...);

class driver;
class xinput_driver;
class dualshock4_driver;
class dualsense_driver;
class dualsense_edge_driver;
class set;
```

## Calibration and aim

```cpp
bool validate(const calibration::profile&, std::string&) noexcept;
calibration::profile default_profile(controller::family);

void apply(
    const calibration::profile&,
    const sample::raw_sample&,
    sample::canonical_sample&) noexcept;

class calibration::store {
public:
    static std::filesystem::path directory();
    static std::filesystem::path file_for(...);
    static bool save(...);
    static std::optional<profile> load(...);
};

float fov_scale(...);
class aim_calibration;
class aim_processor;
class low_pass;
class turn_integrator;
class aim_graph;
class response_curve;
```

## Mapping and engine integration

```cpp
std::optional<engine_key> key_from_name(std::string_view);
std::string_view key_name(engine_key);
bool is_controller_key(int);

std::string_view to_string(action);
std::string_view command(action);

engine_key to_engine_key(button);
engine_key to_engine_key(apad_input);

bool axis_deflected(...);
stick_layout stick_layout_from_name(std::string_view);

void register_commands(const context&, runtime&);
dvars register_dvars(const context&);
void install(runtime&);

bool move_changed(...);
uint16_t pack_move(int8_t forward, int8_t right, uint16_t key);
move_delta unpack_move(uint16_t packed, uint16_t key);

class bind_bridge;
class key_dispatcher;
class view_driver;
```

## Offline LIVE exports

The `xlive.def` ABI must export all declared `xlive_X*` symbols at fixed ordinals with `NONAME`, including Winsock, XNet, notifications, users, profiles, statistics, sessions, storage, UI, and enumerator functions.

# 4. Data Structures & State

Core ABI types include:

- `XUID`
- `xoverlapped`
- `xsession_info`
- `xstorage_download_results`
- `xuser_data`
- `xuser_profile_setting`
- `xuser_read_profile_setting_result`
- `xuser_signin_info`
- `game::dvar_s`
- `game::DvarValue`
- `game::DvarLimits`
- `game::Material`
- `game::windowDef_t`
- `game::menuDef_t`
- `game::playerState_t`
- `game::entity_t`
- `game::client_t`
- `game::usercmd_t`

Required layout assertions:

- `AimAssistGlobals::screenTargets == 0xFC`
- `AimAssistGlobals::screenTargetCount == 0xDFC`
- `sizeof(AimAssistGlobals) == 0xE34`
- `sizeof(Material) == 104`
- `sizeof(windowDef_t) == 168`
- `sizeof(menuDef_t) == 308`
- `sizeof(usercmd_t) == 0x2C`
- `sizeof(entity_t) == 0x290`
- `dvar_s::type == 12`
- `dvar_s::modified == 13`
- `dvar_s::current == 16`
- `dvar_s::latched == 32`
- `dvar_s::reset == 48`
- `dvar_s::domain == 64`
- `sizeof(report_id) == 1`

Controller sample state:

- `raw_sample`: native device values.
- `canonical_sample`: normalized buttons, sticks, triggers, touch, motion, battery, and capabilities.
- `input_frame`: device ID, family, connection, sequence, timing, and canonical sample.
- `frame_ring<Capacity>`: fixed-inline SPSC queue with atomic head/tail.
- `button_set`: `uint32_t` bitmask.
- `stick_vector`: normalized `x/y`.
- `trigger_sample`: raw `0..255` and normalized `[0,1]`.
- `touchpad`: maximum two touch points.
- `battery_state`: optional percentage and status.
- `profile`: version, family, device key, stick calibration, trigger calibration, motion calibration, smoothing.
- `output_request`: rumble, light bar, player LEDs, adaptive triggers.

Controller runtime state includes `active_`, `latest_`, `sequence_`, `last_published_`, `had_device_`, `engine_ready_`, `lit_device_`, and `lit_colour_`.

Offline state is persisted beneath:

- `storage/profiles`
- `storage/title_storage`
- legacy profile `41560829.gpd`

Profile settings are serialized according to profile setting type tags `1`, `2`, `3`, `4`, `5`, `6`, `7`, and `0xFF`.

# 5. Configuration

Build configuration:

- Architecture: x86/Win32.
- Language: C++20.
- Toolset: `v143`.
- Configurations: `Debug`, `Release`.
- Debug defines: `DEBUG`, `_DEBUG`.
- Release defines/options: `NDEBUG`, `/Os`, `FatalCompileWarnings`, `LinkTimeOptimization`.
- Premake version: `v5.0.0-beta8`.
- Solution: `build/consolation-client.sln`.
- Projects: `d3d9`, `xlive`.

Runtime dvars include:

| Dvar | Type | Default |
|---|---|---:|
| `r_borderless` | bool | `0` |
| `g_debugVelocity` | bool | `0` |
| `g_debugLocalization` | bool | `0` |
| `bot_maxHealth` | int `[1,1000]` | `100` |
| `m_rawInput` | bool | `1` |
| `gpad_enabled` | bool | `1` |
| `gpad_present` | bool | `0` |
| `gpad_in_use` | bool | `0` |
| `gpad_debug` | bool | `0` |
| `gpad_buttonsConfig` | string | `buttons_default_alt` |
| `gpad_sticksConfig` | string | `thumbstick_default` |
| `gpad_rumble` | bool | `1` |
| `gpad_stick_deadzone_min` | float `[0,1]` | `0.2` |
| `gpad_stick_deadzone_max` | float `[0,1]` | `0.01` |
| `gpad_button_deadzone` | float `[0,1]` | `0.13` |
| `gpad_menu_scroll_delay_first` | int `[0,1000]` | `420` |
| `gpad_menu_scroll_delay_rest` | int `[0,1000]` | `210` |
| `gpad_menu_scroll_delay_min` | int `[0,1000]` | `50` |
| `gpad_menu_scroll_accel_time` | int `[0,5000]` | `1500` |
| `input_invertPitch` | bool | `0` |
| `cg_drawWatermark` | bool | `1` |
| `cg_drawVersion` | bool | `1` |
| `cg_drawVersionX` | float `[-1024,1024]` | `50.0` |
| `cg_drawVersionY` | float `[-1024,1024]` | `18.0` |
| `r_aspectRatioCustomEnable` | bool | unset |
| `r_aspectRatioCustom` | float `[4/3,63/9]` | `16/9` |
| `r_ultrawideCustomMode` | string | `disabled` |

Environment and command-line options:

- `offline`, `local_offline`, `local-offline`
- `name`, `+name`, `-name`
- `-set name value`
- `-seta name value`
- `LIBIW4X_CONTROLLER_TRACE`, default `0`
- `--yes`
- `--clean`
- `--no-deps`
- `--no-submodules`
- `--force`
- `--verbose`
- directory and timeout overrides
- `debug|Debug`
- `release|Release`

# 6. Dependencies

| Dependency | Version | Purpose |
|---|---|---|
| Premake | `v5.0.0-beta8` | Workspace and project generation |
| Visual Studio/MSVC | `v143` | x86 C++20 compilation |
| Windows SDK | latest installed | Win32, PE, resource, HID, and D3D APIs |
| Wine | configured runtime | Linux cross-build execution |
| MSVC-Wine | configured runtime | Windows-compatible compiler under Wine |
| MSBuild | x86-capable | Solution compilation |
| MinHook | repository dependency | Function detours |
| AsmJit | repository dependency | Runtime assembly generation |
| GSL | repository dependency | Scope guards and utility support |
| TomMath | repository dependency | RNG fallback support |
| Winsock2 | Windows SDK | Network forwarding |
| Direct3D 9 | Windows SDK | Proxy exports and rendering |
| GDI+ | Windows SDK | Resource and graphics support |
| Windows HID / SetupAPI | Windows SDK | HID discovery and I/O |
| XInput | dynamically loaded | Xbox controller input |
| DbgHelp | Windows SDK | Minidump generation |
| Common Controls v6 | Windows manifest | Modern Windows controls |

# 7. Behavioral Contracts

## Runtime Behavior

Component lifecycle:

- `post_start()` and `post_load()` execute once using static handled sentinels.
- Repeated calls return immediately.
- `post_load()` cleans unsupported components before dispatch.
- `pre_destroy()` invokes retained component cleanup.
- `trigger_premature_shutdown()` throws `premature_shutdown_trigger`.
- `premature_shutdown_trigger::what()` returns `"Premature shutdown requested"`.
- Lifecycle exceptions are wrapped as `std::runtime_error` using:
  - `post_start failed in {}: {}`
  - `post_load failed in {}: {}`

Scheduler:

- `scheduler::loop()` retains tasks through `cond_continue`.
- `scheduler::once()` removes tasks through `cond_end`.
- Main-thread scheduling uses `scheduler::main`.
- Renderer scheduling uses `scheduler::pipeline::renderer`.
- Async scheduler polls window closure every `10ms`.
- `game_window_closed()` treats invalid windows lasting longer than `3` seconds as shutdown.
- `gamepad::runtime::engine_ready()` runs after `100ms`.
- Gamepad frames run every `16ms`.
- Dvar refresh runs every `250ms`.
- Mouse readiness runs every `100ms`.

Controller behavior:

- `driver::poll` and `driver::submit` are `noexcept`, non-allocating operations.
- HID draining is capped at `max_reports_per_poll == 32`.
- `frame_ring` drops newest frames when full.
- `runtime::advance()` aborts publication when polling fails.
- `runtime::engine_ready()` is idempotent.
- Held keys are released when active-device ownership changes.
- Light-bar RGB values clamp to `[0,255]`.
- Output is submitted only when device or color changes.
- `light_bar_r`, `light_bar_g`, and `light_bar_b` default to `196`, `151`, and `54`.

Transport failures:

- Missing XInput functions return `ERROR_DEVICE_NOT_CONNECTED`.
- Empty enumerations return `ERROR_NO_MORE_FILES`.
- Missing title-storage files return `0x8015C004`.
- Oversized title-storage files return `0x8015C003`.
- `xlive_XNotifyGetNext()` returns `0`.
- `xlive_XNetGetConnectStatus()` returns `1`.
- `xlive_XOnlineGetNatType()` returns `1`.
- `xlive_XNetGetEthernetLinkStatus()` returns `1`.
- APIs accepting `xoverlapped` may return `ERROR_IO_PENDING`; callers must query the overlapped result.

Exceptions and crash handling:

- Harmless errors: `STATUS_INTEGER_OVERFLOW`, `STATUS_FLOAT_OVERFLOW`, `STATUS_SINGLE_STEP`.
- Failed minidump writing displays `"Minidump Error"` and exits with code `123`.
- Crash dialog title: `"Project: Consolation ERROR"`.
- Crash text format: `Fatal error (0x%08X) at 0x%p.\nA minidump has been written.\n\n`.
- Minidump path format: `minidumps/consolation-crash-%s.dmp`.

## Implementation Contracts

Exact patterns and sentinels:

```text
PREMAKE_HASH = 2301e3e23ff3074cb83a5ea6103d68c7ea81dad56b786807c84b0643cddea31b
gitVersioningCommand = "git describe --tags --dirty --always"
#define GIT_DESCRIBE (.+)%s*$
#define VERSION_PRODUCT \"([^\"]*)\"%s*$
#define VERSION_BUILD \"([^\"]*)\"%s*$
```

Offline markers:

```text
offline
local_offline
local-offline
```

Entity parser sentinels:

```text
PARSE_AWAIT_KEY
PARSE_READ_KEY
PARSE_AWAIT_VALUE
PARSE_READ_VALUE
```

Storage:

```text
offline://<facility>/<sanitized-item>
```

Storage sanitization replaces control characters and:

```text
<>:"/\|?*
```

with `_`; empty, `"."`, and `".."` become `"unnamed"`.

Bot constants:

```text
CLIENT_STRIDE = 688916
CLIENT_ENTITYPTR_OFF = 0x2128C
CLIENT_LAST_USERCMD_OFF = 0x20E9C
CLIENT_USERINFO_OFF = 1604
PS_ORIGIN_OFFSET = 0x20
PS_MAXHEALTH_OFFSET = 0x32C4
MAX_CLIENTS = 18
TARGET_MEMORY_MS = 5000
SEARCH_UPDATE_MS = 900
STUCK_REPATH_MS = 1400
AIM_SETTLE_THRESHOLD = 1400
FIRE_RANGE = 1400.0f
MELEE_RANGE = 96.0f
CROUCH_RANGE = 384.0f
SPRINT_RANGE = 700.0f
STUCK_DIST_EPSILON = 24.0f
```

Console formats:

```text
"%08X  "
"%02X "
"?? "
"\r\n"
"%s %d %u\n"
"-%s %d %u\n"
"%s\n"
```

Console limits:

```text
max_console_lines = 128u
max_history_entries = 32u
max_input_chars = 240u
max_auto_complete_matches = 24u
caret blink = 500ms
```

Dvar formats:

```text
"Domain is any %iD vector"
"Domain is any %iD vector with components %g or smaller"
"Domain is any %iD vector with components %g or bigger"
"Domain is any %iD vector with components from %g to %g"
"Domain is 0 or 1"
"Domain is any number"
"Domain is any integer"
"Domain is any 4-component color, in RGBA format"
"Domain is any text"
"unhandled dvar type '%i'"
"true"
"false"
"<?>"
```

Game entity formats:

```text
"{\n"
"\"key\" \"value\"\n"
"}\n"
"Parsing error!"
```

Fastfile and script formats:

```text
"{} \"{}\"\r\n"
"  {} @ {:08X}: "
"{:02X} "
"?? "
"******* script compile error *******"
"script compile error\n%s\n%s\n(see console for details)\n"
```

Security logs:

```text
"UI_ReplaceDirective: rejected oversized directive input"
"PartyAtomicHost_HandleMemberJoin: rejected malformed message cursor"
"PartyAtomicHost_HandleMemberJoin: rejected oversized message (%zu bytes)"
"securityGuardSelfTest: party=%s ui=%s"
```

Gamepad calibration:

```text
"gamepad-controller-calibration"
"controller-<family>.cal"
"controller-<family>-<16-digit lowercase hex key>.cal"
```

Calibration uses float stream precision `9`.

FNV-1a offline XUID:

```text
1469598103934665603ull
1099511628211ull
0x1100001000000000ull | (hash & 0x0000FFFFFFFFFFFFull)
```

CRC:

```text
0xEDB88320u
ps_input_crc_seed = 0xA1
ps_output_crc_seed = 0xA2
ps_feature_crc_seed = 0xA3
```

Controller reports:

```text
DualShock 4 USB: report 0x01, 64 bytes
DualShock 4 Bluetooth: report 0x11, 78 bytes
DualSense USB: report 0x01, 64 bytes
DualSense Bluetooth: report 0x31, 78 bytes
```

# 8. Test Contracts

Tests must verify:

- Premake hash verification and fallback selection.
- Version extraction, invalid tags, empty tags, and default `"0.0.1"`.
- Offline flag recognition and multiplayer-marker validation.
- Component registration, ordering, idempotence, exception wrapping, unsupported filtering, and cleanup.
- Fixed ABI sizes, offsets, enum values, and exported xlive ordinals.
- Entity parsing, quoting errors, lowercase keys, serialization, and mutation filters.
- Bot target expiry, visibility fallback, stuck recovery, health synchronization, and hook restoration.
- Console command argument access, integer parsing, memory dumping, color stripping, autocomplete ranking, history limits, and output classification.
- Mouse hook readiness, raw-input registration, cursor handling, callsite restoration, and foreground gating.
- Dvar registration, replacement, domains, flags, serialization, and annex defaults.
- Filesystem path normalization, `.cfg` extension insertion, override precedence, and cleanup.
- Fastfile path recognition, fallback errors, asset-name sentinels, and zone loading.
- Controller identity classification, capability masks, binding equality, registry generation, and notification polling.
- HID report lengths, XInput DLL probing order, missing exports, feature reports, and transport failures.
- CRC correctness, malformed report rejection, hat mapping, radial normalization, report encoding, and Bluetooth sequence wrapping.
- Calibration measurement, normalization, validation, serialization precision, device identity checks, and invalid profile rejection.
- Aim curve, graph, deadzone, low-pass, lock-on, FOV scaling, pitch inversion, and integrator edge cases.
- Mapping layouts, key names, command conversion, axis hysteresis, glyph-family selection, and unknown-layout fallback.
- Key edge detection, repeats, ownership release, movement packing, view clamping, and frame-time clamping.
- Offline profile import, title storage, XUID determinism, missing/oversized files, metadata, enumerators, and overlapped completion.
- Minidump creation, temporary-file cleanup, crash dialogs, harmless exception filtering, and exit behavior.

# 9. Build Plan

## Phase 1: Platform and shared support

Defines:

- `gamepad::unstable::function_ref`
- `clock`
- `timestamp`
- `seconds`
- `milliseconds`
- `latency_span`
- `facility`
- `severity`
- `errc`
- `diagnostic`
- `diagnostic_sink`
- `logging_sink`
- `report`
- `utils::string`
- `utils::memory`
- `utils::io`
- `utils::thread`
- `utils::flags`

Consumes:

- None.

Produces:

- Windows/x86 platform layer.
- Standard aliases.
- String, memory, filesystem, thread, and diagnostic primitives.

## Phase 2: Game ABI and patch infrastructure

Defines:

- `game::game_offset`
- `game::symbol<T>`
- `game::dvar_s`
- `game::DvarValue`
- `game::DvarLimits`
- `game::XAssetHeader`
- `dvars::dvar_spec`
- `dvars::make_bool`
- `dvars::make_int`
- `dvars::make_float`
- `dvars::make_string`
- `dvars::Dvar_RegisterBool`
- `dvars::Dvar_RegisterInt`
- `dvars::Dvar_RegisterFloat`
- `dvars::Dvar_RegisterVec4`
- `dvars::Dvar_RegisterString`
- `dvars::replace_dvar`
- `dvars::replace_dvar_at`
- `utils::hook::detour`
- `utils::hook::set`
- `utils::hook::nop`
- `utils::hook::call`
- `utils::hook::jump`

Consumes:

- Phase 1 support primitives.

Produces:

- Fixed-address game bridge.
- Runtime memory patching.
- Dvar registry and typed wrappers.

## Phase 3: Component lifecycle and scheduler

Defines:

- `component_interface`
- `component_loader`
- `installer<T>`
- `get<T>()`
- `REGISTER_COMPONENT`
- `scheduler::schedule`
- `scheduler::loop`
- `scheduler::once`
- `scheduler::on_shutdown`
- `premature_shutdown_trigger`

Consumes:

- Phase 1 utilities.
- Phase 2 game ABI and hooks.

Produces:

- Component startup, loading, shutdown, scheduling, and import dispatch.

## Phase 4: Bootstrap, proxy, exceptions, and resources

Defines:

- `main`
- `DllMain`
- `sdllp::get_export`
- `sdllp::load_library`
- `sdllp::is_loaded`
- `create_minidump`
- `ID_ICON`
- `IMAGE_SPLASH`

Consumes:

- Phases 1–3.

Produces:

- Loadable `d3d9.dll`.
- Crash handling.
- Resource interception.
- System-library forwarding.

## Phase 5: Device-neutral controller samples

Defines:

- `button`
- `button_set`
- `stick`
- `stick_vector`
- `stick_sample`
- `trigger_side`
- `trigger_sample`
- `motion_sample`
- `touch_point`
- `touchpad`
- `battery_state`
- `raw_sample`
- `canonical_sample`
- `input_frame`
- `frame_ring<Capacity>`

Consumes:

- Phase 1 support and diagnostics.

Produces:

- Device-neutral sample and frame transport types.

## Phase 6: Device discovery and transport

Defines:

- `capability`
- `capabilities`
- `device_id`
- `transport_kind`
- `connection`
- `user_index`
- `report_id`
- `device_identity`
- `family`
- `transport_binding`
- `device_connection`
- `registry`
- `discovery`
- `hid_device`
- `device_notifier`
- `xinput_module`

Consumes:

- Phases 1 and 5.

Produces:

- HID/XInput discovery, stable device identity, capabilities, and registry synchronization.

## Phase 7: Drivers and calibration

Defines:

- `driver`
- `max_reports_per_poll`
- `set`
- `decode_xinput`
- `decode_dualshock4`
- `decode_dualsense`
- `decode_dualsense_edge`
- `crc32_le`
- `verify_ps_crc32`
- `encode_dualshock4_output`
- `encode_dualsense_output`
- `profile`
- `stick_calibration`
- `trigger_calibration`
- `motion_calibration`
- `calibration::validate`
- `calibration::store`
- `calibration::apply`

Consumes:

- Phases 1, 5, and 6.

Produces:

- Native report decoding, output encoding, calibration, and persistence.

## Phase 8: Aim and mapping

Defines:

- `degrees`
- `radians`
- `deg_per_s`
- `deg_per_s2`
- `axis_input`
- `magnitude`
- `screen_vector`
- `world_vector`
- `curve_kind`
- `response_curve`
- `deadzone_params`
- `knot`
- `aim_graph`
- `low_pass`
- `turn_integrator`
- `aim_calibration`
- `aim_processor`
- `engine_key`
- `action`
- `physical_input`
- `stick_layout`
- `binding_table`
- `glyph_family`
- `controller_command_for`

Consumes:

- Phases 5 and 7.

Produces:

- Deterministic calibrated aim, mapping, layouts, glyphs, and command bindings.

## Phase 9: Engine controller integration

Defines:

- `register_commands`
- `register_dvars`
- `install`
- `key_dispatcher`
- `view_driver`
- `bind_bridge`
- `move_changed`
- `pack_move`
- `unpack_move`
- `gamepad::is_controller_active`
- `gamepad::should_hide_cursor`
- `gamepad::note_mouse_activity`

Consumes:

- Phases 2, 3, 5, 7, and 8.

Produces:

- Engine key, movement, view, dvar, command, and controller ownership integration.

## Phase 10: Game components and offline LIVE

Defines:

- `Entities`
- `patches::enforce_ads_sprint_interrupt`
- `xlive::apply_early`
- All `xlive_X*` exports.
- `offline_xuid`
- `offline_name`
- `title_storage_path`
- `sanitize_storage_name`

Consumes:

- Phases 2–9.

Produces:

- Multiplayer runtime features.
- Bot and asset components.
- Offline profile, session, notification, networking, and title-storage compatibility.

## Phase 11: Build and packaging

Defines:

- `generate-buildinfo`
- `premake5.lua` project generation.
- `generate.bat`
- `generate-nightly.bat`
- `generate-linux.sh`
- `fixdeps.sh`
- `resource.rc`
- `xlive.def`

Consumes:

- All previous phases.

Produces:

- `build/consolation-client.sln`.
- `d3d9.dll`.
- `xlive.dll`.
- Version files and release package.

# 10. Prompt Templates & System Instructions

No AI prompt templates or system-prompt annex files were supplied. The supplied annex contains reproduction-critical C++ source for dvar registration, not an AI prompt template.

# 11. IDE Integration & Installer

No IDE integration templates, installer permission lists, or platform configuration annex files were supplied.

The documented launch contract is:

```text
JB_Launcher_s.exe -multiplayer
```

The documented installation path is:

```text
C:\Program Files (x86)\Activision\Quantum of Solace(TM)\
```

The Linux build workflow is:

```text
sync_submodules
gen_vscode
gen_bbuild
compile
print_summary
```

The generated solution target is:

```text
build/consolation-client.sln
Platform=Win32
```

# 12. File Manifest

## Root

| Relative path | Module | Public exports |
|---|---|---|
| `Directory.Build.props` | Build configuration | MSBuild include paths and warning settings |
| `premake5.lua` | Build generation | Workspace/project definitions |
| `generate.bat` | Installer/build | Premake bootstrap |
| `generate-nightly.bat` | Installer/build | Forwarding wrapper |
| `generate-linux.sh` | Installer/build | Wine/MSBuild bootstrap and compilation |
| `fixdeps.sh` | Dependency management | Submodule reconstruction |
| `README.md` | Documentation | Installation and usage documentation |
| `LICENSE` | Licensing | CC BY-NC 4.0 terms |

## Required files

| Relative path | Module | Public exports |
|---|---|---|
| `required_files/README.txt` | Installation | Launch and Patch 1.1 instructions |
| `required_files/Launch Consolation.lnk` | Installation | Shell shortcut; binary asset |

## Offline LIVE

| Relative path | Module | Public exports |
|---|---|---|
| `src_xlive/xlive_shim.cpp` | Offline LIVE | All implemented `xlive_X*` functions |
| `src_xlive/xlive.def` | Offline LIVE ABI | Fixed-ordinal `xlive_X*` exports |
| `src_xlive/README.md` | Offline LIVE documentation | Storage/profile documentation |

## Core source

| Relative path | Module | Public exports |
|---|---|---|
| `src/main.cpp` | Bootstrap | `main`, `DllMain` |
| `src/sdllp.cpp` | Proxy | `sdllp` APIs and D3D9 trampolines |
| `src/sdllp.hpp` | Proxy types | `sdllp` declarations |
| `src/std_include.cpp` | Platform | Linker metadata and runtime globals |
| `src/std_include.hpp` | Platform types | Shared includes |
| `src/resource.hpp` | Resources | `ID_ICON`, `IMAGE_SPLASH` |
| `src/resource.rc` | Resources | Version and embedded resources |
| `src/exception/minidump.cpp` | Exceptions | `create_minidump` |
| `src/exception/minidump.hpp` | Exceptions | Minidump declaration |
| `src/game/game.cpp` | Game bridge | Engine wrappers |
| `src/game/game.hpp` | Game bridge | Engine API declarations |
| `src/game/structs.hpp` | Game ABI | ABI structures and enums |
| `src/game/symbols.hpp` | Game ABI | Bound engine symbols |
| `src/game/dvars.cpp` | Dvars | Dvar implementation |
| `src/game/dvars.hpp` | Dvars | Dvar APIs and state |

## Loader and utilities

| Relative path | Module | Public exports |
|---|---|---|
| `src/loader/component_interface.hpp` | Loader | `component_interface` |
| `src/loader/component_loader.hpp` | Loader | `component_loader`, `installer`, `get`, registration |
| `src/loader/component_loader.cpp` | Loader | Lifecycle implementation |
| `src/utils/concurrency.hpp` | Utilities | `container<T, MutexType>` |
| `src/utils/flags.cpp` | Utilities | Flag parsing |
| `src/utils/flags.hpp` | Utilities | `has_flag` |
| `src/utils/hook.cpp` | Utilities | Hook implementation |
| `src/utils/hook.hpp` | Utilities | Hook APIs |
| `src/utils/io.cpp` | Utilities | Filesystem and PE I/O |
| `src/utils/io.hpp` | Utilities | I/O declarations |
| `src/utils/memory.cpp` | Utilities | Allocation and pointer validation |
| `src/utils/memory.hpp` | Utilities | Memory APIs |
| `src/utils/nt.cpp` | Utilities | PE/module/process integration |
| `src/utils/nt.hpp` | Utilities | NT APIs |
| `src/utils/string.cpp` | Utilities | String implementation |
| `src/utils/string.hpp` | Utilities | String APIs |
| `src/utils/thread.cpp` | Utilities | Thread management |
| `src/utils/thread.hpp` | Utilities | Thread APIs |

## Components

| Relative path | Module | Public exports |
|---|---|---|
| `src/component/bots.cpp` | Bots | `bots::component`, bot hooks |
| `src/component/entities.cpp` | Entities | `Entities` methods |
| `src/component/entities.hpp` | Entities | `Entities` declarations |
| `src/component/engine/console/command.cpp` | Console commands | `command` implementation |
| `src/component/engine/console/command.hpp` | Console commands | `command::params`, command APIs |
| `src/component/engine/console/console.cpp` | Console | `console::component` |
| `src/component/engine/console/console.hpp` | Console | Logging APIs |
| `src/component/engine/console/game_console.cpp` | Game console | Overlay implementation |
| `src/component/engine/console/game_console.hpp` | Game console | Overlay APIs |
| `src/component/engine/mouse_input/mouse_input.cpp` | Mouse input | Mouse hooks |
| `src/component/engine/patches/patches.cpp` | Patches | Patch component |
| `src/component/engine/patches/patches.hpp` | Patches | `enforce_ads_sprint_interrupt` |
| `src/component/engine/patches/profile_patches.cpp` | Profiles | Profile component |
| `src/component/engine/patches/xlive.cpp` | LIVE patches | `xlive::apply_early` implementation |
| `src/component/engine/patches/xlive.hpp` | LIVE patches | `xlive::apply_early` |
| `src/component/engine/renderer/draw_version.cpp` | Renderer | Version overlay |
| `src/component/engine/renderer/ultrawide.cpp` | Renderer | Ultrawide component |
| `src/component/engine/renderer/ultrawide.hpp` | Renderer | Ultrawide commands |
| `src/component/engine/scripting/filesystem.cpp` | Filesystem | Filesystem component |
| `src/component/engine/scripting/filesystem.hpp` | Filesystem | File APIs |
| `src/component/engine/scripting/gametypes.cpp` | Gametypes | Gametype component |
| `src/component/engine/scripting/gametypes.hpp` | Gametypes | Gametype refresh/debug APIs |
| `src/component/engine/scripting/gsc.cpp` | GSC | GSC component |
| `src/component/engine/scripting/gsc.hpp` | GSC | Function/method registration |
| `src/component/engine/ux/sounds.cpp` | Audio | Sounds component |
| `src/component/engine/zones/fastfiles.cpp` | Fastfiles | Fastfile component |
| `src/component/engine/zones/fastfiles.hpp` | Fastfiles | `fastfiles::enum_assets` |
| `src/component/utils/exception.cpp` | Exceptions | Exception component |
| `src/component/utils/resources.cpp` | Resources | Resource component |
| `src/component/utils/scheduler.cpp` | Scheduler | Scheduler implementation |
| `src/component/utils/scheduler.hpp` | Scheduler | Scheduler API |
| `src/component/gamepad/controller_input.cpp` | Gamepad | Controller component |
| `src/component/gamepad/gamepad.hpp` | Gamepad | Controller-state APIs |
| `src/component/gamepad/sdl_input.cpp` | Gamepad | Inert SDL component |

## Controller framework

| Relative path | Module | Public exports |
|---|---|---|
| `src/component/gamepad/controller/clock.hpp` | Timing | Clock types |
| `src/component/gamepad/controller/clock.cpp` | Timing | Clock implementation |
| `src/component/gamepad/controller/diagnostic.hpp` | Diagnostics | Diagnostic types |
| `src/component/gamepad/controller/diagnostic.cpp` | Diagnostics | Diagnostic implementation |
| `src/component/gamepad/controller/error.hpp` | Errors | `errc`, `error` |
| `src/component/gamepad/controller/error.cpp` | Errors | Error implementation |
| `src/component/gamepad/controller/context.hpp` | Context | Context API |
| `src/component/gamepad/controller/context.cpp` | Context | Context implementation |
| `src/component/gamepad/controller/runtime.hpp` | Runtime | `runtime` |
| `src/component/gamepad/controller/runtime.cpp` | Runtime | Runtime implementation |
| `src/component/gamepad/controller/trace.hpp` | Instrumentation | Trace macros |
| `src/component/gamepad/controller/aim/*.hpp` | Aim | Aim types |
| `src/component/gamepad/controller/aim/*.cpp` | Aim | Aim implementation |
| `src/component/gamepad/controller/calibration/*.hpp` | Calibration | Calibration types |
| `src/component/gamepad/controller/calibration/*.cpp` | Calibration | Calibration implementation |
| `src/component/gamepad/controller/device/*.hpp` | Devices | Device types |
| `src/component/gamepad/controller/device/*.cpp` | Devices | Device implementation |
| `src/component/gamepad/controller/driver/*.hpp` | Drivers | Driver types |
| `src/component/gamepad/controller/driver/*.cpp` | Drivers | Driver implementation |
| `src/component/gamepad/controller/engine/*.hpp` | Engine input | Engine integration types |
| `src/component/gamepad/controller/engine/*.cpp` | Engine input | Engine integration implementation |
| `src/component/gamepad/controller/mapping/*.hpp` | Mapping | Mapping types |
| `src/component/gamepad/controller/mapping/*.cpp` | Mapping | Mapping implementation |
| `src/component/gamepad/controller/sample/*.hpp` | Samples | Sample types |
| `src/component/gamepad/controller/sample/*.cpp` | Samples | Sample implementation |
| `src/component/gamepad/controller/support/*.hpp` | Support | Shared aliases and `function_ref` |
| `src/component/gamepad/controller/transport/*.hpp` | Transport | Transport types |
| `src/component/gamepad/controller/transport/*.cpp` | Transport | Transport implementation |

## Reproduction-critical annex

The supplied `src/game/dvars.annex.sum` contains the complete dvar implementation source and must be treated as authoritative for exact dvar names, defaults, descriptions, flags, registration order, formatting, and shutdown behavior.