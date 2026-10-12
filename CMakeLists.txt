cmake_minimum_required (VERSION 3.12)
project(VTextEdit VERSION 8.0.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include(GNUInstallDirs)
set(QT_DEFAULT_MAJOR_VERSION 6 CACHE STRING "Qt version to use (5 or 6), defaults to 6")
option(VTEXTEDIT_ENABLE_TYPST "Build the native Typst editor and compiler helper" OFF)

if(VTEXTEDIT_ENABLE_TYPST)
    if(NOT QT_DEFAULT_MAJOR_VERSION EQUAL 6)
        message(FATAL_ERROR "Typst support requires Qt 6. Set QT_DEFAULT_MAJOR_VERSION=6 or disable VTEXTEDIT_ENABLE_TYPST to use Qt 5.")
    endif()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    add_subdirectory(tools/typst-backend)
endif()

add_subdirectory(libs)
add_subdirectory(src)
add_subdirectory(demo)
if(VTEXTEDIT_ENABLE_TYPST)
    add_dependencies(VTextEditDemo vtextedit-typst)
    add_dependencies(VTypstEditorDemo vtextedit-typst)
    install(TARGETS VTextEditDemo VTypstEditorDemo
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        BUNDLE DESTINATION .
    )
endif()
enable_testing()
add_subdirectory(tests)
