add_library(folly_task_core TaskExample.cpp)

target_include_directories(folly_task_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(folly_task_core
    PUBLIC
        Folly::folly
        bofeng::project_options
    PRIVATE
        bofeng::project_warnings
)

add_executable(folly-task
    TaskConfig.cpp
    main.cpp
)

target_link_libraries(folly-task
    PRIVATE
        bofeng::flags
        folly_task_core
        bofeng::project_warnings
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
