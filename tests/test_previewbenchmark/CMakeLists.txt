cmake_minimum_required(VERSION 3.12)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(QT_DEFAULT_MAJOR_VERSION 6 CACHE STRING "Qt version to use (5 or 6), defaults to 6")
find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Core Gui Widgets Test)

set(SRC_FOLDER ../../src)

# Links the shared library and uses only exported API, exactly like
# test_interactivepreview: the host is an internal class, so every counter this
# benchmark reads has to arrive as a QObject dynamic property.
add_executable(test_previewbenchmark
    test_previewbenchmark.cpp test_previewbenchmark.h
)
target_include_directories(test_previewbenchmark PRIVATE
    ..
    ${SRC_FOLDER}/include
)
target_compile_definitions(test_previewbenchmark PRIVATE
    EVIDENCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../.sisyphus/evidence"
)
target_link_libraries(test_previewbenchmark PRIVATE
    Qt::Core
    Qt::Gui
    Qt::Test
    Qt::Widgets
    VTextEdit
)
if(WIN32)
    # Attached to THIS target: a sibling target's post-build step does not run
    # when this one is built alone, and the miss shows up as 0xc0000135.
    add_custom_command(TARGET test_previewbenchmark POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:VTextEdit>
            $<TARGET_FILE_DIR:test_previewbenchmark>
    )
endif()
add_test(NAME test_previewbenchmark COMMAND test_previewbenchmark)
# Six scenarios, each driving many parse generations over the default fixture
# (50 tables of 4x4, i.e. 800 cells; raise it with VTE_PREVIEW_BENCHMARK_TABLES).
# Comfortably past the 60 s ctest default on a loaded runner in a Debug build.
set_tests_properties(test_previewbenchmark PROPERTIES TIMEOUT 900)
