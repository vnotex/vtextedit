QT += core gui widgets network svg

TARGET = VTextEditDemo
TEMPLATE = app

HEADERS += \
    helper.h \
    logger.h

SOURCES += main.cpp

RESOURCES += data/syntax/syntax.qrc \
    data/example_files/example_files.qrc

include($$PWD/../src/editor/editor_export.pri)
