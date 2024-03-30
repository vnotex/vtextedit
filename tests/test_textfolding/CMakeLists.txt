cmake_minimum_required (VERSION 3.12)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(QT_VERSION_MAJOR 6 CACHE STRING "Qt version to use (5 or 6), defaults to 6")
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets Test)

set(SRC_FOLDER ../../src)
set(EDITOR_FOLDER ${SRC_FOLDER}/texteditor)

qt_add_executable(test_textfolding
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

install(TARGETS test_textfolding
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

qt_generate_deploy_app_script(
    TARGET test_textfolding
    FILENAME_VARIABLE deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
