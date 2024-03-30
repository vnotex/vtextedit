cmake_minimum_required(VERSION 3.16)
project(VTextEditDemo VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(QT_VERSION_MAJOR 6 CACHE STRING "Qt version to use (5 or 6), defaults to 6")
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Network Svg Widgets)
find_package(Qt${QT_VERSION_MAJOR} OPTIONAL_COMPONENTS Core5Compat)

qt_standard_project_setup()

qt_add_executable(VTextEditDemo WIN32 MACOSX_BUNDLE
    helper.h
    logger.h
    main.cpp
    data/syntax/syntax.qrc
    data/example_files/example_files.qrc
)

set(LIBS_FOLDER "../libs")
set(SRC_FOLDER "../src")
target_include_directories(VTextEditDemo PRIVATE
    ${SRC_FOLDER}/include
)
target_link_directories(VTextEditDemo PRIVATE
    ${SRC_FOLDER}
)

target_link_libraries(VTextEditDemo PRIVATE
    Qt::Core
    Qt::Gui
    Qt::Network
    Qt::Svg
    Qt::Widgets
    VTextEdit
)

if((QT_VERSION_MAJOR GREATER 5))
    target_link_libraries(VTextEditDemo PUBLIC
        Qt::Core5Compat
    )
endif()

install(TARGETS VTextEditDemo
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

qt_generate_deploy_app_script(
    TARGET VTextEditDemo
    FILENAME_VARIABLE deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
