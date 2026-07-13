add_executable(bofeng_text_tests TextTest.cpp)

target_link_libraries(bofeng_text_tests
    PRIVATE
        hello_world_core
        bofeng::project_options
        bofeng::project_warnings
)

add_test(NAME bofeng_text_tests COMMAND bofeng_text_tests)
