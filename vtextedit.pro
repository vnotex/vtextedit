TEMPLATE = subdirs

CONFIG += c++14

SUBDIRS += \
    src \
    tests \
    demo

demo.depends = src
tests.depends = src
