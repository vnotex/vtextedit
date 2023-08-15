TARGET = KateVi

QT += core gui widgets core5compat

TEMPLATE = lib

CONFIG += staticlib

DEFINES += KATEVI_STATIC_DEFINE

INCLUDEPATH *= $$PWD/src/include
INCLUDEPATH *= $$PWD/src

include($$PWD/src/src.pri)
