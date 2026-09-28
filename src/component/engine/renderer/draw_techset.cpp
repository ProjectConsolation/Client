#include <std_include.hpp>

#include "loader/component_loader.hpp"

#include "component/utils/scheduler.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>

namespace draw_techset
{
	namespace
	{
		constexpr std::size_t max_texture_count = 16;
		constexpr DWORD capture_lifetime_ms = 100;

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
			bool valid{};
			DWORD captured_at{};
			game::Font_s* font{};
			float x{};
			float y{};
			float x_scale{};
			float y_scale{};
			char material_name[96]{};
			char technique_name[96]{};
			unsigned char texture_count{};
			texture_snapshot textures[max_texture_count]{};
		};

		utils::hook::detour draw_text_hook;
		material_snapshot current_material{};
		bool resolving_material_label = false;
		bool drawing_overlay = false;
		float text_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
		float shadow_color[4] = {0.0f, 0.0f, 0.0f, 0.75f};

		bool copy_guarded_string(char* destination, const std::size_t capacity, const char* source)
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

		bool capture_material(const game::Material* material, material_snapshot* snapshot)
		{
			if (!material || !snapshot)
			{
				return false;
			}

			__try
			{
				if (!material->name || !material->techniqueSet || !material->techniqueSet->name
					|| material->textureCount < 0 || material->textureCount > max_texture_count
					|| (material->textureCount && !material->textureTable))
				{
					return false;
				}

				if (!copy_guarded_string(snapshot->material_name, sizeof(snapshot->material_name), material->name)
					|| !copy_guarded_string(snapshot->technique_name, sizeof(snapshot->technique_name),
						material->techniqueSet->name))
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
					if (!image || !copy_guarded_string(texture.image_name, sizeof(texture.image_name), image->name))
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

		bool is_material_label(const char* text)
		{
			if (!text || !*text)
			{
				return false;
			}

			for (std::size_t index = 0; index < 96; ++index)
			{
				const auto value = static_cast<unsigned char>(text[index]);
				if (!value)
				{
					return index > 1;
				}
				if (value <= ' ' || value >= 0x7F)
				{
					return false;
				}
			}

			return false;
		}

		void draw_text_stub(const char* text, const int max_chars, game::Font_s* font,
			const float x, const float y, const float x_scale, const float y_scale,
			const float rotation, const float* color, const int style)
		{
			if (!resolving_material_label && !drawing_overlay
				&& dvars::cg_drawTechset && dvars::cg_drawTechset->current.enabled)
			{
				const auto* const draw_material = game::Dvar_FindVar("cg_drawMaterial");
				if (draw_material && draw_material->current.enabled && is_material_label(text))
				{
					resolving_material_label = true;
					const auto reset_guard = gsl::finally([] { resolving_material_label = false; });
					const auto header = game::DB_FindXAssetHeader_Internal(game::ASSET_TYPE_MATERIAL, text, 0);
					material_snapshot snapshot{};
					if (capture_material(header.material, &snapshot)
						&& !_stricmp(snapshot.material_name, text))
					{
						snapshot.valid = true;
						snapshot.captured_at = GetTickCount();
						snapshot.font = font;
						snapshot.x = x;
						snapshot.y = y;
						snapshot.x_scale = x_scale;
						snapshot.y_scale = y_scale;
						current_material = snapshot;
					}
				}
			}

			draw_text_hook.invoke<void>(text, max_chars, font, x, y, x_scale, y_scale,
				rotation, color, style);
		}

		void draw_line(const char* text, game::Font_s* font, const float x, const float y,
			const float x_scale, const float y_scale)
		{
			draw_text_hook.invoke<void>(text, 0x7FFFFFFF, font, x + 1.0f, y + 1.0f,
				x_scale, y_scale, 0.0f, shadow_color, 0);
			draw_text_hook.invoke<void>(text, 0x7FFFFFFF, font, x, y,
				x_scale, y_scale, 0.0f, text_color, 0);
		}

		void draw()
		{
			if (!dvars::cg_drawTechset || !dvars::cg_drawTechset->current.enabled
				|| !current_material.valid
				|| GetTickCount() - current_material.captured_at > capture_lifetime_ms
				|| !current_material.font || current_material.font->pixelHeight <= 0)
			{
				return;
			}

			drawing_overlay = true;
			const auto reset_guard = gsl::finally([] { drawing_overlay = false; });
			const auto line_height = static_cast<float>(current_material.font->pixelHeight)
				* current_material.y_scale;
			auto y = current_material.y + line_height * 3.0f;
			draw_line(utils::string::va("techset: %s", current_material.technique_name),
				current_material.font, current_material.x, y,
				current_material.x_scale, current_material.y_scale);

			for (std::size_t index = 0; index < current_material.texture_count; ++index)
			{
				y += line_height;
				const auto& texture = current_material.textures[index];
				draw_line(utils::string::va("texture %u: semantic=%u sampler=%u image=%s",
					static_cast<unsigned int>(index), static_cast<unsigned int>(texture.semantic),
					static_cast<unsigned int>(texture.sampler_state), texture.image_name),
					current_material.font, current_material.x, y,
					current_material.x_scale, current_material.y_scale);
			}
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			draw_text_hook.create(game::R_AddCmdDrawText, draw_text_stub);
			scheduler::loop(draw, scheduler::pipeline::renderer);
		}
	};
}

REGISTER_COMPONENT(draw_techset::component)
