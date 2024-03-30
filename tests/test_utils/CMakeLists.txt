cmake_minimum_required (VERSION 3.12)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(QT_VERSION_MAJOR 6 CACHE STRING "Qt version to use (5 or 6), defaults to 6")
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets Test)
find_package(Qt${QT_VERSION_MAJOR} OPTIONAL_COMPONENTS Core5Compat)

set(SRC_FOLDER ../../src)

qt_add_executable(test_utils
    ${SRC_FOLDER}/include/vtextedit/lrucache.h
    test_utils.cpp test_utils.h
)
target_include_directories(test_utils PRIVATE
    ${SRC_FOLDER}/include
)

target_compile_definitions(test_utils PRIVATE
    VTEXTEDIT_STATIC_DEFINE
)

target_link_libraries(test_utils PRIVATE
    Qt::Core
    Qt::Gui
    Qt::Test
    Qt::Widgets
)

if((QT_VERSION_MAJOR GREATER 5))
    target_link_libraries(test_utils PRIVATE
        Qt::Core5Compat
    )
endif()

install(TARGETS test_utils
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

qt_generate_deploy_app_script(
    TARGET test_utils
    FILENAME_VARIABLE deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
