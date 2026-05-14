cmake_minimum_required (VERSION 3.12)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)

set(QT_DEFAULT_MAJOR_VERSION 6 CACHE STRING "Qt version to use (5 or 6), defaults to 6")
find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Core Gui Widgets Test)

set(SRC_FOLDER ../../src)
set(TEXTEDIT_FOLDER ${SRC_FOLDER}/textedit)

add_executable(test_theme
    ${TEXTEDIT_FOLDER}/theme.cpp
    ${SRC_FOLDER}/include/vtextedit/theme.h
    ../utils/utils.cpp ../utils/utils.h
    test_theme.cpp test_theme.h
)
target_include_directories(test_theme PRIVATE
    ..
    ${SRC_FOLDER}/include
    ${TEXTEDIT_FOLDER}
)

target_compile_definitions(test_theme PRIVATE
    VTEXTEDIT_STATIC_DEFINE
)

target_link_libraries(test_theme PRIVATE
    Qt::Core
    Qt::Gui
    Qt::Test
    Qt::Widgets
)
add_test(NAME test_theme COMMAND test_theme)
