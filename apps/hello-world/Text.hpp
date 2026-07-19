#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace helloworld {

// Declarations shared by the hello-world application and its tests.
[[nodiscard]] std::string greeting(std::string_view name);

[[nodiscard]] std::string formatTools(
   const std::vector<std::string>& tools);

}  // namespace helloworld
