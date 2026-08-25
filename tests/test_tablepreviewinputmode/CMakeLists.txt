cmake_minimum_required(VERSION 3.12)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(QT_DEFAULT_MAJOR_VERSION 6 CACHE STRING "Qt version to use (5 or 6), defaults to 6")
find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Core Gui Widgets Test)

set(SRC_FOLDER ../../src)
set(MDEDITOR_FOLDER ${SRC_FOLDER}/markdowneditor)
set(LIBS_FOLDER ../../libs)

add_executable(test_tablepreviewinputmode
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
    # The sheet is a VTextEdit now (that is what lets an input mode run inside
    # it), so its base and the small closure that base needs are compiled in
    # too. Still deliberately sources rather than the shared library: this test
    # reaches TablePreviewSheet and TablePreviewDocument, neither of which is
    # exported API.
    ${SRC_FOLDER}/textedit/vtextedit.cpp ${SRC_FOLDER}/include/vtextedit/vtextedit.h
    ${SRC_FOLDER}/textedit/scrollbar.cpp ${SRC_FOLDER}/textedit/scrollbar.h
    ${SRC_FOLDER}/textedit/autoindenthelper.cpp ${SRC_FOLDER}/textedit/autoindenthelper.h
    ${SRC_FOLDER}/inputmode/abstractinputmode.cpp ${SRC_FOLDER}/inputmode/abstractinputmode.h
    ${SRC_FOLDER}/utils/texteditutils.cpp ${SRC_FOLDER}/include/vtextedit/texteditutils.h
    ${SRC_FOLDER}/utils/textutils.cpp ${SRC_FOLDER}/include/vtextedit/textutils.h
    # The sheet installs one of the three input modes, so the mode registry and
    # all three implementations come with it.
    ${MDEDITOR_FOLDER}/tablepreviewinputmode.cpp ${MDEDITOR_FOLDER}/tablepreviewinputmode.h
    ${SRC_FOLDER}/inputmode/inputmodemgr.cpp ${SRC_FOLDER}/inputmode/inputmodemgr.h
    ${SRC_FOLDER}/inputmode/texteditinputmode.cpp ${SRC_FOLDER}/inputmode/texteditinputmode.h
    ${SRC_FOLDER}/inputmode/normalinputmode.cpp ${SRC_FOLDER}/inputmode/normalinputmode.h
    ${SRC_FOLDER}/inputmode/normalinputmodefactory.cpp
    ${SRC_FOLDER}/inputmode/normalinputmodefactory.h
    ${SRC_FOLDER}/inputmode/viinputmode.cpp ${SRC_FOLDER}/inputmode/viinputmode.h
    ${SRC_FOLDER}/inputmode/viinputmodefactory.cpp ${SRC_FOLDER}/inputmode/viinputmodefactory.h
    ${SRC_FOLDER}/inputmode/vscodeinputmode.cpp ${SRC_FOLDER}/inputmode/vscodeinputmode.h
    ${SRC_FOLDER}/inputmode/vscodeinputmodefactory.cpp
    ${SRC_FOLDER}/inputmode/vscodeinputmodefactory.h
    ${SRC_FOLDER}/texteditor/inputmodestatuswidget.h
    # VTextEditTranslate lives here and is a QObject; moc it, or the input mode
    # factories cannot resolve its static metaobject.
    ${SRC_FOLDER}/include/vtextedit/global.h
    test_tablepreviewinputmode.cpp test_tablepreviewinputmode.h
)
target_include_directories(test_tablepreviewinputmode PRIVATE
    ..
    ${SRC_FOLDER}
    ${SRC_FOLDER}/include
    ${MDEDITOR_FOLDER}
    ${LIBS_FOLDER}/cmark/src
    ${CMAKE_BINARY_DIR}/libs/cmark/src
    ${LIBS_FOLDER}/katevi/src
    ${LIBS_FOLDER}/katevi/src/include
)

target_compile_definitions(test_tablepreviewinputmode PRIVATE
    VTEXTEDIT_STATIC_DEFINE
    KATEVI_STATIC_DEFINE
)

target_link_libraries(test_tablepreviewinputmode PRIVATE
    Qt::Core
    Qt::Gui
    Qt::Test
    Qt::Widgets
    cmark
    KateVi
)
add_test(NAME test_tablepreviewinputmode COMMAND test_tablepreviewinputmode)
