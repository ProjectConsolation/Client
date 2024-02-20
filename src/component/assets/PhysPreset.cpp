#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"
#include "game/structs.IW4.hpp"

#include <utils/memory.hpp>

namespace physpreset
{
	namespace
	{
		game::iw4::PhysPreset* convert(game::qos::PhysPreset* phys_preset)
		{
			if (!phys_preset) return nullptr;

			auto iw4_asset = utils::memory::allocate<game::iw4::PhysPreset>();
			//static_assert(sizeof game::qos::PhysPreset == sizeof game::iw4::PhysPreset);

			std::memcpy(iw4_asset, phys_preset, sizeof game::qos::PhysPreset);

			return iw4_asset;
		}
	}

	class component final : public component_interface
	{
	public:
		game::qos::XAssetType get_type() override
		{
			return game::qos::ASSET_TYPE_PHYSPRESET;
		}

		game::iw4::XAssetHeader convert_asset(game::qos::XAssetHeader header) override
		{
			return { convert(header.physPreset) };
		}
	};
}

REGISTER_COMPONENT(physpreset::component)
