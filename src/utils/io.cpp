#include <std_include.hpp>
#include "io.hpp"

namespace utils::io
{
	bool remove_file(const std::string& file)
	{
		return DeleteFileA(file.data()) == TRUE;
	}

	bool file_exists(const std::string& file)
	{
		return std::ifstream(file).good();
	}

	bool write_file(const std::string& file, const std::string& data, const bool append)
	{
		const auto pos = file.find_last_of("/\\");
		if (pos != std::string::npos)
		{
			create_directory(file.substr(0, pos));
		}

		std::ofstream stream(
			file, std::ios::binary | std::ofstream::out | (append ? std::ofstream::app : std::ofstream::out));

		if (stream.is_open())
		{
			stream.write(data.data(), data.size());
			stream.close();
			return true;
		}

		return false;
	}

	std::string read_file(const std::string& file)
	{
		std::string data;
		read_file(file, &data);
		return data;
	}

	std::optional<std::string> read_pe_string_rva(const std::string& file, const std::uint32_t rva, const std::size_t max_length)
	{
		std::string data;
		if (!read_file(file, &data))
		{
			return std::nullopt;
		}

		if (data.size() < sizeof(IMAGE_DOS_HEADER))
		{
			return std::nullopt;
		}

		const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(data.data());
		if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
		{
			return std::nullopt;
		}

		if (data.size() < static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS))
		{
			return std::nullopt;
		}

		const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(data.data() + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE)
		{
			return std::nullopt;
		}

		const auto* section = IMAGE_FIRST_SECTION(nt);
		for (std::uint16_t i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
		{
			const auto virtual_size = std::max(section->Misc.VirtualSize, section->SizeOfRawData);
			if (rva < section->VirtualAddress || rva >= section->VirtualAddress + virtual_size)
			{
				continue;
			}

			const auto raw_offset = section->PointerToRawData + (rva - section->VirtualAddress);
			if (raw_offset >= data.size())
			{
				return std::nullopt;
			}

			std::string result;
			result.reserve(max_length);

			for (std::size_t offset = raw_offset; offset < data.size() && result.size() < max_length; ++offset)
			{
				const auto ch = static_cast<unsigned char>(data[offset]);
				if (ch == '\0')
				{
					break;
				}

				result.push_back(static_cast<char>(ch));
			}

			if (result.empty())
			{
				return std::nullopt;
			}

			return result;
		}

		return std::nullopt;
	}

	bool read_file(const std::string& file, std::string* data)
	{
		if (!data) return false;
		data->clear();

		if (file_exists(file))
		{
			std::ifstream stream(file, std::ios::binary);
			if (!stream.is_open()) return false;

			stream.seekg(0, std::ios::end);
			const std::streamsize size = stream.tellg();
			stream.seekg(0, std::ios::beg);

			if (size > -1)
			{
				data->resize(static_cast<uint32_t>(size));
				stream.read(data->data(), size);
				stream.close();
				return true;
			}
		}

		return false;
	}

	size_t file_size(const std::string& file)
	{
		if (file_exists(file))
		{
			std::ifstream stream(file, std::ios::binary);

			if (stream.good())
			{
				stream.seekg(0, std::ios::end);
				return static_cast<size_t>(stream.tellg());
			}
		}

		return 0;
	}

	bool create_directory(const std::string& directory)
	{
		return std::filesystem::create_directories(directory);
	}

	bool directory_exists(const std::string& directory)
	{
		return std::filesystem::is_directory(directory);
	}

	bool directory_is_empty(const std::string& directory)
	{
		return std::filesystem::is_empty(directory);
	}

	std::vector<std::string> list_files(const std::string& directory)
	{
		std::vector<std::string> files;

		for (auto& file : std::filesystem::directory_iterator(directory))
		{
			files.push_back(file.path().generic_string());
		}

		return files;
	}
}
