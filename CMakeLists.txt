cmake_minimum_required(VERSION 3.24)

# Prefer Boost's package configuration when using newer CMake versions.
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(bofeng-cpp
    VERSION 0.1.0
    DESCRIPTION "C++ applications built with Meta Folly"
    LANGUAGES CXX
)

set(CMAKE_CXX_EXTENSIONS OFF)

# Folly is provided by Homebrew, vcpkg, or another external package manager.
find_package(folly CONFIG REQUIRED)

# Shared language requirements inherited by every project target.
add_library(bofeng_project_options INTERFACE)
add_library(bofeng::project_options ALIAS bofeng_project_options)
target_compile_features(bofeng_project_options INTERFACE cxx_std_23)

# Shared compiler warnings, selected for the active compiler.
add_library(bofeng_project_warnings INTERFACE)
add_library(bofeng::project_warnings ALIAS bofeng_project_warnings)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
        bofeng_project_warnings
        INTERFACE -Wall -Wextra -Wpedantic
    )
elseif(MSVC)
    target_compile_options(
        bofeng_project_warnings
        INTERFACE /W4 /permissive-
    )
endif()

# Register all applications under apps/.
add_subdirectory(apps)

# BUILD_TESTING can be disabled with -DBUILD_TESTING=OFF.
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
