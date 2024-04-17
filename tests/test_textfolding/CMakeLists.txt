cmake_minimum_required (VERSION 3.12)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(QT_DEFAULT_MAJOR_VERSION 6 CACHE STRING "Qt version to use (5 or 6), defaults to 6")
find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Core Gui Widgets Test)

set(SRC_FOLDER ../../src)
set(EDITOR_FOLDER ${SRC_FOLDER}/texteditor)

add_executable(test_textfolding
    ${SRC_FOLDER}/include/vtextedit/textrange.h
    ${EDITOR_FOLDER}/extraselectionmgr.cpp ${EDITOR_FOLDER}/extraselectionmgr.h
    ${EDITOR_FOLDER}/textfolding.cpp ${EDITOR_FOLDER}/textfolding.h
    ../utils/utils.cpp ../utils/utils.h
    test_textfolding.cpp test_textfolding.h
)
target_include_directories(test_textfolding PRIVATE
    ..
    ${SRC_FOLDER}/include
    ${EDITOR_FOLDER}
)

target_compile_definitions(test_textfolding PRIVATE
    VTEXTEDIT_STATIC_DEFINE
)

target_link_libraries(test_textfolding PRIVATE
    Qt::Core
    Qt::Gui
    Qt::Test
    Qt::Widgets
)
