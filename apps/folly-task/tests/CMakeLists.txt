add_executable(folly_task_tests TaskExampleTest.cpp)

target_link_libraries(folly_task_tests
    PRIVATE
        folly_task_core
        GTest::gtest_main
        bofeng::project_options
        bofeng::project_warnings
)

gtest_discover_tests(folly_task_tests)
