TARGET = KateVi

QT += core gui widgets

greaterThan(QT_MAJOR_VERSION, 5) {
    QT += core5compat
}

TEMPLATE = lib

CONFIG += staticlib

DEFINES += KATEVI_STATIC_DEFINE

INCLUDEPATH *= $$PWD/src/include
INCLUDEPATH *= $$PWD/src

include($$PWD/src/src.pri)
