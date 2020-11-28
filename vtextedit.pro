TEMPLATE = subdirs

CONFIG += c++11

SUBDIRS += \
    src \
    tests \
    demo

demo.depends = src
tests.depends = src
