cmake_minimum_required(VERSION 3.12)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(QT_DEFAULT_MAJOR_VERSION 6 CACHE STRING "Qt version to use (5 or 6), defaults to 6")
find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Core Gui Widgets Test)

set(SRC_FOLDER ../../src)

# Deliberately links the shared library and uses only exported API: this is the
# same surface an application consumes, so it also catches export and metatype
# problems on Windows.
add_executable(test_interactivepreview
    test_interactivepreview.cpp test_interactivepreview.h
)
target_include_directories(test_interactivepreview PRIVATE
    ..
    ${SRC_FOLDER}/include
    # AbstractInputMode is reachable through the exported
    # VTextEdit::getInputMode(), but its header does not live under include/.
    # Only that header is used from here; nothing else internal is compiled in.
    ${SRC_FOLDER}
)
target_link_libraries(test_interactivepreview PRIVATE
    Qt::Core
    Qt::Gui
    Qt::Test
    Qt::Widgets
    VTextEdit
)
if(WIN32)
    add_custom_command(TARGET test_interactivepreview POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:VTextEdit>
            $<TARGET_FILE_DIR:test_interactivepreview>
    )
endif()
add_test(NAME test_interactivepreview COMMAND test_interactivepreview)
