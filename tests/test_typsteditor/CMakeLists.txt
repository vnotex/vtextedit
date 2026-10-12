cmake_minimum_required(VERSION 3.12)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Pdf Test)

# Exercise the exported editor, not a second source-compiled implementation.
add_executable(test_typsteditor test_typsteditor.cpp test_typsteditor.h)
target_include_directories(test_typsteditor PRIVATE ../../src/include)
target_link_libraries(test_typsteditor PRIVATE
    VTextEdit Qt::Core Qt::Gui Qt::Widgets Qt6::Pdf Qt::Test
)
target_compile_definitions(test_typsteditor PRIVATE
    VTEXTEDIT_TYPST_HELPER="${VTEXTEDIT_TYPST_HELPER}"
)
add_dependencies(test_typsteditor vtextedit-typst)
# Enabled builds already put both targets in the shared runtime bin directory.
if(WIN32 AND NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    add_custom_command(TARGET test_typsteditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:VTextEdit> $<TARGET_FILE_DIR:test_typsteditor>
    )
endif()
add_test(NAME test_typsteditor COMMAND test_typsteditor)
set_tests_properties(test_typsteditor PROPERTIES TIMEOUT 900)
