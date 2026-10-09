#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "component/engine/console/command.hpp"
#include "component/engine/console/console.hpp"
#include "component/utils/scheduler.hpp"
#include "game/game.hpp"
#include <utils/hook.hpp>
#include <d3d9.h>
#include "aa_selection.hpp"

namespace renderer_quality
{
	namespace
	{
		std::uintptr_t native_aa_setup{};
		bool installed{};
		constexpr std::array<unsigned char, 5> original_call{0xE8, 0x64, 0xFC, 0xFF, 0xFF};
		std::array<unsigned char, 5> installed_call{};

		template <typename T> T& engine_value(const std::uintptr_t address)
		{
			return *reinterpret_cast<T*>(game::game_offset(address));
		}

		unsigned query_levels(const int samples, const bool depth, const bool windowed)
		{
			auto* const d3d = engine_value<IDirect3D9*>(0x10E24504);
			if (!d3d) return 0;
			DWORD levels{};
			// QoS PC 1.1 R_SetupAntiAliasing (103BCC20): adapter 0, HAL,
			// A8R8G8B8, !wndParms->fullscreen. R_CreateDevice (103BD940)
			// selects the actual depth format at 10E24510 (D24FS8 or D24S8).
			const auto format = depth ? engine_value<D3DFORMAT>(0x10E24510) : D3DFMT_A8R8G8B8;
			return SUCCEEDED(d3d->CheckDeviceMultiSampleType(0, D3DDEVTYPE_HAL,
				format, windowed, static_cast<D3DMULTISAMPLE_TYPE>(samples), &levels)) ? levels : 0;
		}

		void __cdecl setup_extended_aa(const unsigned char* const window_parms)
		{
			// Prefix offsets verified in QoS; do not import the whole COD4 struct.
			const int requested = *reinterpret_cast<const int*>(window_parms + 40);
			const bool windowed = window_parms[8] == 0;
			const int selected = select_extended_samples(requested,
				[windowed](const int samples, const bool depth)
				{ return query_levels(samples, depth, windowed); });
			engine_value<D3DMULTISAMPLE_TYPE>(0x10E27124) = static_cast<D3DMULTISAMPLE_TYPE>(selected);
			// Quality 0 is valid for both checked surfaces. QoS also passes this
			// global into non-MSAA auxiliary depth surfaces (1038E820/1038E640),
			// so avoid driver-specific nonzero quality indices for extended modes.
			engine_value<DWORD>(0x10E27128) = 0;
			console::info("[renderer] AA requested=%dx selected=%dx quality=0 (%s, color+depth checked)\n",
				requested, selected ? selected : 1, windowed ? "windowed" : "fullscreen");
		}

		__declspec(naked) void setup_aa_stub()
		{
			// QoS caller 103BCFB7 supplies GfxWindowParms in EDI. Keep the
			// original selector/quality behavior for every stock setting (1..4).
			__asm
			{
				cmp dword ptr [edi + 40], 4
				jg extended
				jmp native_aa_setup
			extended:
				pushfd
				pushad
				push edi
				call setup_extended_aa
				add esp, 4
				popad
				popfd
				ret
			}
		}

		void graphics_info()
		{
			const auto* aa = game::Dvar_FindVar("r_aaSamples");
			const auto* min_af = game::Dvar_FindVar("r_texFilterAnisoMin");
			const auto* max_af = game::Dvar_FindVar("r_texFilterAnisoMax");
			const bool windowed = engine_value<int>(0x10E271A8) == 0;
			console::info("gfxinfo: D3D9, extended AA hook=%d, requested=%d pending=%d actual=%d quality=%u depthFormat=%u\n",
				installed, aa ? aa->current.integer : 1, aa ? aa->latched.integer : 1,
				static_cast<int>(engine_value<D3DMULTISAMPLE_TYPE>(0x10E27124)),
				engine_value<DWORD>(0x10E27128), static_cast<unsigned>(engine_value<D3DFORMAT>(0x10E24510)));
			console::info("gfxinfo: AF min=%d max=%d hardwareMax=%d effectiveMipped=%d minFilter=%d magFilter=%d\n",
				min_af ? min_af->current.integer : 1, max_af ? max_af->current.integer : 1,
				engine_value<int>(0x10E272F0), engine_value<int>(0x10E2717C),
				engine_value<int>(0x10E2716C), engine_value<int>(0x10E27170));
			for (int samples = 2; samples <= 16; ++samples)
			{
				const auto color = query_levels(samples, false, windowed);
				const auto depth = query_levels(samples, true, windowed);
				if (color && depth) console::info("gfxinfo: supported=%dx colorLevels=%u depthLevels=%u (%s)\n",
					samples, color, depth, windowed ? "windowed" : "fullscreen");
			}
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			const auto limit = game::game_offset(0x103AFC41);
			const auto call = game::game_offset(0x103BCFB7);
			// Paired compatibility gate: never unlock modes without their selector.
			if (std::memcmp(reinterpret_cast<const void*>(limit), "\x6A\x04", 2) == 0
				&& std::memcmp(reinterpret_cast<const void*>(call), original_call.data(), original_call.size()) == 0)
			{
				native_aa_setup = game::game_offset(0x103BCC20);
				utils::hook::call(call, setup_aa_stub);
				std::memcpy(installed_call.data(), reinterpret_cast<const void*>(call), installed_call.size());
				utils::hook::set<unsigned char>(limit + 1, 16);
				installed = true;
			}
			else console::warn("[renderer] extended AA skipped: QoS PC 1.1 patch bytes do not match\n");
			// No default changes, device replacement, or global sampler overrides.
			// Native AF already clamps to hardware limits (103AE5E0), like
			// KisakCOD r_state.cpp R_SetTexFilter; retain its material policy.
			scheduler::once([] { command::add("gfxinfo", graphics_info); }, scheduler::main);
		}

		void pre_destroy() override
		{
			if (!installed) return;
			const auto call = game::game_offset(0x103BCFB7);
			if (std::memcmp(reinterpret_cast<const void*>(call), installed_call.data(), installed_call.size()) == 0)
			{
				utils::hook::set(call, original_call);
				const auto limit = game::game_offset(0x103AFC42);
				if (engine_value<unsigned char>(0x103AFC42) == 16) utils::hook::set<unsigned char>(limit, 4);
			}
			installed = false;
		}
	};
}

REGISTER_COMPONENT(renderer_quality::component)
