#include <std_include.hpp>

#include <component/console.hpp>
#include <game/game.hpp>
#include <game/structs.hpp>
#include <utils/string.hpp>
#include "entities.hpp"

std::string Entities::Build()
{
	std::string entityString;

	for (auto& entity : this->entities)
	{
		entityString.append("{\n");

		for (auto& property : entity)
		{
			entityString.push_back('"');
			entityString.append(property.first);
			entityString.append("\" \"");
			entityString.append(property.second);
			entityString.append("\"\n");
		}

		entityString.append("}\n");
	}

	return entityString;
}

std::vector<std::string> Entities::GetModels()
{
	std::vector<std::string> models = std::vector<std::string>();
	std::ofstream destructiblesModelList;

	for (auto& entity : this->entities)
	{
		if (entity.find("model") != entity.end())
		{
			std::string model = entity["model"];

			if (!model.empty()
				&& model[0] != '*' && model[0] != '?'  // Skip brushmodels
				&& model != "com_plasticcase_green_big_us_dirt"s // Skip care package (part of team zones)
				)
			{
				if (std::find(models.begin(), models.end(), model) == models.end())
				{
					models.push_back(model);
				}
			}
		}
	}

	return models;
}

bool Entities::ConvertVehicles() {
	bool hasVehicles = false;

	for (auto& entity : this->entities)
	{
		if (entity.find("classname") != entity.end())
		{
			if (entity.find("targetname") != entity.end() &&
				entity["targetname"] == "destructible"s &&
				utils::string::starts_with(entity["destructible_type"], "vehicle"s))
			{
				entity["targetname"] = "destructible_vehicle";
				entity["sound_csv_include"] = "vehicle_car_exp";

				hasVehicles = true;
			}
		}
	}

	return hasVehicles;
}

bool Entities::ConvertTurrets()
{
	bool hasTurrets = false;

	for (auto& entity : this->entities)
	{
		if (entity.find("classname") != entity.end())
		{
			if (entity["classname"] == "misc_turret"s)
			{
				entity["weaponinfo"] = "turret_minigun_mp";
				entity["model"] = "weapon_minigun";
				hasTurrets = true;
			}
		}
	}

	return hasTurrets;
}

/// Yes - QOS still has OldSchoolPickups
void Entities::DeleteOldSchoolPickups()
{
	for (auto i = this->entities.begin(); i != this->entities.end();)
	{
		if (i->find("weaponinfo") != i->end() || (i->find("targetname") != i->end() && (*i)["targetname"] == "oldschool_pickup"s))
		{
			if (i->find("classname") == i->end() || (*i)["classname"] != "misc_turret"s)
			{
				console::info("Erased weapon %s from map ents\n", (*i)["model"].c_str());
				i = this->entities.erase(i);
				continue;
			}
		}

		++i;
	}
}

void Entities::AddRemovedSModels()
{
	///todo
	/*game::GfxWorld* iw3World{};

	game::DB_EnumXAssetEntries(game::ASSET_TYPE_GFXWORLD, [&iw3World](game::XAssetEntryPoolEntry* entry)
	{
		iw3World = entry->entry.asset.header.gfxWorld;
	}, false);


	if (iw3World) {
		for (auto index : IGfxWorld::removedStaticModelIndices)
		{
			auto drawInst = &iw3World->smodelDrawInsts[index];

			game::vec3_t angles{};
			game::AxisToAngles(&angles, drawInst->placement.axis);
			const std::string origin = utils::string::va("%f %f %f", drawInst->placement.origin[0], drawInst->placement.origin[1], drawInst->placement.origin[2]);
			const std::string anglesStr = utils::string::va("%f %f %f", angles[0], angles[1], angles[2]);

			std::unordered_map<std::string, std::string> scriptModelEntity =
			{
				{    "_comment",                 "added by qos-exp"},
				{    "classname",                "script_model"},
				{    "ltOrigin",                 origin},
				{    "origin",                   origin},
				{    "angles",                   anglesStr},
				{    "model",                    drawInst->model->name }
			};

			entities.push_back(scriptModelEntity);
		}
	}*/
}

void Entities::parse(std::string buffer)
{
	int parseState = 0;
	std::string key;
	std::string value;
	std::unordered_map<std::string, std::string> entity;

	for (unsigned int i = 0; i < buffer.size(); ++i)
	{
		char character = buffer[i];
		if (character == '{')
		{
			entity.clear();
		}

		switch (character)
		{
		case '{':
		{
			entity.clear();
			break;
		}

		case '}':
		{
			this->entities.push_back(entity);
			entity.clear();
			break;
		}

		case '"':
		{
			if (parseState == PARSE_AWAIT_KEY)
			{
				key.clear();
				parseState = PARSE_READ_KEY;
			}
			else if (parseState == PARSE_READ_KEY)
			{
				parseState = PARSE_AWAIT_VALUE;
			}
			else if (parseState == PARSE_AWAIT_VALUE)
			{
				value.clear();
				parseState = PARSE_READ_VALUE;
			}
			else if (parseState == PARSE_READ_VALUE)
			{
				entity[utils::string::to_lower(key)] = value;
				parseState = PARSE_AWAIT_KEY;
			}
			else
			{
				throw std::runtime_error("Parsing error!");
			}
			break;
		}

		default:
		{
			if (parseState == PARSE_READ_KEY) key.push_back(character);
			else if (parseState == PARSE_READ_VALUE) value.push_back(character);

			break;
		}
		}
	}
}