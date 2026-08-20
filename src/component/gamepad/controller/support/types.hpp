#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <queue>
#include <compare>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace gamepad::unstable
{
	namespace chrono = std::chrono;
	namespace filesystem = std::filesystem;

	using std::array;
	using std::atomic;
	using std::byte;
	using std::ifstream;
	using std::ios;
	using std::map;
	using std::mutex;
	using std::lock_guard;
	using std::nullopt;
	using std::optional;
	using std::ofstream;
	using std::ostream;
	using std::pair;
	using std::queue;
	using std::runtime_error;
	using std::set;
	using std::span;
	using std::string;
	using std::string_view;
	using std::unique_ptr;
	using std::variant;
	using std::vector;
	using std::wstring;

	using path = std::filesystem::path;

	using std::forward;
	using std::make_unique;
	using std::move;
}
