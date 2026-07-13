add_library(folly_task_core TaskExample.cpp)

target_include_directories(folly_task_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(folly_task_core
    PUBLIC
        Folly::folly
        bofeng::project_options
    PRIVATE
        bofeng::project_warnings
)

add_executable(folly-task main.cpp)

target_link_libraries(folly-task
    PRIVATE
        folly_task_core
        bofeng::project_warnings
)
