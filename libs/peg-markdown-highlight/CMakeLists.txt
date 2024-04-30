cmake_minimum_required(VERSION 3.16)
project(peg-markdown-highlight VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(QT_DEFAULT_MAJOR_VERSION 6 CACHE STRING "Qt version to use (5 or 6), defaults to 6")

find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Core)

if ((QT_DEFAULT_MAJOR_VERSION GREATER 5))
    qt_standard_project_setup()
else()
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)
endif()

add_library(peg-markdown-highlight STATIC
    pmh_definitions.h
    pmh_parser.c pmh_parser.h
)
