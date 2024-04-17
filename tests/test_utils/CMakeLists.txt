cmake_minimum_required (VERSION 3.12)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(QT_DEFAULT_MAJOR_VERSION 6 CACHE STRING "Qt version to use (5 or 6), defaults to 6")
find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Core Gui Widgets Test)
find_package(Qt${QT_DEFAULT_MAJOR_VERSION} OPTIONAL_COMPONENTS Core5Compat)

set(SRC_FOLDER ../../src)

add_executable(test_utils
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

if((QT_DEFAULT_MAJOR_VERSION GREATER 5))
    target_link_libraries(test_utils PRIVATE
        Qt::Core5Compat
    )
endif()
