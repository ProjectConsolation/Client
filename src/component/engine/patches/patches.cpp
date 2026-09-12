#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "component/engine/console/console.hpp"
#include "component/engine/console/command.hpp"
#include "component/utils/scheduler.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include <utils/hook.hpp>
#include <utils/flags.hpp>
#include <utils/string.hpp>
#include <cstring>
#include <stdexcept>
#include <unordered_set>
#include <d3d9.h>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")
#ifndef VERSION_BUILD
#define VERSION_BUILD "0"
#endif

//#define XLIVELESS

namespace patches
{
	void enforce_ads_sprint_interrupt(game::usercmd_t* cmd)
	{
		if (!cmd)
		{
			return;
		}

		if ((cmd->buttons & game::BUTTON_ADS) != 0)
		{
			cmd->buttons = static_cast<game::usercmd_buttons>(cmd->buttons & ~game::BUTTON_SPRINT);
		}
	}

	namespace
	{
		constexpr std::size_t k_huffman_max_decoded_bytes = 0x20000;
		constexpr std::size_t k_huffman_max_compressed_bytes = k_huffman_max_decoded_bytes;
		constexpr std::size_t k_ui_replace_directive_max_len = 0x100;
		constexpr std::size_t k_party_member_join_max_message_bytes = 0x4000;

		std::size_t bounded_length(const char* value, const std::size_t max_len)
		{
			if (!value)
			{
				return 0;
			}

			std::size_t length = 0;
			while (length < max_len && value[length] != '\0')
			{
				++length;
			}

			return length;
		}

		std::string build_shortversion_string()
		{
			std::string version = VERSION_PRODUCT;

#ifdef DEBUG
			version += "-dbg";
#elif defined(NDEBUG)
			// release keeps the plain semantic version
#else
			version += "-nightly";
#endif

			return version;
		}

		std::string build_build_label()
		{
			std::string short_hash = GIT_HASH;
			if (short_hash.size() > 7)
			{
				short_hash.resize(7);
			}

			if (GIT_DIRTY)
			{
				short_hash += "-dirty";
			}

			return short_hash;
		}

		std::string build_timestamp_label()
		{
			return std::string(__DATE__) + " " + __TIME__;
		}

		std::string build_game_date_string()
		{
			return __DATE__;
		}

		std::string build_version_string()
		{
			return "Project: Consolation "
				+ build_shortversion_string()
				+ " build "
				+ build_build_label()
				+ " "
				+ build_timestamp_label()
				+ " win-x86";
		}

		int ret_one(DWORD*, int)
		{
			return 1;
		}

		void apply_cinematic_stats_guard()
		{
			// QoS 1.1: cinematic command 0x104547F0 sets state 1 via 0x10454770.
			// StatsReadComplete (0x10240C60) runs stats_init.cfg for a missing profile.
			// Allow that non-network state without changing the connection state or
			// bypassing the native stat writes. Remove when the stat command is replaced.
			const auto site = game::game_offset(0x10240FF2);
			const unsigned char expected[] = {0x8B, 0x15, 0xF8, 0x45, 0x1F, 0x11};
			// The absolute operand is relocated with jb_mp_s.dll.
			const auto state_address = static_cast<std::uint32_t>(game::game_offset(0x111F45F8));
			if (memcmp(reinterpret_cast<const void*>(site), expected, 2) != 0
				|| *reinterpret_cast<const std::uint32_t*>(site + 2) != state_address)
			{
				throw std::runtime_error("Unsupported QoS statset connection-state instruction");
			}

			auto* stub = utils::hook::assemble([state_address](utils::hook::assembler& a)
			{
				const auto original_check = a.newLabel();
				a.mov(edx, dword_ptr(state_address));
				a.cmp(edx, 1);
				a.jne(original_check);
				a.jmp(reinterpret_cast<void*>(game::game_offset(0x1024102C)));
				a.bind(original_check);
				a.jmp(reinterpret_cast<void*>(game::game_offset(0x10240FF8)));
			});
			utils::hook::nop(site, sizeof(expected));
			utils::hook::jump(site, stub);
			console::info("[patches - stats] PATCHED: profile initialization during cinematics\n");
		}

		void apply_missing_voice_engine_guard()
		{
			// QoS 1.1 faults at 0x102462F0 when XHVCreateEngine leaves a null engine.
			// The SDK vtable and COD4A Voice_EnableMic confirm this is local-talker
			// registration. Use QoS' false-return path without setting its mic flag.
			// Retire with a replacement voice lifecycle; verify LIVE entry with and
			// without an engine, preserving normal registration when one is present.
			const auto site = game::game_offset(0x102462ED);
			const unsigned char expected[] = {0x8B, 0x47, 0x60, 0x8B, 0x08};
			const auto failure = game::game_offset(0x10246320);
			const unsigned char expected_failure[] = {0x32, 0xC0, 0x5F, 0xC3};
			if (memcmp(reinterpret_cast<const void*>(site), expected, sizeof(expected)) != 0
				|| memcmp(reinterpret_cast<const void*>(failure), expected_failure, sizeof(expected_failure)) != 0)
			{
				const auto* const actual = reinterpret_cast<const unsigned char*>(site);
				console::error("[patches - voice] skipped: unsupported registration bytes at 0x102462ED "
					"(%02X %02X %02X %02X %02X)\n",
					actual[0], actual[1], actual[2], actual[3], actual[4]);
				return;
			}

			try
			{
				auto* stub = utils::hook::assemble([failure](utils::hook::assembler& a)
				{
					const auto engine_available = a.newLabel();
					a.mov(eax, dword_ptr(edi, 0x60));
					a.test(eax, eax);
					a.jnz(engine_available);
					a.jmp(reinterpret_cast<void*>(failure));
					a.bind(engine_available);
					a.mov(ecx, dword_ptr(eax));
					a.jmp(reinterpret_cast<void*>(game::game_offset(0x102462F2)));
				});
				utils::hook::jump(site, stub);

			// These QoS 1.1 sites load the engine and its vtable before a voice call.
			// Keep the native peer bookkeeping even without audio: COD4A's remote
			// registration has the same separation between voice and connectivity.
			// No local mic flag is set by the guard above; shutdown already checks
			// for null. Cover remote cleanup, status queries and incoming audio too.
			const auto guard_voice_call = [](const std::uintptr_t address,
				const unsigned char engine_operand, const unsigned char vtable_operand,
				const asmjit::x86::Gp& session, const asmjit::x86::Gp& vtable,
				const std::uintptr_t unavailable, const int stack_cleanup = 0)
			{
				const auto call_site = game::game_offset(address);
				const unsigned char instructions[] = {0x8B, engine_operand, 0x60, 0x8B, vtable_operand};
				if (memcmp(reinterpret_cast<const void*>(call_site), instructions, sizeof(instructions)) != 0)
				{
					throw std::runtime_error("Unsupported QoS voice-engine call instructions");
				}
				auto* call_stub = utils::hook::assemble([=](utils::hook::assembler& a)
				{
					const auto available = a.newLabel();
					a.mov(eax, dword_ptr(session, 0x60));
					a.test(eax, eax);
					a.jnz(available);
					if (stack_cleanup)
					{
						a.add(esp, stack_cleanup);
					}
					if (unavailable)
					{
						a.jmp(reinterpret_cast<void*>(game::game_offset(unavailable)));
					}
					else
					{
						// Engine is null, so EAX already represents false.
						a.ret();
					}
					a.bind(available);
					a.mov(vtable, dword_ptr(eax));
					a.jmp(reinterpret_cast<void*>(call_site + sizeof(instructions)));
				});
				utils::hook::jump(call_site, call_stub);
			};

			// Registration still has five Com_Printf arguments to discard here.
			guard_voice_call(0x102464B4, 0x47, 0x08, edi, ecx, 0x102464F7, 0x14);
			guard_voice_call(0x102461BE, 0x46, 0x08, esi, ecx, 0x1024624A);
			guard_voice_call(0x10245BF0, 0x40, 0x08, eax, ecx, 0); // headset present
			guard_voice_call(0x10245E83, 0x40, 0x10, eax, edx, 0); // local talking
			guard_voice_call(0x10245ECF, 0x47, 0x10, edi, edx, 0x10245EB3);
			guard_voice_call(0x10245EE7, 0x47, 0x08, edi, ecx, 0x10245EB3);
			guard_voice_call(0x10245F3C, 0x47, 0x08, edi, ecx, 0x10245F28);
			guard_voice_call(0x10246100, 0x43, 0x10, ebx, edx, 0x10246139);
			// Inlined IsHeadsetPresent/IsLocalTalking calls bypass the shared helpers.
			// Resume at their result checks with EAX == 0, retaining party state and
			// native headset-change notifications instead of skipping party setup.
			guard_voice_call(0x103072A9, 0x40, 0x08, eax, ecx, 0x103072B5);
			guard_voice_call(0x10309C62, 0x40, 0x08, eax, ecx, 0x10309C6E);
			guard_voice_call(0x10309C7D, 0x40, 0x08, eax, ecx, 0x10309C89);
			guard_voice_call(0x10324F37, 0x40, 0x10, eax, edx, 0x10324F43);
			guard_voice_call(0x10324F62, 0x40, 0x10, eax, edx, 0x10324F6E);
			guard_voice_call(0x102DBE40, 0x40, 0x10, eax, edx, 0x102DBE4C);
			guard_voice_call(0x103009FD, 0x47, 0x10, edi, edx, 0x10300A09);
				console::info("[patches - voice] PATCHED: unavailable-engine lifecycle and party/UI guards\n");
			}
			catch (const std::exception& error)
			{
				console::error("[patches - voice] skipped: %s\n", error.what());
			}
		}

		void private_match_set_unpaused()
		{
			game::Dvar_SetString("cl_paused", "0");
		}

		void apply_private_match_unpause()
		{
			// COD4 clears cl_paused during SV_SpawnServer. QoS 1.1 reaches the
			// map and Game Initialization without that reset, leaving local private
			// matches on a black paused screen after ui_mp is unloaded.
			console::info("[patches - private-match] applying server startup guard\n");
			const auto site = game::game_offset(0x102F7281);
			const unsigned char expected[] = {0xFF, 0x15, 0x54, 0x60, 0x47, 0x10};
			if (memcmp(reinterpret_cast<const void*>(site), expected, sizeof(expected)) != 0)
			{
				const auto* const actual = reinterpret_cast<const unsigned char*>(site);
				console::error("[patches - private-match] skipped: unexpected bytes at 0x102F7281 "
					"(%02X %02X %02X %02X %02X %02X)\n",
					actual[0], actual[1], actual[2], actual[3], actual[4], actual[5]);
				return;
			}

			try
			{
				auto* stub = utils::hook::assemble([site](utils::hook::assembler& a)
				{
					// Preserve the original six-byte indirect import call and its
					// existing stdcall stack argument exactly.
					a.call(dword_ptr(site + 2));
					a.call(private_match_set_unpaused);
					a.jmp(reinterpret_cast<void*>(site + 6));
				});
				utils::hook::nop(site, sizeof(expected));
				utils::hook::jump(site, stub);
				console::info("[patches - private-match] PATCHED: clear cl_paused after server startup\n");
			}
			catch (const std::exception& error)
			{
				console::error("[patches - private-match] skipped: %s\n", error.what());
			}
		}

		bool local_offline_mode_requested()
		{
			return utils::flags::has_flag("offline")
				|| utils::flags::has_flag("local_offline")
				|| utils::flags::has_flag("local-offline");
		}

		void apply_local_offline_mode_patches()
		{
			// Skip XLive-backed playlist/stat downloads. This lets local map bring-up
			// proceed without waiting on online storage checks.
			utils::hook::jump(game::game_offset(0x10240B30), ret_one);
			utils::hook::jump(game::game_offset(0x10240A30), ret_one);

			// Skip the XSessionCreate zero-session-id failure branch that raises
			// XBOXLIVE_NETCONNECTION during local session startup.
			utils::hook::nop(game::game_offset(0x102489A1), 5);

			console::info("[patches - offline] online storage/session checks bypassed\n");
		}

		using cl_parse_server_message_huffman_t = unsigned int(__cdecl*)(int, std::uint32_t*);
		utils::hook::detour cl_parse_server_message_huffman_hook;
		unsigned int __cdecl CL_ParseServerMessage_huffman_guard(int a1, std::uint32_t* a2)
		{
			const cl_parse_server_message_huffman_t original = reinterpret_cast<cl_parse_server_message_huffman_t>(cl_parse_server_message_huffman_hook.get_original());

			if (!a2)
			{
				return original(a1, a2);
			}

			if (a2[5] < a2[7])
			{
				game::Com_Error(
					(int)".\\cl_parse_mp.cpp",
					1243,
					1,
					(char*)"Huffman compressed msg cursor underflow detected\n");
				return 0;
			}

			const auto compressed_bytes = static_cast<std::size_t>(a2[5] - a2[7]);
			if (compressed_bytes > k_huffman_max_compressed_bytes)
			{
				game::Com_Error(
					(int)".\\cl_parse_mp.cpp",
					1243,
					1,
					(char*)"Huffman compressed msg exceeded safe decode limit (%u > %u)\n",
					static_cast<unsigned int>(compressed_bytes),
					static_cast<unsigned int>(k_huffman_max_compressed_bytes));
				return 0;
			}

			return original(a1, a2);
		}

		using ui_replace_directive_t = char*(__fastcall*)(int, char*, int, unsigned __int8);
		utils::hook::detour ui_replace_directive_hook;
		char* __fastcall UI_ReplaceDirective_guard(int ArgList, char* a2, int a3, unsigned __int8 a4)
		{
			const ui_replace_directive_t original = reinterpret_cast<ui_replace_directive_t>(ui_replace_directive_hook.get_original());
			const auto* const arg_list = reinterpret_cast<const char*>(ArgList);
			if (bounded_length(arg_list, k_ui_replace_directive_max_len + 1) > k_ui_replace_directive_max_len
				|| bounded_length(a2, k_ui_replace_directive_max_len + 1) > k_ui_replace_directive_max_len)
			{
				game::Com_Printf(0, "UI_ReplaceDirective: rejected oversized directive input\n");
				return a2;
			}

			return original(ArgList, a2, a3, a4);
		}

		using party_atomic_host_handle_member_join_t = int(__cdecl*)(char, std::uint32_t*, int, __int64, int, std::uint32_t*);
		utils::hook::detour party_atomic_host_handle_member_join_hook;
		int __cdecl PartyAtomicHost_HandleMemberJoin_guard(char a1, std::uint32_t* a2, int a3, __int64 a4, int a5, std::uint32_t* a6)
		{
			const party_atomic_host_handle_member_join_t original = reinterpret_cast<party_atomic_host_handle_member_join_t>(party_atomic_host_handle_member_join_hook.get_original());
			if (!a2 || !a6)
			{
				return original(a1, a2, a3, a4, a5, a6);
			}

			if (a6[5] < a6[7])
			{
				game::Com_Printf(0, "PartyAtomicHost_HandleMemberJoin: rejected malformed message cursor\n");
				return 0;
			}

			const auto unread_bytes = static_cast<std::size_t>(a6[5] - a6[7]);
			if (unread_bytes > k_party_member_join_max_message_bytes)
			{
				game::Com_Printf(0, "PartyAtomicHost_HandleMemberJoin: rejected oversized message (%zu bytes)\n", unread_bytes);
				return 0;
			}

			return original(a1, a2, a3, a4, a5, a6);
		}

		bool PartyAtomicHost_HandleMemberJoin_self_test()
		{
			std::uint32_t packet_cursor[8]{};
			std::uint32_t join_state[8]{};

			packet_cursor[5] = 0;
			packet_cursor[7] = 1;

			const auto result = PartyAtomicHost_HandleMemberJoin_guard(0, &join_state[0], 0, 0, 0, &packet_cursor[0]);
			if (result != 0)
			{
				game::Com_Printf(0, "PartyAtomicHost_HandleMemberJoin self-test failed\n");
				return false;
			}

			game::Com_Printf(0, "PartyAtomicHost_HandleMemberJoin self-test passed\n");
			return true;
		}

		bool UI_ReplaceDirective_self_test()
		{
			char oversized[0x110]{};
			std::memset(oversized, 'A', sizeof(oversized) - 1);

			volatile std::uint32_t canary_before = 0xDEADBEEF;
			char output[0x110]{};
			volatile std::uint32_t canary_after = 0xCAFEBABE;

			const auto result = UI_ReplaceDirective_guard(reinterpret_cast<std::uintptr_t>(oversized), output, 0, 0);
			if (result != output)
			{
				game::Com_Printf(0, "UI_ReplaceDirective self-test failed: Wrong return pointer\n");
				return false;
			}

			if (canary_before != 0xDEADBEEF || canary_after != 0xCAFEBABE)
			{
				game::Com_Printf(0, "UI_ReplaceDirective self-test failed: Stack overflow detected!\n");
				return false;
			}

			if (output[sizeof(output) - 1] != '\0')
			{
				game::Com_Printf(0, "UI_ReplaceDirective self-test failed: Output not null-terminated\n");
				return false;
			}

			game::Com_Printf(0, "UI_ReplaceDirective self-test passed\n");
			return true;
		}

		void register_security_guard_self_test()
		{
			command::add("securityGuardSelfTest", [](const command::params&)
			{
				const auto party_ok = PartyAtomicHost_HandleMemberJoin_self_test();
				const auto ui_ok = UI_ReplaceDirective_self_test();
				game::Com_Printf(0, "securityGuardSelfTest: party=%s ui=%s\n",
					party_ok ? "pass" : "fail",
					ui_ok ? "pass" : "fail");
			});
		}

		bool dvar_enabled(const char* name)
		{
			const auto* const dvar = game::Dvar_FindVar(name);
			if (!dvar)
			{
				return false;
			}

			switch (dvar->type)
			{
			case game::dvar_type::boolean:
				return dvar->current.enabled;
			case game::dvar_type::integer:
				return dvar->current.integer != 0;
			case game::dvar_type::value:
				return dvar->current.value != 0.0f;
			case game::dvar_type::string:
			case game::dvar_type::enumeration:
				if (!dvar->current.string)
				{
					return false;
				}

				return dvar->current.string[0] != '\0'
					&& strcmp(dvar->current.string, "0") != 0
					&& _stricmp(dvar->current.string, "false") != 0
					&& _stricmp(dvar->current.string, "off") != 0;
			default:
				return dvar->current.integer != 0;
			}
		}

		int dvar_int_value(const char* name, const int fallback = 0)
		{
			const auto* const dvar = game::Dvar_FindVar(name);
			if (!dvar)
			{
				return fallback;
			}

			switch (dvar->type)
			{
			case game::dvar_type::boolean:
				return dvar->current.enabled ? 1 : 0;
			case game::dvar_type::integer:
				return dvar->current.integer;
			case game::dvar_type::value:
				return static_cast<int>(dvar->current.value);
			case game::dvar_type::string:
			case game::dvar_type::enumeration:
				return dvar->current.string ? std::atoi(dvar->current.string) : fallback;
			default:
				return dvar->current.integer;
			}
		}

		void make_dvar_saved_and_writable(const char* name)
		{
			auto* const dvar = game::Dvar_FindVar(name);
			if (!dvar)
			{
				return;
			}

			const auto writable_flags = static_cast<std::uint16_t>(dvar->flags)
				& ~static_cast<std::uint16_t>(game::dvar_flags::read_only | game::dvar_flags::write_protected | game::dvar_flags::latched);

			dvar->flags = static_cast<game::dvar_flags>(writable_flags | static_cast<std::uint16_t>(game::dvar_flags::saved));
		}

		void make_dvar_debug_writable(const char* name)
		{
			auto* const dvar = game::Dvar_FindVar(name);
			if (!dvar)
			{
				return;
			}

			const auto writable_flags = static_cast<std::uint16_t>(dvar->flags)
				& ~static_cast<std::uint16_t>(game::dvar_flags::read_only
					| game::dvar_flags::write_protected
					| game::dvar_flags::latched
					| game::dvar_flags::cheat_protected);

			dvar->flags = static_cast<game::dvar_flags>(writable_flags | static_cast<std::uint16_t>(game::dvar_flags::saved));
		}

		game::dvar_s* __cdecl register_fullscreen_for_window_parms(const char* name)
		{
			// QoS 1.1, 0x103BE15B: register before window parms are read, including
			// the first launch. Use native registration to convert a config-created
			// string and apply latched values without allocating a duplicate dvar.
			const auto target = game::game_offset(0x10278E60);
			const auto* description = "Display game full screen";
			const int flags = game::dvar_flags::saved | game::dvar_flags::latched;
			game::dvar_s* result;
			__asm
			{
				push flags
				push 1
				push name
				xor ecx, ecx
				mov edx, description
				call target
				add esp, 0Ch
				mov result, eax
			}
			*reinterpret_cast<game::dvar_s**>(game::game_offset(0x113EFA78)) = result;
			return result;
		}

		void apply_video_dvar_patches()
		{
			// Verified against QoS 1.1 and COD4 Mac Com_Frame_Try_Block_Function,
			// R_BeginRegistration and R_SetD3DPresentParameters (2026-09-10).
			// Keep native registration calls AND their EAX consumers intact.
			// In particular, WM_CREATE stores EAX into the fullscreen pointer.
			// Runtime regression: r_fullscreen 0/1 + vid_restart, repeated restarts,
			// relaunch with saved windowed config, and Alt-Tab/device recovery.
			// Check videoInfo after each; test com_maxfps 30/60/125/250/0 with
			// r_vsync 0 + vid_restart. These patches can go when these native
			// registration/window-parms routines are replaced in source.
			const auto check = [](const std::uintptr_t address, const char* bytes, const std::size_t size)
			{
				if (std::memcmp(reinterpret_cast<const void*>(game::game_offset(address)), bytes, size) != 0)
				{
					throw std::runtime_error("Unsupported engine instructions for video dvar patches");
				}
			};
			check(0x103BE15B, "\xE8\xE0\x7E\xEB\xFF", 5);
			check(0x103BE16D, "\xE8\x2E\x6A\xEB\xFF", 5);
			check(0x102C448F, "\x6A\x40", 2);
			check(0x102C44A8, "\xE8\xB3\x49\xFB\xFF", 5);
			check(0x103F6960, "\x6A\x40", 2);
			check(0x103F6969, "\x6A\x1E", 2);

			utils::hook::call(game::game_offset(0x103BE15B), register_fullscreen_for_window_parms);
			utils::hook::nop(game::game_offset(0x103BE16D), 5);
			utils::hook::set<std::uint8_t>(game::game_offset(0x102C4490),
				game::dvar_flags::saved | game::dvar_flags::latched);
			utils::hook::set<std::uint8_t>(game::game_offset(0x103F6961), game::dvar_flags::saved);
			utils::hook::set<std::uint8_t>(game::game_offset(0x103F696A), 85);
		}

		HWND __stdcall create_window_ex_stub(DWORD ex_style, LPCSTR class_name, LPCSTR window_name, DWORD style, int x, int y, int width, int height, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
		{
			if (!strcmp(class_name, "JB_MP"))
			{
				window_name = "Project: Consolation - Multiplayer";

				const auto fullscreen = dvar_enabled("r_fullscreen");
				const bool borderless = dvar_enabled("r_borderless");

				if (!fullscreen)
				{
					x = dvar_int_value("vid_xpos", x);
					y = dvar_int_value("vid_ypos", y);
				}

				if (!fullscreen && borderless)
				{
					// The engine already enlarged width/height for the original frame.
					// Remove that padding before changing to a popup window.
					RECT frame{ 0, 0, 0, 0 };
					if (AdjustWindowRectEx(&frame, style, menu != nullptr, ex_style))
					{
						width -= frame.right - frame.left;
						height -= frame.bottom - frame.top;
					}
					style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
					style |= WS_POPUP;
					ex_style &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
					ex_style |= WS_EX_APPWINDOW;
				}
			}
			return CreateWindowExA(ex_style, class_name, window_name, style, x, y, width, height, parent, menu, inst, param);
		}

		template <typename T>
		T* find_dvar(std::unordered_map<std::string, T>& map, const std::string& name)
		{
			auto i = map.find(name);
			if (i != map.end())
			{
				return &i->second;
			}

			return nullptr;
		}

		bool find_dvar(std::unordered_set<std::string>& set, const std::string& name)
		{
			return set.find(name) != set.end();
		}

		utils::hook::detour dvar_registernew_hook;
		game::dvar_s* Dvar_RegisterNew_Stub(const char* dvarName, game::DvarType type, unsigned short flags, char* desc, int unk, game::DvarValue value, game::DvarLimits domain)
		{
			if (type == game::DVAR_TYPE_FLOAT_2 && !_stricmp(dvarName, "cg_debugInfoCornerOffset"))
			{
				value.vector[0] = 0.0f;
				value.vector[1] = 0.0f;
			}

			if (type == game::DVAR_TYPE_BOOL)
			{
				auto* var = find_dvar(dvars::overrides::register_bool_overrides, dvarName);
				if (var)
				{

					value.enabled = var->value;
					flags = var->flags;
				}

			}

			if (type == game::DVAR_TYPE_INT)
			{
				auto* var = find_dvar(dvars::overrides::register_int_overrides, dvarName);
				if (var)
				{
					value.integer = var->value;
					domain.integer.max = var->max;
					domain.integer.min = var->min;
					flags = var->flags;
				}
			}

			if (type == game::DVAR_TYPE_ENUM)
			{
				if (!_stricmp(dvarName, "cg_drawFPS"))
				{
					const auto writable_flags = flags
						& ~static_cast<unsigned short>(game::dvar_flags::read_only | game::dvar_flags::write_protected | game::dvar_flags::latched);
					flags = static_cast<unsigned short>(writable_flags | static_cast<unsigned short>(game::dvar_flags::saved));
				}
			}

			if (type == game::DVAR_TYPE_FLOAT)
			{
				auto* var = find_dvar(dvars::overrides::register_float_overrides, dvarName);
				if (var)
				{
					value.value = var->value;
					domain.value.max = var->max;
					domain.value.min = var->min;
					flags = var->flags;
				}
			}

			if (type == game::DVAR_TYPE_STRING)
			{
				auto* var = find_dvar(dvars::overrides::register_string_overrides, dvarName);
				if (var)
				{
					value.string = var->value.c_str();
					flags = static_cast<unsigned short>(var->flags);
				}
			}

			return dvar_registernew_hook.invoke<game::dvar_s*>(dvarName, type, flags, desc, unk, value, domain);
		}

		utils::hook::detour BG_GetPlayerJumpHeight_hook;
		float BG_GetPlayerJumpHeight_stub(int a1)
		{
			auto jump_height = game::Dvar_FindVar("jump_height");

			if (!jump_height)
				return BG_GetPlayerJumpHeight_hook.invoke<float>(a1);

			return jump_height->current.value;
		}

		utils::hook::detour BG_GetPlayerSpeed_hook;
		int BG_GetPlayerSpeed_stub(int a1)
		{
			auto g_speed = game::Dvar_FindVar("g_speed");

			if (!g_speed)
				return BG_GetPlayerSpeed_hook.invoke<int>(a1);

			return g_speed->current.integer;
		}

		utils::hook::detour r_lodScale_hook;
		void r_lodScale_stub(const float value)
		{
			const auto* const dvar = game::Dvar_FindVar("r_lodScale");
			// The renderer re-applies this from r_lodScaleParam during init and
			// asset load. If the user has already modified the dvar, keep their
			// value instead of snapping back to the param-derived default.
			if (dvar && dvar->modified)
			{
				return;
			}

			r_lodScale_hook.invoke<void>(value);
		}


		float __cdecl Jump_GetLandFactor(DWORD* ps)
		{
			__int64 v1; // r10
			double v2; // fp1

			auto jump_slowdownEnable = game::Dvar_FindVar("jump_slowdownEnable");
			if (jump_slowdownEnable->current.enabled)
			{
				if (*(DWORD*)(ps + 24) < 1700)
				{
					v1 = *(DWORD*)(ps + 24);
					v2 = (float)((float)((float)v1 * (float)0.00088235294) + (float)1.0);
				}
				else
				{
					v2 = 2.5;
				}
			}
			else
			{
				v2 = 1.0;
			}
			return *((float*)&v2 + 1);
		}

		utils::hook::detour Jump_Start_hook;
		int Jump_Start_stub(int unused, int unused2, DWORD* pml_t)
		{
			DWORD* pmove_t{};
			float jump_height = game::Dvar_FindVar("jump_height")->current.value;

			_asm
			{
				mov  edi, DWORD PTR[edi]; edi = *edi
				mov  DWORD PTR[pmove_t], edi
			}

			auto v3 = *pmove_t;
			auto gravity = *(int*)(*pmove_t + 0x68);
			auto calculatedGravity = (double)gravity * (jump_height + jump_height);

			if ((*(DWORD*)(*pmove_t + 12) & 0x4000) != 0 && *(DWORD*)(v3 + 24) <= 1800)
			{
				auto landFactor = Jump_GetLandFactor(pmove_t);
				calculatedGravity = (float)((float)calculatedGravity / (float)landFactor);
			}

			pml_t[12] = 0;
			pml_t[13] = 0;
			pml_t[11] = 0;

			auto zOrigin = *(float*)(v3 + 40);
			*(DWORD*)(v3 + 128) = 1023; // groundEntityNum
			auto serverTime = pmove_t[1];
			*(float*)(v3 + 140) = zOrigin;

			*(DWORD*)(v3 + 136) = serverTime;
			auto v9 = sqrt(calculatedGravity);
			auto v11 = *(DWORD*)(v3 + 12) & 0xFFFFFE7F | 0x4000;
			*(float*)(v3 + 52) = v9;
			*(DWORD*)(v3 + 12) = v11;
			*(DWORD*)(v3 + 24) = 0;
			*(DWORD*)(v3 + 3900) = 0;

			auto v13 = game::Dvar_FindVar("jump_spreadAdd")->current.value;
			auto v14 = *(float*)(v3 + 4340) + v13;
			*(float*)(v3 + 4340) = v14;
			if (v14 > 255.0)
				*(DWORD*)(v3 + 4340) = 255.0;

			return v13;
		}
	}

	class component final : public component_interface
	{
		bool timer_period_active_ = false;

	public:
		void post_load() override
		{
			apply_video_dvar_patches();
			apply_cinematic_stats_guard();
			apply_missing_voice_engine_guard();
			apply_private_match_unpause();
			// branding - intercept import for CreateWindowExA to change window title
			utils::hook::set(game::game_offset(0x1047627C), create_window_ex_stub);

			// nop call to Com_Printf for "SCALEFORM: %s" messages
			utils::hook::nop(game::game_offset(0x1000230F), 0x05); // TODO: Dvar toggle? Could be useful info
			utils::hook::nop(game::game_offset(0x102E1284), 0x05);
			// nop above call to Com_Printf for "unknown UI script %s in block:\n%s\n"

			// keep the registered version dvar value instead of letting stock init overwrite it
			utils::hook::nop(game::game_offset(0x103F9E53), 0x05);

			// stop an engine UI path from intentionally breaking into the debugger
			utils::hook::nop(game::game_offset(0x1027D3C4), 0x05);

			// various hooks to return dvar functionality, thanks to Liam
			BG_GetPlayerJumpHeight_hook.create(game::game_offset(0x101E6900), BG_GetPlayerJumpHeight_stub);
			BG_GetPlayerSpeed_hook.create(game::game_offset(0x101E6930), BG_GetPlayerSpeed_stub);
			r_lodScale_hook.create(game::game_offset(0x10279010), r_lodScale_stub);

			Jump_Start_hook.create(game::game_offset(0x101DB390), Jump_Start_stub);

			const auto offline_mode = local_offline_mode_requested();
			dvars::overrides::register_bool("cl_offlineMode", offline_mode, game::dvar_flags::read_only);

			// support local/xliveless map bring-up without online storage/session checks
			if (offline_mode)
			{
				apply_local_offline_mode_patches();
			}

#ifdef XLIVELESS
			if (!offline_mode)
			{
				apply_local_offline_mode_patches();
			}
#endif

			dvars::overrides::register_bool("sv_cheats", 1, game::dvar_flags::none);
			dvars::overrides::register_string("version", build_version_string(),
				static_cast<unsigned int>(game::dvar_flags::server_info | game::dvar_flags::read_only));
			dvars::overrides::register_string("shortversion", build_shortversion_string(),
				static_cast<unsigned int>(game::dvar_flags::server_info | game::dvar_flags::read_only));
			dvars::overrides::register_string("gamename", "Project: Consolation",
				static_cast<unsigned int>(game::dvar_flags::read_only));
			dvars::overrides::register_string("gamedate", build_game_date_string(),
				static_cast<unsigned int>(game::dvar_flags::read_only));
			//dvars::overrides::register_float("r_lodScale", 0, 0, 3, game::dvar_flags::saved); //doesn't save
			//dvars::overrides::register_float("jump_height", 39.0, 0, 1000, game::dvar_flags::saved); //adjusted to 39 to allow cod4-like jump onto ledges

			
			dvar_registernew_hook.create(game::Dvar_RegisterNew, Dvar_RegisterNew_Stub);

			scheduler::once([this]
			{
				// Run outside DLL initialization. The native frame loop uses
				// timeGetTime and Sleep(1), but this image imports no timeBeginPeriod.
				// Request precision in this process to avoid coarse timer pacing.
				timer_period_active_ = timeBeginPeriod(1) == TIMERR_NOERROR;

				dvars::replace_dvar_at(game::game_offset(0x103AF41F), 5, reinterpret_cast<game::dvar_s**>(game::game_offset(0x11054688)),
					dvars::make_float("r_lodScale", "Scale the level of detail distance (larger reduces detail)", 0.0f, 0.0f, 3.0f, game::dvar_flags::saved));

				dvars::replace_dvar_at(game::game_offset(0x102BE942), 5, reinterpret_cast<game::dvar_s**>(game::game_offset(0x1148BECC)),
					dvars::make_float("cg_fovScale", "Scale applied to the field of view", 1.0f, 0.0f, 2.0f, game::dvar_flags::saved));

				dvars::replace_dvar_at(game::game_offset(0x102BE908), 5, reinterpret_cast<game::dvar_s**>(game::game_offset(0x1148F6A4)),
					dvars::make_float("cg_fov", "The field of view angle in degrees", 65.0f, 0.0f, 160.0f, game::dvar_flags::saved));

				dvars::replace_dvar_at(game::game_offset(0x10321250), 5, reinterpret_cast<game::dvar_s**>(game::game_offset(0x11260BD0)),
					dvars::make_float("input_viewSensitivity", "Mouse sensitivity", 1.0f, 0.01f, 30.0f, game::dvar_flags::saved));

				dvars::replace_dvar_at(game::game_offset(0x101DB65A), 5, reinterpret_cast<game::dvar_s**>(game::game_offset(0x118EE1C0)),
					dvars::make_float("jump_height", "The maximum height of a player's jump", 41.0f, 0.0f, 1000.0f, game::dvar_flags::saved));

				dvars::replace_dvar_at(game::game_offset(0x103B2260), 5, reinterpret_cast<game::dvar_s**>(game::game_offset(0x11054944)),
					dvars::make_int("developer", "Enable development environment", 0, 0, 2, game::dvar_flags::none));

				make_dvar_saved_and_writable("sv_cheats");
				make_dvar_saved_and_writable("vid_xpos");
				make_dvar_saved_and_writable("vid_ypos");

				//debug block sv_cheats
#ifdef DEBUG
				utils::hook::nop(game::game_offset(0x101AB211), 5);
				utils::hook::nop(game::game_offset(0x10245A2A), 5);

				auto* const sv_cheats = dvars::Dvar_RegisterBool("sv_cheats", 1, "Enable Cheats", game::dvar_flags::none);
				*reinterpret_cast<game::dvar_s**>(game::game_offset(0x11A343C0)) = sv_cheats;
				*reinterpret_cast<game::dvar_s**>(game::game_offset(0x1149FCD8)) = sv_cheats;
				make_dvar_debug_writable("r_fullbright");
#endif
			}, scheduler::main);

			cl_parse_server_message_huffman_hook.create(game::game_offset(0x1030D960), CL_ParseServerMessage_huffman_guard);
			ui_replace_directive_hook.create(game::game_offset(0x102BB870), UI_ReplaceDirective_guard);
			party_atomic_host_handle_member_join_hook.create(game::game_offset(0x103087B0), PartyAtomicHost_HandleMemberJoin_guard);
			register_security_guard_self_test();
			command::add("videoInfo", [this](const command::params&)
			{
				// Compare requested/current dvars with the renderer's cached
				// presentation parameters, which are updated on creation AND reset.
				for (const auto* name : { "r_fullscreen", "r_vsync", "com_maxfps" })
				{
					const auto* dvar = game::Dvar_FindVar(name);
					if (dvar && (dvar->type == game::dvar_type::boolean || dvar->type == game::dvar_type::integer))
					{
						const bool boolean = dvar->type == game::dvar_type::boolean;
						game::Com_Printf(0, "%s: current=%i latched=%i flags=0x%04X\n", name,
							boolean ? dvar->current.enabled : dvar->current.integer,
							boolean ? dvar->latched.enabled : dvar->latched.integer,
							static_cast<unsigned int>(dvar->flags));
					}
				}
				const auto* present = reinterpret_cast<const D3DPRESENT_PARAMETERS*>(game::game_offset(0x10E271F4));
				game::Com_Printf(0, "Renderer cache: %ux%u windowed=%i refresh=%u interval=0x%08X timer1ms=%i\n",
					present->BackBufferWidth, present->BackBufferHeight, present->Windowed,
					present->FullScreen_RefreshRateInHz, present->PresentationInterval, timer_period_active_);
				if (present->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE)
				{
					game::Com_Printf(0, "VSync can limit FPS to the display refresh rate. Use r_vsync 0; vid_restart for higher FPS.\n");
				}
			});

			scheduler::loop([]
			{
				make_dvar_saved_and_writable("sv_cheats");
				make_dvar_saved_and_writable("vid_xpos");
				make_dvar_saved_and_writable("vid_ypos");
#ifdef DEBUG
				make_dvar_debug_writable("r_fullbright");
#endif
			}, scheduler::main, 250ms);
		}

		void pre_destroy() override
		{
			if (timer_period_active_)
			{
				timeEndPeriod(1);
				timer_period_active_ = false;
			}
		}
	};
}

REGISTER_COMPONENT(patches::component)
