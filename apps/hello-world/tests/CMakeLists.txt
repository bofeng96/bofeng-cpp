add_executable(bofeng_text_tests TextTest.cpp)

target_link_libraries(bofeng_text_tests
    PRIVATE
        hello_world_core
        GTest::gtest_main
        bofeng::project_options
        bofeng::project_warnings
)

gtest_discover_tests(bofeng_text_tests)
