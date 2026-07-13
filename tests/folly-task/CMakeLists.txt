add_executable(folly_task_tests TaskExampleTest.cpp)

target_link_libraries(folly_task_tests
    PRIVATE
        folly_task_core
        bofeng::project_options
        bofeng::project_warnings
)

add_test(NAME folly_task_tests COMMAND folly_task_tests)
