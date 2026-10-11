#include "../src/component/engine/scripting/ambient_script.hpp"
#include <cassert>
#include <fstream>
#include <iterator>
#include <iostream>

int main(int argc, char** argv)
{
	using gametypes::ambient_script::restore_loops;
	const std::string base = "main()\n{\n/#\nif (getdvar(\"clientSideEffects\") != \"1\")\n"
		"maps\\createfx\\mp_barge_fx::main();\n#/\n}\n";
	const std::string sound = "ent = maps\\mp\\_createfx::createLoopSound();\n"
		"ent.v[\"origin\"] = (1, -2.5, 3);\n"
		"ent.v[\"angles\"] = (270, 0, 0);\n"
		"ent.v[\"soundalias\"] = \"bgt_ship\";\n";
	auto data = base;
	assert(restore_loops(data, sound, "mp_barge", [](const auto&) { return true; }));
	assert(data.find("spawn(\"script_origin\", (1, -2.5, 3))") != std::string::npos);
	assert(data.find("playLoopSound(\"bgt_ship\")") != std::string::npos);
	assert(!restore_loops(data, sound, "mp_barge", [](const auto&) { return true; }));
	data = base;
	assert(!restore_loops(data, sound, "mp_shipment", [](const auto&) { return true; }));
	assert(data == base);
	auto no_developer = std::string("main()\n{\nmaps\\createfx\\mp_barge_fx::main();\n}\n");
	const auto unchanged = no_developer;
	assert(!restore_loops(no_developer, sound, "mp_barge", [](const auto&) { return true; }));
	assert(no_developer == unchanged);
	auto no_effects = base;
	assert(!restore_loops(no_effects, "ent = maps\\mp\\_utility::createOneshotEffect(\"x\");", "mp_barge", [](const auto&) { return true; }));
	assert(no_effects == base);
	auto crlf = base;
	for (std::size_t i = 0; i < crlf.size(); ++i) if (crlf[i] == '\n') { crlf.insert(i, 1, '\r'); ++i; }
	assert(restore_loops(crlf, sound, "mp_barge", [](const auto&) { return true; }));
	assert(!restore_loops(data, sound, "mp_barge", [](const auto&) { return false; }));
	assert(data == base);
	assert(!restore_loops(data, "ent.v[\"soundalias\"] = \"fake\";", "mp_barge", [](const auto&) { return true; }));
	assert(data == base);
	assert(!restore_loops(data, sound + sound + sound, "unknown", [](const auto&) { return true; }));
	std::string too_many;
	for (int i = 0; i < 129; ++i) too_many += sound;
	assert(!restore_loops(data, too_many, "mp_barge", [](const auto&) { return true; }));
	assert(data == base);
	if (argc == 4)
	{
		auto read = [](const char* path) { std::ifstream in(path); return std::string(
			std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()); };
		data = read(argv[1]);
		assert(restore_loops(data, read(argv[2]), argv[3], [](const auto&) { return true; }));
		std::cout << argv[3] << ": authored loop fallback accepted\n";
	}
	std::cout << "ambient script tests passed\n";
}
