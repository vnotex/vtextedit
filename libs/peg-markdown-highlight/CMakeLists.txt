cmake_minimum_required(VERSION 3.16)
project(peg-markdown-highlight VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(QT_VERSION_MAJOR 6 CACHE STRING "Qt version to use (5 or 6), defaults to 6")

find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)

qt_standard_project_setup()

qt_add_library(peg-markdown-highlight STATIC
    pmh_definitions.h
    pmh_parser.c pmh_parser.h
)

install(TARGETS peg-markdown-highlight
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    FRAMEWORK DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
