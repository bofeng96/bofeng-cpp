#include "TaskConfig.hpp"

#include "Flags.hpp"

#include <expected>
#include <string>

namespace follytask {

std::expected<TaskConfig, std::string> loadTaskConfig() {
   if (FLAGS_executor_threads != 2 && FLAGS_executor_threads != 4 &&
      FLAGS_executor_threads != 6) {
      return std::unexpected{
         "--executor_threads must be 2, 4, or 6"};
   }

   return TaskConfig{
      .executorThreads = FLAGS_executor_threads,
   };
}

}  // namespace follytask
