cmake_minimum_required(VERSION 3.12)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(QT_DEFAULT_MAJOR_VERSION 6 CACHE STRING "Qt version to use (5 or 6), defaults to 6")
find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Core Gui Widgets Test)

set(SRC_FOLDER ../../src)
set(MARKDOWNEDITOR_FOLDER ${SRC_FOLDER}/markdowneditor)
set(LIBS_FOLDER ../../libs)

add_executable(test_astwalker
    ${MARKDOWNEDITOR_FOLDER}/markdownastwalker.cpp ${MARKDOWNEDITOR_FOLDER}/markdownastwalker.h
    ${MARKDOWNEDITOR_FOLDER}/cmarkadapter.cpp ${MARKDOWNEDITOR_FOLDER}/cmarkadapter.h
    ${MARKDOWNEDITOR_FOLDER}/highlightelement.h
    test_astwalker.cpp test_astwalker.h
)
target_include_directories(test_astwalker PRIVATE
    ..
    ${SRC_FOLDER}/include
    ${MARKDOWNEDITOR_FOLDER}
    ${LIBS_FOLDER}/cmark/src
    ${CMAKE_BINARY_DIR}/libs/cmark/src
)
target_compile_definitions(test_astwalker PRIVATE
    VTEXTEDIT_STATIC_DEFINE
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../test_markdownparser/fixtures"
    GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../test_goldenmaster/golden"
)
target_link_libraries(test_astwalker PRIVATE
    Qt::Core
    Qt::Gui
    Qt::Test
    Qt::Widgets
    cmark
)
add_test(NAME test_astwalker COMMAND test_astwalker)
