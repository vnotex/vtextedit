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
        if(VTEXTEDIT_ENABLE_TYPST)
            list(APPEND DEMO_RUNTIME_COMMANDS
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:VTextEditTypst>" "$<TARGET_FILE_DIR:${DEMO_TARGET}>"
            )
        endif()
    endforeach()
    add_custom_target(VTextEditDemoRuntime
        ${DEMO_RUNTIME_COMMANDS}
        DEPENDS VTextEdit
        VERBATIM
    )
    if(VTEXTEDIT_ENABLE_TYPST)
        add_dependencies(VTextEditDemoRuntime VTextEditTypst)
    endif()
endif()

file(RELATIVE_PATH DEMO_INSTALL_LIBDIR "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
file(RELATIVE_PATH DEMO_BUNDLE_LIBDIR "${CMAKE_INSTALL_PREFIX}" "${CMAKE_INSTALL_FULL_LIBDIR}")
set(SRC_FOLDER "../src")
foreach(DEMO_TARGET IN LISTS DEMO_TARGETS)
    if(WIN32)
        add_dependencies(${DEMO_TARGET} VTextEditDemoRuntime)
    endif()
    if(APPLE)
        set_property(TARGET ${DEMO_TARGET} APPEND PROPERTY INSTALL_RPATH
            "@executable_path/../../../${DEMO_BUNDLE_LIBDIR}")
    elseif(UNIX)
        set_property(TARGET ${DEMO_TARGET} APPEND PROPERTY INSTALL_RPATH
            "$ORIGIN/${DEMO_INSTALL_LIBDIR}")
        set_property(TARGET ${DEMO_TARGET} PROPERTY BUILD_RPATH_USE_ORIGIN TRUE)
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
