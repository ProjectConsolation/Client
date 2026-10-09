#include <std_include.hpp>

#include "loader/component_loader.hpp"
#include "component/utils/scheduler.hpp"
#include "game/game.hpp"

#include <utils/string.hpp>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace draw_techset
{
	namespace
	{
		constexpr auto cg_initialized_address = 0x129FE8E4;
		// QoS PC 1.1 CG_DrawMaterial (0x102B58D0) uses this trace to fill
		// its material, surface-type, and contents labels.
		constexpr auto material_trace_function_address = 0x103B9F50;
		constexpr auto material_trace_start_address = 0x12A502E8;
		constexpr auto material_trace_end_address = 0x12A502F4;
		constexpr std::size_t trace_text_capacity = 0x1000;
		constexpr std::size_t max_texture_count = 16;

		struct gfx_image_view
		{
			char fields[32];
			const char* name;
		};
		static_assert(sizeof(gfx_image_view) == 36);

		struct texture_snapshot
		{
			unsigned char semantic{};
			unsigned char sampler_state{};
			char image_name[96]{};
		};

		struct material_snapshot
		{
			char material_name[96]{};
			char technique_name[96]{};
			unsigned char texture_count{};
			texture_snapshot textures[max_texture_count]{};
		};

		struct trace_text
		{
			char material[trace_text_capacity]{};
			char surface_type[trace_text_capacity]{};
			char contents[trace_text_capacity]{};
		};

		std::uintptr_t material_trace_function{};
		std::uintptr_t material_trace_start{};
		std::uintptr_t material_trace_end{};
		float text_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		std::uintptr_t native_text_draw{};

		// QoS CG_DrawMaterial 102B58D0 uses EAX=ECX=1 for placement
		// alignment, ten caller-cleaned stack arguments, and its cached font.
		// Preserve that exact path instead of recreating viewport scaling.
		__declspec(naked) void draw_native_text(void*, const char*, int, game::Font_s*,
			float, float, float, float, const float*, int)
		{
			__asm
			{
				mov eax, 1
				mov ecx, 1
				jmp dword ptr[native_text_draw]
			}
		}

		__declspec(naked) int trace_material_native(char*, char*, char*)
		{
			__asm
			{
				mov eax, [esp + 0Ch]
				push eax
				mov eax, [esp + 0Ch]
				push eax
				mov eax, [esp + 0Ch]
				push eax
				mov eax, dword ptr [material_trace_end]
				mov ecx, dword ptr [material_trace_start]
				call dword ptr [material_trace_function]
				add esp, 0Ch
				ret
			}
		}

		bool copy_guarded_string(char* destination, const std::size_t capacity,
			const char* source)
		{
			if (!destination || !capacity)
			{
				return false;
			}

			destination[0] = '\0';
			if (!source)
			{
				return false;
			}

			__try
			{
				for (std::size_t index = 0; index + 1 < capacity; ++index)
				{
					const auto value = source[index];
					destination[index] = value;
					if (!value)
					{
						return index != 0;
					}
				}

				destination[capacity - 1] = '\0';
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				destination[0] = '\0';
				return false;
			}
		}

		bool capture_material(const game::Material* material,
			material_snapshot* snapshot)
		{
			if (!material || !snapshot)
			{
				return false;
			}

			__try
			{
				if (!material->name || !material->techniqueSet
					|| !material->techniqueSet->name || material->textureCount < 0
					|| material->textureCount > max_texture_count
					|| (material->textureCount && !material->textureTable))
				{
					return false;
				}

				if (!copy_guarded_string(snapshot->material_name,
					sizeof(snapshot->material_name), material->name)
					|| !copy_guarded_string(snapshot->technique_name,
						sizeof(snapshot->technique_name), material->techniqueSet->remappedTechniqueSet
							? material->techniqueSet->remappedTechniqueSet->name : material->techniqueSet->name))
				{
					return false;
				}

				snapshot->texture_count = static_cast<unsigned char>(material->textureCount);
				for (std::size_t index = 0; index < snapshot->texture_count; ++index)
				{
					const auto& definition = material->textureTable[index];
					auto& texture = snapshot->textures[index];
					texture.semantic = static_cast<unsigned char>(definition.semantic);
					texture.sampler_state = static_cast<unsigned char>(definition.sampleState);
					const auto* const image = static_cast<const gfx_image_view*>(definition.image);
					if (!image || !copy_guarded_string(texture.image_name,
						sizeof(texture.image_name), image->name))
					{
						strcpy_s(texture.image_name, "<invalid>");
					}
				}

				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool trace_current_material(trace_text* text)
		{
			if (!text || !material_trace_function || !material_trace_start
				|| !material_trace_end)
			{
				return false;
			}

			__try
			{
				if (!*reinterpret_cast<std::uintptr_t*>(
					game::game_offset(cg_initialized_address)))
				{
					return false;
				}

				return trace_material_native(text->material, text->surface_type,
					text->contents) != 0 && text->material[0] != '\0';
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		void draw_line(const char* text, game::Font_s* font, const float x,
			const float y, const float scale)
		{
			draw_native_text(reinterpret_cast<void*>(game::game_offset(0x1127BA50)),
				text, 0x7FFFFFFF, font, x, y, scale, scale, text_color, 0);
		}

		void configure_draw_material()
		{
			auto* const dvar = game::Dvar_FindVar("cg_drawMaterial");
			if (!dvar)
			{
				return;
			}
			// QoS PC 1.1 registers a bool at 0x102BEB07; its draw gate at
			// 0x10312151 tests the low byte, so integer modes 0..2 retain it.
			if (dvar->type == game::dvar_type::boolean)
			{
				dvar->current.integer = dvar->current.enabled ? 1 : 0;
				dvar->latched.integer = dvar->latched.enabled ? 1 : 0;
				dvar->reset.integer = dvar->reset.enabled ? 1 : 0;
				dvar->type = game::dvar_type::integer;
			}
			if (dvar->type == game::dvar_type::integer)
			{
				dvar->domain.integer.min = 0;
				dvar->domain.integer.max = 2;
				dvar->description = "Material diagnostics: 0 = off, 1 = material/surface/contents, 2 = also show resolved techset and image bindings.";
			}
		}

		bool resolve_material(const char* name, material_snapshot* snapshot)
		{
			const auto capture_named = [snapshot](const char* candidate)
			{
				const auto header = game::DB_FindXAssetHeader_Internal(
					game::ASSET_TYPE_MATERIAL, candidate, 0);
				return capture_material(header.material, snapshot)
					&& !_stricmp(snapshot->material_name, candidate);
			};
			if (capture_named(name))
			{
				return true;
			}
			// Collision names omit the world-render material's wc/ prefix.
			if (!strchr(name, '/') && capture_named(utils::string::va("wc/%s", name)))
			{
				return true;
			}
			// The IW3 converter deliberately namespaces render assets to avoid
			// overriding native QoS materials. Match its SHA-256 name contract,
			// rather than interpreting a collision label as a shader failure.
			const auto* map = game::Dvar_FindVar("mapname");
			if (!map || !map->current.string || !map->current.string[0])
			{
				return false;
			}
			BCRYPT_ALG_HANDLE algorithm{};
			if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
			{
				return false;
			}
			bool resolved = false;
			for (const auto* prefix : {"", "wc/", "mc/"})
			{
				const std::string source = std::string(prefix) + name;
				unsigned char digest[32]{};
				if (BCryptHash(algorithm, nullptr, 0,
					reinterpret_cast<PUCHAR>(const_cast<char*>(source.data())),
					static_cast<ULONG>(source.size()), digest, sizeof(digest)) < 0)
				{
					continue;
				}
				char suffix[17]{};
				constexpr char hex[] = "0123456789abcdef";
				for (std::size_t i = 0; i < 8; ++i)
				{
					suffix[2 * i] = hex[digest[i] >> 4];
					suffix[2 * i + 1] = hex[digest[i] & 15];
				}
				if (capture_named(utils::string::va("consolation/iw3/%s/%s", map->current.string, suffix)))
				{
					resolved = true;
					break;
				}
			}
			BCryptCloseAlgorithmProvider(algorithm, 0);
			return resolved;
		}

		void draw()
		{
			const auto* const draw_material = game::Dvar_FindVar("cg_drawMaterial");
			if (!draw_material || draw_material->type != game::dvar_type::integer
				|| draw_material->current.integer != 2)
			{
				return;
			}

			trace_text trace{};
			if (!trace_current_material(&trace))
			{
				return;
			}

			material_snapshot material{};
			const auto resolved = resolve_material(trace.material, &material);

			auto* const font = *reinterpret_cast<game::Font_s**>(game::game_offset(0x113FB174));
			if (!font || font->pixelHeight <= 0)
			{
				return;
			}

			constexpr auto font_scale = 1.0f;
			constexpr auto x = 8.0f;
			const auto line_height = static_cast<float>(font->pixelHeight);
			// The three native labels occupy baselines 240+h, 240+2h, 240+3h.
			auto y = 240.0f + 4.0f * line_height;
			if (!resolved)
			{
				draw_line("techset: <render material not resolved>", font, x, y, font_scale);
				return;
			}

			draw_line(utils::string::va("techset: %s", material.technique_name),
				font, x, y, font_scale);
			for (std::size_t index = 0; index < material.texture_count; ++index)
			{
				y += line_height;
				if (y > 480.0f - line_height)
				{
					break;
				}
				const auto& texture = material.textures[index];
				draw_line(utils::string::va(
					"texture %u: semantic=%u sampler=%u image=%s",
					static_cast<unsigned int>(index),
					static_cast<unsigned int>(texture.semantic),
					static_cast<unsigned int>(texture.sampler_state),
					texture.image_name), font, x, y, font_scale);
			}
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			material_trace_function = game::game_offset(material_trace_function_address);
			material_trace_start = game::game_offset(material_trace_start_address);
			material_trace_end = game::game_offset(material_trace_end_address);
			native_text_draw = game::game_offset(0x1031CF90);
			scheduler::loop(configure_draw_material, scheduler::pipeline::main, 250ms);
			scheduler::loop(draw, scheduler::pipeline::renderer);
		}

		void pre_destroy() override
		{
			material_trace_function = 0;
			material_trace_start = 0;
			material_trace_end = 0;
			native_text_draw = 0;
		}
	};
}

REGISTER_COMPONENT(draw_techset::component)
