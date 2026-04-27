cmake_minimum_required(VERSION 3.12)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(QT_DEFAULT_MAJOR_VERSION 6 CACHE STRING "Qt version to use (5 or 6), defaults to 6")
find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Core Gui Widgets Test)

set(LIBS_FOLDER ../../libs)
set(SRC_FOLDER ../../src)

add_executable(test_cmark_probe
    test_cmark_probe.cpp test_cmark_probe.h
    ${SRC_FOLDER}/markdowneditor/cmarkadapter.cpp
    ${SRC_FOLDER}/markdowneditor/markdownastwalker.cpp ${SRC_FOLDER}/markdowneditor/markdownastwalker.h
)
target_include_directories(test_cmark_probe PRIVATE
    ${LIBS_FOLDER}/cmark/src
    ${CMAKE_BINARY_DIR}/libs/cmark/src
    ${SRC_FOLDER}/include
    ${SRC_FOLDER}/markdowneditor
)
target_compile_definitions(test_cmark_probe PRIVATE
    VTEXTEDIT_STATIC_DEFINE
)
target_link_libraries(test_cmark_probe PRIVATE
    Qt::Core
    Qt::Gui
    Qt::Test
    Qt::Widgets
    cmark
)
add_test(NAME test_cmark_probe COMMAND test_cmark_probe)
