cmake_minimum_required(VERSION 3.12)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(QT_DEFAULT_MAJOR_VERSION 6 CACHE STRING "Qt version to use (5 or 6), defaults to 6")
find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Core Gui Widgets Test)

set(SRC_FOLDER ../../src)
set(MDEDITOR_FOLDER ${SRC_FOLDER}/markdowneditor)
set(LIBS_FOLDER ../../libs)

add_executable(test_tablepreview
    ${SRC_FOLDER}/include/vtextedit/preview.h
    ${SRC_FOLDER}/include/vtextedit/previewwidget.h
    ${MDEDITOR_FOLDER}/preview.cpp ${MDEDITOR_FOLDER}/previewbuilder.h
    ${MDEDITOR_FOLDER}/previewlogging.cpp ${MDEDITOR_FOLDER}/previewlogging.h
    ${MDEDITOR_FOLDER}/previewwidget.cpp
    ${MDEDITOR_FOLDER}/tablepreviewwidget.cpp ${MDEDITOR_FOLDER}/tablepreviewwidget.h
    ${MDEDITOR_FOLDER}/hlformatresolver.cpp ${MDEDITOR_FOLDER}/hlformatresolver.h
    ${SRC_FOLDER}/utils/htmltablescanner.cpp ${SRC_FOLDER}/include/vtextedit/htmltablescanner.h
    ${SRC_FOLDER}/utils/htmlimgscanner.cpp ${SRC_FOLDER}/include/vtextedit/htmlimgscanner.h
    ${SRC_FOLDER}/utils/htmltagparse.cpp ${SRC_FOLDER}/utils/htmltagparse.h
    test_tablepreview.cpp test_tablepreview.h
)
target_include_directories(test_tablepreview PRIVATE
    ..
    ${SRC_FOLDER}
    ${SRC_FOLDER}/include
    ${MDEDITOR_FOLDER}
    ${LIBS_FOLDER}/cmark/src
    ${CMAKE_BINARY_DIR}/libs/cmark/src
)

target_compile_definitions(test_tablepreview PRIVATE
    VTEXTEDIT_STATIC_DEFINE
)

target_link_libraries(test_tablepreview PRIVATE
    Qt::Core
    Qt::Gui
    Qt::Test
    Qt::Widgets
    cmark
)
add_test(NAME test_tablepreview COMMAND test_tablepreview)
