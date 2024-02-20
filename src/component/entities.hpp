#pragma once

class Entities
{
public:
	Entities() {};
	Entities(const char* string, size_t lenPlusOne) : Entities(std::string(string, lenPlusOne - 1)) {}
	Entities(std::string buffer) : Entities() { this->parse(buffer); };
	Entities(const Entities& obj) : entities(obj.entities) {};

	std::string Build();

	std::vector<std::string> GetModels();
	bool ConvertTurrets();
	bool ConvertVehicles();
	void AddCarePackages();
	void AddRemovedSModels();
	void DeleteOldSchoolPickups();

private:
	enum
	{
		PARSE_AWAIT_KEY,
		PARSE_READ_KEY,
		PARSE_AWAIT_VALUE,
		PARSE_READ_VALUE,
	};

	std::vector<std::unordered_map<std::string, std::string>> entities;
	void parse(std::string buffer);
};