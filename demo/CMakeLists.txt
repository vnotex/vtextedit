cmake_minimum_required(VERSION 3.21)
project(VTextEditDemo VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Qt6 6.9 REQUIRED COMPONENTS Core Gui Network Svg Widgets)

qt_standard_project_setup()

set(DEMO_SOURCES
    helper.h
    logger.h
    main.cpp
    data/example_files/example_files.qrc
)

add_executable(VTextEditDemo WIN32 MACOSX_BUNDLE ${DEMO_SOURCES})
set(DEMO_TARGETS VTextEditDemo)
if(VTEXTEDIT_ENABLE_TYPST)
    add_executable(VTypstEditorDemo WIN32 MACOSX_BUNDLE ${DEMO_SOURCES})
    target_compile_definitions(VTypstEditorDemo PRIVATE VTEXTEDIT_TYPST_DEMO)
    list(APPEND DEMO_TARGETS VTypstEditorDemo)
endif()

set(SRC_FOLDER "../src")
foreach(DEMO_TARGET IN LISTS DEMO_TARGETS)
    target_include_directories(${DEMO_TARGET} PRIVATE
        ${SRC_FOLDER}/include
    )
    target_link_directories(${DEMO_TARGET} PRIVATE
        ${SRC_FOLDER}
    )

    target_link_libraries(${DEMO_TARGET} PRIVATE
        Qt::Core
        Qt::Gui
        Qt::Network
        Qt::Svg
        Qt::Widgets
        VTextEdit
    )

endforeach()
