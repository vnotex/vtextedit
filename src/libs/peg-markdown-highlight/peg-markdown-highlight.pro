# PEG-Markdown-Highlight
# GitHub: https://github.com/tamlok/peg-markdown-highlight

QT -= core gui

TARGET = peg-markdown-highlight

TEMPLATE = lib

CONFIG += warn_off
CONFIG += staticlib

SOURCES += pmh_parser.c

HEADERS += pmh_parser.h \
    pmh_definitions.h
