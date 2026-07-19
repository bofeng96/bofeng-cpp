#pragma once

#include <cstddef>
#include <expected>
#include <string>

namespace follytask {

struct TaskConfig {
   std::size_t executorThreads;
};

// Call this after folly::Init has parsed the shared command-line flags.
[[nodiscard]] std::expected<TaskConfig, std::string> loadTaskConfig();

}  // namespace follytask
