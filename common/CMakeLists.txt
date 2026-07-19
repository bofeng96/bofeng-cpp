add_library(bofeng_flags Flags.cpp)
add_library(bofeng::flags ALIAS bofeng_flags)

target_include_directories(
    bofeng_flags
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
    bofeng_flags
    PUBLIC
        bofeng::project_options
        gflags
    PRIVATE
        bofeng::project_warnings
)
