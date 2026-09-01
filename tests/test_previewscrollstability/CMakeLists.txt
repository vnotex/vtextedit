cmake_minimum_required(VERSION 3.12)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(QT_DEFAULT_MAJOR_VERSION 6 CACHE STRING "Qt version to use (5 or 6), defaults to 6")
find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Core Gui Widgets Test)

set(SRC_FOLDER ../../src)

# Links the shared library and uses only exported API, exactly like
# test_previewbenchmark: EditorPreviewMgr is internal, so the compensation is
# driven through PreviewMgr::updateCodeBlocks() and observed on the scrollbar.
add_executable(test_previewscrollstability
    test_previewscrollstability.cpp test_previewscrollstability.h
)
target_include_directories(test_previewscrollstability PRIVATE
    ..
    ${SRC_FOLDER}/include
)
target_link_libraries(test_previewscrollstability PRIVATE
    Qt::Core
    Qt::Gui
    Qt::Test
    Qt::Widgets
    VTextEdit
)
if(WIN32)
    # Attached to THIS target: a sibling target's post-build step does not run
    # when this one is built alone, and the miss shows up as 0xc0000135.
    add_custom_command(TARGET test_previewscrollstability POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:VTextEdit>
            $<TARGET_FILE_DIR:test_previewscrollstability>
    )
endif()
add_test(NAME test_previewscrollstability COMMAND test_previewscrollstability)
set_tests_properties(test_previewscrollstability PROPERTIES TIMEOUT 300)
