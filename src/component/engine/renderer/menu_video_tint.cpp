#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/game.hpp"
#include "game/dvars.hpp"
#include "component/engine/console/console.hpp"
#include "component/utils/scheduler.hpp"
#include "menu_video_tint_policy.hpp"
#include <utils/hook.hpp>
#include <d3d9.h>
#include <array>

namespace menu_video_tint
{
	namespace
	{
		game::dvar_s* menu_wine_tint{};
		bool installed{};
		bool cinematic_installed{};
		int cinematic_reported_state{-1};
		unsigned int cinematic_reports{};
		constexpr std::array<unsigned char, 5> cinematic_call_bytes{0xE8, 0x1E, 0x44, 0x00, 0x00};
		bool reported_sampler_failure{};
		constexpr std::array<unsigned char, 6> native_bytes{0x8B, 0x88, 0xB4, 0x01, 0x00, 0x00};

		// Diagnostic probe (QoS PC 1.1): proves whether the native Bink
		// background upload reaches the adapter and which tint was applied.
		// Readable from a dump via the PDB; remove once the tint is verified.
		volatile unsigned int upload_calls{};
		volatile unsigned int upload_tinted{};
		volatile float last_red_row[4]{};
		int reported_state{-1};
		unsigned int reports{};

		char __cdecl draw_cinematic_background(const float x, const float y, const float width, const float height,
			const float s0, const float t0, const float s1, const float t1,
			float* color, game::Material* material, const int flags)
		{
			// QoS PC 1.1 102DBB90 -> 103BC2E0 -> 103BC3FD. This is the
			// visible native menu movie; 10412460 instead updates a DIFFERENT
			// Bink handle/texture for Scaleform. Compare KisakCOD's cinematic
			// draw: retain its rectangle, UVs, material, flags and colour alpha.
			const auto* in_game = game::Dvar_FindVar("cl_ingame");
			const auto* movie = reinterpret_cast<const char*>(game::game_offset(0x10E29310));
			const auto length = strnlen_s(movie, 256);
			const auto* cinematic = *reinterpret_cast<game::Material* const*>(game::game_offset(0x10C4A364));
			const bool enabled = menu_wine_tint && menu_wine_tint->current.enabled;
			const std::array<float, 4> native = color
				? std::array<float, 4>{color[0], color[1], color[2], color[3]}
				: std::array<float, 4>{1, 1, 1, 1};
			auto adjusted = background_color(native, enabled, !in_game || in_game->current.enabled,
				std::string_view(movie, length), cinematic && material == cinematic);
			const bool tinted = adjusted != native;
			if (cinematic_reported_state != static_cast<int>(tinted) && cinematic_reports < 8)
			{
				cinematic_reported_state = static_cast<int>(tinted);
				++cinematic_reports;
				console::info("[renderer] visible menu cinematic: tint=%d rgba=(%.3f %.3f %.3f %.3f)\n",
					tinted ? 1 : 0, adjusted[0], adjusted[1], adjusted[2], adjusted[3]);
			}
			// Native R_AddCmdDrawStretchPic copies RGBA into the command buffer
			// before returning (103C094E..103C0960); no pointer escapes this call.
			return utils::hook::invoke<char>(game::game_offset(0x103C0820), x, y, width, height,
				s0, t0, s1, t1, tinted ? adjusted.data() : color, material, flags);
		}

		void install_cinematic()
		{
			const auto site = game::game_offset(0x103BC3FD);
			if (std::memcmp(reinterpret_cast<const void*>(site), cinematic_call_bytes.data(), cinematic_call_bytes.size()) != 0)
			{
				console::warn("[renderer] visible menu cinematic tint skipped: unexpected engine instructions\n");
				return;
			}
			// Replace this one existing cdecl CALL, not the global draw routine.
			// Undo in pre_destroy; remove once the native movie colour is authored.
			utils::hook::call(site, &draw_cinematic_background);
			cinematic_installed = true;
			console::info("[renderer] visible menu cinematic tint installed at 0x103BC3FD\n");
		}

		HRESULT __stdcall upload_constants(IDirect3DDevice9* device, const UINT first,
			const float* values, const UINT count)
		{
			// QoS 10412460 -> 10412B20 is the dedicated bink_bkg_loop_1
			// background pass. Its direct SetTexture calls bypass R_SetSampler;
			// its setup 10413160 sets filters/addressing but never sRGB decoding,
			// so YUV planes are forced linear here. Runs on the native video
			// render path; no native matrix/global is modified.
			if (!device || !values) return D3DERR_INVALIDCALL;
			if (first != 0 || count != 4) return device->SetPixelShaderConstantF(first, values, count);
			for (UINT stage = 0; stage < 4; ++stage)
			{
				const auto result = device->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
				if (FAILED(result) && !reported_sampler_failure)
				{
					reported_sampler_failure = true;
					console::warn("[renderer] menu video linear sampling failed (0x%08X)\n",
						static_cast<unsigned int>(result));
				}
			}
			std::array<float, 16> native{};
			std::copy_n(values, native.size(), native.begin());
			const bool tint = menu_wine_tint && menu_wine_tint->current.enabled;
			const auto adjusted = constants(native, tint);
			++upload_calls;
			if (tint) ++upload_tinted;
			for (unsigned i = 0; i < 4; ++i) last_red_row[i] = adjusted[i];
			const int state = !menu_wine_tint ? 2 : tint ? 1 : 0;
			if (state != reported_state && reports < 8)
			{
				reported_state = state;
				++reports;
				console::info("[renderer] offscreen Scaleform video adapter active: dvar=%s tint=%d red row=(%.3f %.3f %.3f %.3f)\n",
					menu_wine_tint ? "registered" : "missing", tint ? 1 : 0,
					adjusted[0], adjusted[1], adjusted[2], adjusted[3]);
			}
			return device->SetPixelShaderConstantF(first, adjusted.data(), count);
		}

		void install()
		{
			// Replace only MOV ECX,[EAX+1B4h] (IDirect3DDevice9 constant upload)
			// with MOV ECX,adapter / NOP. The original stdcall argument pushes,
			// CALL ECX and stack cleanup remain unchanged. PC 1.1 only.
			const auto site = game::game_offset(0x10412B88);
			if (std::memcmp(reinterpret_cast<const void*>(site), native_bytes.data(), native_bytes.size()) != 0)
			{
				console::warn("[renderer] menu video adapter skipped: unexpected engine instructions\n");
				return;
			}
			std::array<unsigned char, 6> replacement{0xB9, 0, 0, 0, 0, 0x90};
			const auto target = reinterpret_cast<std::uintptr_t>(&upload_constants);
			static_assert(sizeof(target) == 4);
			std::memcpy(replacement.data() + 1, &target, sizeof(target));
			utils::hook::set(site, replacement);
			installed = true;
			console::info("[renderer] menu video adapter installed at 0x10412B88\n");
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			install_cinematic();
			install();
			scheduler::once([]
			{
				menu_wine_tint = dvars::Dvar_RegisterBool("ui_menuWineTint", true,
					"Wine-red (#952F48) menu background movie. Preserves video detail/fades; does not recolour gray panels, text, previews or gameplay.",
					game::dvar_flags::saved);
			}, scheduler::main);
		}

		void pre_destroy() override
		{
			if (cinematic_installed)
				utils::hook::set(game::game_offset(0x103BC3FD), cinematic_call_bytes);
			cinematic_installed = false;
			if (installed)
				utils::hook::set(game::game_offset(0x10412B88), native_bytes);
			installed = false;
			menu_wine_tint = nullptr;
		}
	};
}

REGISTER_COMPONENT(menu_video_tint::component)
