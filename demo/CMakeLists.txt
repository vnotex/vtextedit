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

if(WIN32)
    # Refresh the DLL even when the demo executable does not need relinking.
    # One target serializes copies when both demos share a runtime directory.
    set(DEMO_RUNTIME_COMMANDS)
    foreach(DEMO_TARGET IN LISTS DEMO_TARGETS)
        list(APPEND DEMO_RUNTIME_COMMANDS
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_FILE_DIR:${DEMO_TARGET}>"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:VTextEdit>"
                "$<TARGET_FILE_DIR:${DEMO_TARGET}>"
        )
    endforeach()
    add_custom_target(VTextEditDemoRuntime
        ${DEMO_RUNTIME_COMMANDS}
        DEPENDS VTextEdit
        VERBATIM
    )
endif()

set(SRC_FOLDER "../src")
foreach(DEMO_TARGET IN LISTS DEMO_TARGETS)
    if(WIN32)
        add_dependencies(${DEMO_TARGET} VTextEditDemoRuntime)
    endif()
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
