include($$PWD/../common.pri)

QT += widgets

TARGET = test_utils
TEMPLATE = app

DEFINES += VTEXTEDIT_STATIC_DEFINE

EDITOR_FOLDER = $$PWD/../../src/editor

SOURCES += \
    test_utils.cpp

HEADERS += \
    test_utils.h \
    $$EDITOR_FOLDER/include/vtextedit/lrucache.h

INCLUDEPATH += $$EDITOR_FOLDER/include
