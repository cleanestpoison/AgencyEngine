#pragma once

// Stands in for the game PCH when the ledger is compiled outside the plugin.
//
// PendingImpulse.h names std types without including them — inside the plugin
// PCH.h has already pulled them in, and adding includes there purely for the
// test build would be the test dictating the shape of production code. So the
// test target force-includes this instead, and it carries exactly the std
// headers PCH.h supplies and nothing from CommonLibSSE.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::literals;
