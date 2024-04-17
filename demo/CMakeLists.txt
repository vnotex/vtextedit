cmake_minimum_required(VERSION 3.16)
project(VTextEditDemo VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(QT_DEFAULT_MAJOR_VERSION 6 CACHE STRING "Qt version to use (5 or 6), defaults to 6")
find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Core Gui Network Svg Widgets)
find_package(Qt${QT_DEFAULT_MAJOR_VERSION} OPTIONAL_COMPONENTS Core5Compat)

if ((QT_DEFAULT_MAJOR_VERSION GREATER 5))
    qt_standard_project_setup()
else()
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)
endif()

add_executable(VTextEditDemo WIN32 MACOSX_BUNDLE
    helper.h
    logger.h
    main.cpp
    data/syntax/syntax.qrc
    data/example_files/example_files.qrc
)

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

if((QT_DEFAULT_MAJOR_VERSION GREATER 5))
    target_link_libraries(VTextEditDemo PUBLIC
        Qt::Core5Compat
    )
endif()
