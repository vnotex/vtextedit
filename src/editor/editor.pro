QT += core gui widgets network svg

TARGET = VTextEdit
TEMPLATE = lib
CONFIG += lib_bundle
CONFIG += shared dll

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
