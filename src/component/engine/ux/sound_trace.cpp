#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "component/engine/console/command.hpp"
#include "component/engine/console/console.hpp"
#include "game/game.hpp"
#include "utils/hook.hpp"
#include <intrin.h>
#include <charconv>
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <sstream>

#ifdef DEBUG
// Temporary, opt-in QoS 1.1 probe. Remove once custom-zone silence is understood.
// QoS PC: SND_Update 10402A30, milesGlob 113ED3A8, chaninfo 106FB430/148.
// Compared with KisakCOD snd.cpp/snd_driver.cpp; Miles private offsets below
// were checked against the INSTALLED mss32.dll, not copied from its older header.
// This observes state only: no forced success, volume changes, restarts or map rules.
namespace sound_trace
{
	namespace
	{
		using sample_function = void (__stdcall*)(uintptr_t);
		utils::hook::detour update_hook, resume_hook, end_hook, stop_hook;
		bool installed{};
		std::atomic_bool recording{false};
		std::atomic<DWORD> owner{0};
		ULONGLONG started{}, next_frame{}, duration{};
		struct record
		{
			unsigned ms{}, caller{}, status{}, position{}, length{}, mixed{}, paused{}, muted{};
			int channel{-1};
			float left{}, right{};
			char event{}, alias[64]{};
		};
		std::array<record, 4096> records;
		std::array<unsigned, 40> previous_status;
		size_t count{};
		bool truncated{};

		template<typename T> T read(const uintptr_t address)
		{
			return *reinterpret_cast<const T*>(address);
		}

		// Keep SEH in a POD-only leaf: the native DB/audio owners may tear down.
		bool snapshot(record* const out, const uintptr_t sample, const int channel)
		{
			__try
			{
				if (read<unsigned>(sample) != 0x4D415348) return false; // HSAM
				const auto driver = read<uintptr_t>(sample + 4);
				const auto index = read<unsigned>(sample + 8);
				if (read<unsigned>(driver) != 0x47494448 || index >= 128) return false; // HDIG
				out->status = read<unsigned>(read<uintptr_t>(driver + 0xD10) + index * 4);
				out->left = read<float>(sample + 0x64);
				out->right = read<float>(sample + 0x68);
				const auto buffer = read<uintptr_t>(sample + 12);
				const auto buffer_index = read<unsigned>(sample + 0x24);
				if (buffer && buffer_index < 2)
				{
					out->length = read<unsigned>(buffer + buffer_index * 24 + 4);
					out->position = read<unsigned>(buffer + buffer_index * 24 + 8);
				}
				// Actual Miles software build buffer: 32-bit mixer values, not DAC samples.
				const auto build = read<uintptr_t>(driver + 0xD38);
				const auto bytes = read<unsigned>(driver + 0xD3C);
				if (build && bytes <= 8192 && bytes % 4 == 0)
					for (unsigned i = 0; i < bytes; i += 4)
						out->mixed += read<int>(build + i) != 0;
				out->paused = read<unsigned char>(game::game_offset(0x106EDDEA));
				const auto mute = read<uintptr_t>(game::game_offset(0x10711AAC));
				out->muted = mute ? read<unsigned>(mute + 16) : 0;
				if (channel >= 0)
				{
					const auto info = game::game_offset(0x106FB430) + channel * 148;
					const auto alias = read<uintptr_t>(info + 92);
					const auto name = alias ? read<uintptr_t>(alias) : 0;
					if (name)
						for (unsigned i = 0; i + 1 < sizeof(out->alias); ++i)
						{
							out->alias[i] = read<char>(name + i);
							if (!out->alias[i]) break;
						}
				}
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		int find_channel(const uintptr_t sample)
		{
			for (int i = 0; i < 40; ++i)
				if (read<uintptr_t>(game::game_offset(0x113ED3AC) + i * 4) == sample) return i;
			return -1;
		}

		void capture(const char event, const uintptr_t sample, const uintptr_t caller)
		{
			if (GetCurrentThreadId() != owner || !recording) return;
			if (count == records.size()) { truncated = true; return; }
			record value{};
			value.event = event;
			value.ms = static_cast<unsigned>(GetTickCount64() - started);
			value.caller = caller ? static_cast<unsigned>(caller - game::game_offset(0x10000000) + 0x10000000) : 0;
			value.channel = find_channel(sample);
			if (snapshot(&value, sample, value.channel)) records[count++] = value;
		}

		void __stdcall resume_stub(const uintptr_t sample)
		{
			reinterpret_cast<sample_function>(resume_hook.get_original())(sample);
			capture('R', sample, reinterpret_cast<uintptr_t>(_ReturnAddress()));
		}
		void __stdcall end_stub(const uintptr_t sample)
		{
			capture('E', sample, reinterpret_cast<uintptr_t>(_ReturnAddress()));
			reinterpret_cast<sample_function>(end_hook.get_original())(sample);
		}
		void __stdcall stop_stub(const uintptr_t sample)
		{
			capture('P', sample, reinterpret_cast<uintptr_t>(_ReturnAddress()));
			reinterpret_cast<sample_function>(stop_hook.get_original())(sample);
		}

		void report()
		{
			recording = false;
			std::ostringstream csv;
			csv << "ms,event,channel,caller_ida,status,position,length,left,right,mixer_nonzero,paused,muted,alias\n";
			for (size_t i = 0; i < count; ++i)
			{
				const auto& r = records[i];
				csv << r.ms << ',' << r.event << ',' << r.channel << ',' << std::hex << r.caller << std::dec
					<< ',' << r.status << ',' << r.position << ',' << r.length << ',' << r.left << ',' << r.right
					<< ',' << r.mixed << ',' << r.paused << ',' << r.muted << ',' << r.alias << '\n';
			}
			const auto path = std::filesystem::path("consolation") / std::format("sound-trace-{}-{}.csv", GetCurrentProcessId(), GetTickCount64());
			const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) { console::warn("snd_trace: could not create report (error %lu); records retained\n", GetLastError()); return; }
			const auto text = csv.str();
			DWORD written{};
			const auto ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
			CloseHandle(file);
			if (!ok || written != text.size()) { console::warn("snd_trace: incomplete report; records retained\n"); return; }
			console::info("snd_trace: %u records%s saved to %s (R=resume E=end P=pause F=frame; mixer values are not proof of audible output)\n",
				static_cast<unsigned>(count), truncated ? " (capacity reached)" : "", path.string().c_str());
		}

		void __cdecl update_stub()
		{
			update_hook.invoke<void>();
			if (GetCurrentThreadId() != owner || !recording) return;
			if (truncated) { report(); return; }
			const auto now = GetTickCount64();
			if (now - started >= duration) { report(); return; }
			if (now < next_frame) return;
			next_frame = now + 50;
			for (int i = 0; i < 40 && recording; ++i)
			{
				const auto sample = read<uintptr_t>(game::game_offset(0x113ED3AC) + i * 4);
				record value{};
				if (!sample || !snapshot(&value, sample, i)) continue;
				if (value.status != 2 || previous_status[i] != value.status) capture('F', sample, 0);
				previous_status[i] = value.status;
			}
			if (truncated) report();
		}

		bool install()
		{
			if (installed) return true;
			// Gate the native owner and installed Miles exports before creating hooks.
			const auto update = game::game_offset(0x10402A30);
			constexpr unsigned char prefix[]{0x83, 0xEC, 0x1C, 0x80, 0x3D};
			if (std::memcmp(reinterpret_cast<const void*>(update), prefix, sizeof(prefix))) return false;
			const auto miles = GetModuleHandleW(L"mss32.dll");
			if (!miles) return false;
			const auto base = reinterpret_cast<uintptr_t>(miles);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
			const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
			if (pe->Signature != IMAGE_NT_SIGNATURE || pe->FileHeader.Machine != IMAGE_FILE_MACHINE_I386
				|| pe->FileHeader.TimeDateStamp != 0x47F1E305 || pe->OptionalHeader.SizeOfImage != 0x99000) return false;
			const auto resume = GetProcAddress(miles, "_AIL_resume_sample@4");
			const auto end = GetProcAddress(miles, "_AIL_end_sample@4");
			const auto stop = GetProcAddress(miles, "_AIL_stop_sample@4");
			if (!resume || !end || !stop) return false;
			try
			{
				resume_hook.create(reinterpret_cast<void*>(resume), resume_stub);
				end_hook.create(reinterpret_cast<void*>(end), end_stub);
				stop_hook.create(reinterpret_cast<void*>(stop), stop_stub);
				update_hook.create(update, update_stub);
				installed = true;
			}
			catch (...) { update_hook.clear(); stop_hook.clear(); end_hook.clear(); resume_hook.clear(); }
			return installed;
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			command::add("snd_trace", [](const command::params& args)
			{
				if (args.size() == 2 && std::string_view(args[1]) == "stop") { report(); return; }
				unsigned seconds = 5;
				bool valid = args.size() >= 2 && args.size() <= 3 && std::string_view(args[1]) == "start";
				if (valid && args.size() == 3)
				{
					const std::string_view text(args[2]);
					const auto parsed = std::from_chars(text.data(), text.data() + text.size(), seconds);
					valid = parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
				}
				if (!valid || seconds < 1 || seconds > 15) { console::info("snd_trace start [seconds: 1..15] | stop (Debug diagnostic only)\n"); return; }
				if (recording) { console::warn("snd_trace: recording already active; use stop before restarting\n"); return; }
				if (!install()) { console::warn("snd_trace: native/Miles hooks unavailable; no capture started\n"); return; }
				owner = GetCurrentThreadId(); count = 0; truncated = false; previous_status.fill(2);
				started = next_frame = GetTickCount64(); duration = seconds * 1000ULL; recording = true;
				console::info("snd_trace: recording %u seconds; close console and fire/move. Auto-saves under consolation/. No audio state is overridden.\n", seconds);
			});
		}
		void pre_destroy() override
		{
			recording = false;
			update_hook.clear(); stop_hook.clear(); end_hook.clear(); resume_hook.clear();
		}
	};
}
REGISTER_COMPONENT(sound_trace::component)
#endif
