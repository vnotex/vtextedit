include($$PWD/../common.pri)

QT += widgets

greaterThan(QT_MAJOR_VERSION, 5) {
    QT += core5compat
}

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
