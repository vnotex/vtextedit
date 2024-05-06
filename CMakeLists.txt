cmake_minimum_required (VERSION 3.12)
project(VTextEdit VERSION 1.0.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(libs)
add_subdirectory(src)
add_subdirectory(demo)
add_subdirectory(tests)
