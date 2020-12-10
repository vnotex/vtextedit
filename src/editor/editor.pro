QT += core gui widgets network svg

TARGET = VTextEdit
TEMPLATE = lib
CONFIG += shared dll

macx: {
    CONFIG += lib_bundle
    FRAMEWORK_HEADERS.version = Versions
    FRAMEWORK_HEADERS.files = \
        include/vtextedit/codeblockhighlighter.h \
        include/vtextedit/global.h \
        include/vtextedit/lrucache.h \
        include/vtextedit/markdowneditorconfig.h \
        include/vtextedit/markdownutils.h \
        include/vtextedit/networkutils.h \
        include/vtextedit/orderedintset.h \
        include/vtextedit/pegmarkdownhighlighter.h \
        include/vtextedit/pegmarkdownhighlighterdata.h \
        include/vtextedit/previewdata.h \
        include/vtextedit/previewmgr.h \
        include/vtextedit/textblockdata.h \
        include/vtextedit/texteditorconfig.h \
        include/vtextedit/texteditutils.h \
        include/vtextedit/textrange.h \
        include/vtextedit/theme.h \
        include/vtextedit/vmarkdowneditor.h \
        include/vtextedit/vtextedit.h \
        include/vtextedit/vtextedit_export.h \
        include/vtextedit/vtexteditor.h
    FRAMEWORK_HEADERS.path = Headers/vtextedit
    QMAKE_BUNDLE_DATA += FRAMEWORK_HEADERS

    # Process VSyntaxHighlighting framework
    sh_lib_name = VSyntaxHighlighting
    sh_lib_dir = $${OUT_PWD}/../libs/syntax-highlighting
    sh_lib_full_name = $${sh_lib_name}.framework/Versions/1/$${sh_lib_name}
    lib_target = $${TARGET}.framework/Versions/1/$${TARGET}
    QMAKE_POST_LINK = \
        install_name_tool -add_rpath $${sh_lib_dir}/ $${lib_target} && \
        install_name_tool -change $${sh_lib_full_name} @rpath/$${sh_lib_full_name} $${lib_target}
}

DEFINES += VTEXTEDIT_LIB
DEFINES += KATEVI_STATIC_DEFINE

include($$PWD/include/include.pri)

include($$PWD/utils/utils.pri)

include($$PWD/textedit/textedit.pri)

include($$PWD/texteditor/texteditor.pri)

include($$PWD/markdowneditor/markdowneditor.pri)

include($$PWD/inputmode/inputmode.pri)


RESOURCES += $$PWD/data/themes/theme.qrc

INCLUDEPATH += $$PWD
INCLUDEPATH += $$PWD/include

LIBS_FOLDER = $$PWD/../libs
include($$LIBS_FOLDER/syntax-highlighting/syntax-highlighting_export.pri)
include($$LIBS_FOLDER/katevi/katevi_export.pri)
include($$LIBS_FOLDER/peg-markdown-highlight/peg-markdown-highlight_export.pri)

## INSTALLS
unix:!macx {
    isEmpty(PREFIX): PREFIX = /usr
    LIBDIR = $${PREFIX}/lib

    # qmake will automatically do symlinks
    target.path = $${LIBDIR}

    INSTALLS += target
}
