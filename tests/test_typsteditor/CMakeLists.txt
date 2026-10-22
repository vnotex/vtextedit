cmake_minimum_required(VERSION 3.21)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Pdf Svg Test)

# Exercise the exported editor, not a second source-compiled implementation.
add_executable(test_typsteditor test_typsteditor.cpp test_typsteditor.h)
target_include_directories(test_typsteditor PRIVATE ../../src/include)
target_link_libraries(test_typsteditor PRIVATE
    VTextEdit VTextEditTypst Qt::Core Qt::Gui Qt::Widgets Qt6::Pdf Qt6::Svg Qt::Test
)
if(WIN32)
    # Refresh both required DLLs even when the test executable does not relink.
    add_custom_target(test_typsteditor_runtime
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:test_typsteditor>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:VTextEdit>" "$<TARGET_FILE:VTextEditTypst>"
            "$<TARGET_FILE_DIR:test_typsteditor>"
        DEPENDS VTextEdit VTextEditTypst
        VERBATIM
    )
    add_dependencies(test_typsteditor test_typsteditor_runtime)
endif()
add_test(NAME test_typsteditor COMMAND test_typsteditor)
set_tests_properties(test_typsteditor PROPERTIES TIMEOUT 900)
