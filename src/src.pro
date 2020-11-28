TEMPLATE = subdirs

SUBDIRS += \
    libs \
    editor

editor.depends = libs
