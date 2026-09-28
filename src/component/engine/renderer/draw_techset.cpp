#include <std_include.hpp>

#include "loader/component_loader.hpp"
#include "component/utils/scheduler.hpp"
#include "game/game.hpp"
#include "game/dvars.hpp"

#include <utils/string.hpp>

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
		constexpr float virtual_width = 640.0f;
		constexpr float virtual_height = 480.0f;
		constexpr float native_anchor_x = 56.0f;
		constexpr float native_anchor_y = 350.0f;

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
		float shadow_color[4] = {0.0f, 0.0f, 0.0f, 0.75f};

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
						sizeof(snapshot->technique_name), material->techniqueSet->name))
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

		void get_client_size(float* width, float* height)
		{
			*width = virtual_width;
			*height = virtual_height;
			RECT client_rect{};
			const auto window = *game::main_window;
			if (window && GetClientRect(window, &client_rect)
				&& client_rect.right > client_rect.left
				&& client_rect.bottom > client_rect.top)
			{
				*width = static_cast<float>(client_rect.right - client_rect.left);
				*height = static_cast<float>(client_rect.bottom - client_rect.top);
			}
		}

		void draw_line(const char* text, game::Font_s* font, const float x,
			const float y, const float scale)
		{
			game::R_AddCmdDrawText(text, 0x7FFFFFFF, font, x + 1.0f, y + 1.0f,
				scale, scale, 0.0f, shadow_color, 0);
			game::R_AddCmdDrawText(text, 0x7FFFFFFF, font, x, y,
				scale, scale, 0.0f, text_color, 0);
		}

		void draw()
		{
			if (!dvars::cg_drawTechset || !dvars::cg_drawTechset->current.enabled)
			{
				return;
			}

			const auto* const draw_material = game::Dvar_FindVar("cg_drawMaterial");
			if (!draw_material || !draw_material->current.enabled)
			{
				return;
			}

			trace_text trace{};
			if (!trace_current_material(&trace))
			{
				return;
			}

			const auto header = game::DB_FindXAssetHeader_Internal(
				game::ASSET_TYPE_MATERIAL, trace.material, 0);
			material_snapshot material{};
			if (!capture_material(header.material, &material)
				|| _stricmp(material.material_name, trace.material))
			{
				return;
			}

			auto* const font = game::R_RegisterFont("fonts/consolefont");
			if (!font || font->pixelHeight <= 0)
			{
				return;
			}

			float width{};
			float height{};
			get_client_size(&width, &height);
			const auto layout_scale = (std::min)(width / virtual_width,
				height / virtual_height);
			const auto font_scale = (std::max)(0.5f, layout_scale * 0.5f);
			const auto x = native_anchor_x * width / virtual_width;
			auto y = native_anchor_y * height / virtual_height;
			const auto line_height = static_cast<float>(font->pixelHeight) * font_scale;

			draw_line(utils::string::va("techset: %s", material.technique_name),
				font, x, y, font_scale);
			for (std::size_t index = 0; index < material.texture_count; ++index)
			{
				y += line_height;
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
			scheduler::loop(draw, scheduler::pipeline::renderer);
		}

		void pre_destroy() override
		{
			material_trace_function = 0;
			material_trace_start = 0;
			material_trace_end = 0;
		}
	};
}

REGISTER_COMPONENT(draw_techset::component)
