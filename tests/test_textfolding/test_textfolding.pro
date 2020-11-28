include($$PWD/../common.pri)

QT += widgets

TARGET = test_textfolding
TEMPLATE = app

DEFINES += VTEXTEDIT_STATIC_DEFINE

EDITOR_FOLDER = $$PWD/../../src/editor

include($$PWD/../utils/utils.pri)

SOURCES += \
    test_textfolding.cpp \
    $$EDITOR_FOLDER/texteditor/textfolding.cpp \
    $$EDITOR_FOLDER/texteditor/extraselectionmgr.cpp

HEADERS += \
    test_textfolding.h \
    $$EDITOR_FOLDER/texteditor/textfolding.h \
    $$EDITOR_FOLDER/texteditor/extraselectionmgr.h \
    $$EDITOR_FOLDER/include/vtextedit/textrange.h

INCLUDEPATH += $$PWD/..
INCLUDEPATH += $$EDITOR_FOLDER/include
INCLUDEPATH += $$EDITOR_FOLDER/texteditor
