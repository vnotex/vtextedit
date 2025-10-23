cmake_minimum_required(VERSION 3.16)
project(VTextEdit VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(QT_DEFAULT_MAJOR_VERSION 6 CACHE STRING "Qt version to use (5 or 6), defaults to 6")
find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Core Gui Network Svg Widgets LinguistTools)
find_package(Qt${QT_DEFAULT_MAJOR_VERSION} OPTIONAL_COMPONENTS Core5Compat)

if ((QT_DEFAULT_MAJOR_VERSION GREATER 5))
    qt_standard_project_setup()
else()
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)
endif()

set(LIBS_FOLDER "../libs")

set(TRANS_FILES
    data/translations/vtextedit_zh_CN.ts
    data/translations/vtextedit_ja.ts
)
qt_add_translation(QM_FILES ${TRANS_FILES})

add_library(VTextEdit SHARED
    ${QM_FILES}
    data/themes/theme.qrc
    ${LIBS_FOLDER}/sonnet/src/core/trigrams.qrc
    include/vtextedit/blocksegment.h
    include/vtextedit/codeblockhighlighter.h
    include/vtextedit/global.h
    include/vtextedit/lrucache.h
    include/vtextedit/markdowneditorconfig.h
    include/vtextedit/markdownutils.h
    include/vtextedit/networkutils.h
    include/vtextedit/orderedintset.h
    include/vtextedit/pegmarkdownhighlighter.h
    include/vtextedit/pegmarkdownhighlighterdata.h
    include/vtextedit/previewdata.h
    include/vtextedit/previewmgr.h
    include/vtextedit/spellchecker.h
    include/vtextedit/textblockdata.h
    include/vtextedit/texteditorconfig.h
    include/vtextedit/texteditutils.h
    include/vtextedit/textrange.h
    include/vtextedit/textutils.h
    include/vtextedit/theme.h
    include/vtextedit/viconfig.h
    include/vtextedit/vmarkdowneditor.h
    include/vtextedit/vsyntaxhighlighter.h
    include/vtextedit/vtextedit.h
    include/vtextedit/vtextedit_export.h
    include/vtextedit/vtexteditor.h
    inputmode/abstractinputmode.cpp inputmode/abstractinputmode.h
    inputmode/abstractinputmodefactory.h
    inputmode/inputmodeeditorinterface.h
    inputmode/inputmodemgr.cpp inputmode/inputmodemgr.h
    inputmode/normalinputmode.cpp inputmode/normalinputmode.h
    inputmode/normalinputmodefactory.cpp inputmode/normalinputmodefactory.h
    inputmode/viinputmode.cpp inputmode/viinputmode.h
    inputmode/viinputmodefactory.cpp inputmode/viinputmodefactory.h
    inputmode/vscodeinputmode.cpp inputmode/vscodeinputmode.h
    inputmode/vscodeinputmodefactory.cpp inputmode/vscodeinputmodefactory.h
    markdowneditor/codeblockhighlighter.cpp
    markdowneditor/documentresourcemgr.cpp markdowneditor/documentresourcemgr.h
    markdowneditor/editorpegmarkdownhighlighter.cpp markdowneditor/editorpegmarkdownhighlighter.h
    markdowneditor/editorpreviewmgr.cpp markdowneditor/editorpreviewmgr.h
    markdowneditor/ksyntaxcodeblockhighlighter.cpp markdowneditor/ksyntaxcodeblockhighlighter.h
    markdowneditor/markdowneditorconfig.cpp
    markdowneditor/peghighlightblockdata.h
    markdowneditor/peghighlighterresult.cpp markdowneditor/peghighlighterresult.h
    markdowneditor/pegmarkdownhighlighter.cpp
    markdowneditor/pegparser.cpp markdowneditor/pegparser.h
    markdowneditor/previewdata.cpp
    markdowneditor/previewmgr.cpp
    markdowneditor/textdocumentlayout.cpp markdowneditor/textdocumentlayout.h
    markdowneditor/textdocumentlayoutdata.h
    markdowneditor/vmarkdowneditor.cpp
    markdowneditor/webcodeblockhighlighter.cpp markdowneditor/webcodeblockhighlighter.h
    spellcheck/spellchecker.cpp
    spellcheck/spellcheckhighlighthelper.cpp spellcheck/spellcheckhighlighthelper.h
    textedit/autoindenthelper.cpp textedit/autoindenthelper.h
    textedit/scrollbar.cpp textedit/scrollbar.h
    textedit/textblockdata.cpp
    textedit/theme.cpp
    textedit/vtextedit.cpp
    texteditor/blockspellcheckdata.h
    texteditor/completer.cpp texteditor/completer.h
    texteditor/editorcompleter.cpp texteditor/editorcompleter.h
    texteditor/editorextraselection.cpp texteditor/editorextraselection.h
    texteditor/editorindicatorsborder.cpp texteditor/editorindicatorsborder.h
    texteditor/editorinputmode.cpp texteditor/editorinputmode.h
    texteditor/extraselectionmgr.cpp texteditor/extraselectionmgr.h
    texteditor/formatcache.cpp texteditor/formatcache.h
    texteditor/indicatorsborder.cpp texteditor/indicatorsborder.h
    texteditor/inputmodestatuswidget.h
    texteditor/ksyntaxhighlighterwrapper.cpp texteditor/ksyntaxhighlighterwrapper.h
    texteditor/plaintexthighlighter.cpp texteditor/plaintexthighlighter.h
    texteditor/statusindicator.cpp texteditor/statusindicator.h
    texteditor/syntaxhighlighter.cpp texteditor/syntaxhighlighter.h
    texteditor/texteditorconfig.cpp
    texteditor/textfolding.cpp texteditor/textfolding.h
    texteditor/viconfig.cpp
    texteditor/vsyntaxhighlighter.cpp
    texteditor/vtexteditor.cpp
    utils/markdownutils.cpp
    utils/networkutils.cpp
    utils/noncopyable.h
    utils/texteditutils.cpp
    utils/textutils.cpp
    utils/utils.cpp utils/utils.h
)
target_include_directories(VTextEdit PUBLIC
    include
)

target_compile_definitions(VTextEdit PRIVATE
    VTEXTEDIT_LIB
)

target_include_directories(VTextEdit PUBLIC
    ${LIBS_FOLDER}/syntax-highlighting/src/lib
    ${LIBS_FOLDER}/syntax-highlighting/autogenerated
    ${LIBS_FOLDER}/syntax-highlighting/autogenerated/src/lib
    ${LIBS_FOLDER}/katevi/src
    ${LIBS_FOLDER}/katevi/src/include
    ${LIBS_FOLDER}/peg-markdown-highlight
    ${LIBS_FOLDER}/sonnet/src/core
)
target_link_directories(VTextEdit PUBLIC
    ${LIBS_FOLDER}/syntax-highlighting
    ${LIBS_FOLDER}/katevi
    ${LIBS_FOLDER}/peg-markdown-highlight
    ${LIBS_FOLDER}/sonnet/src/core
    ${LIBS_FOLDER}/hunspell
)

target_link_libraries(VTextEdit PUBLIC
    Qt::Core
    Qt::Gui
    Qt::Network
    Qt::Svg
    Qt::Widgets
    VSyntaxHighlighting
    KateVi
    peg-markdown-highlight
    sonnet-core
    sonnet-hunspell
    Hunspell
)

if((QT_DEFAULT_MAJOR_VERSION GREATER 5))
    target_link_libraries(VTextEdit PUBLIC
        Qt::Core5Compat
    )
endif()

install(TARGETS VTextEdit
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    FRAMEWORK DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
