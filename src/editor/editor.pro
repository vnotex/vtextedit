QT += core gui widgets network svg

TARGET = VTextEdit
TEMPLATE = lib
CONFIG += lib_bundle
CONFIG += shared

CONFIG += file_copies

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

# Copy include files to output
COPIES += vtextedit_include_files
vtextedit_include_files.files = $$files($$PWD/include/vtextedit/*.h)
vtextedit_include_files.path = $$OUT_PWD/include/vtextedit

COPIES += include_files
include_files.files = $$files($$PWD/include/*.h)
include_files.path = $$OUT_PWD/include
