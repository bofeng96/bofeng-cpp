add_library(hello_world_core Text.cpp)

target_include_directories(hello_world_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_world_core
    PUBLIC
        Folly::folly
        bofeng::project_options
    PRIVATE
        bofeng::project_warnings
)

add_executable(hello-world main.cpp)

target_link_libraries(hello-world
    PRIVATE
        hello_world_core
        bofeng::project_options
        bofeng::project_warnings
)
