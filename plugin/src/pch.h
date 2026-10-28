#pragma once

#define NOMINMAX  // keep Windows.h from defining min/max macros over std::min/std::max

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <spdlog/sinks/basic_file_sink.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std::literals;
namespace logs = SKSE::log;
