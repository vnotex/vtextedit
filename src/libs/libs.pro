TEMPLATE = subdirs

SUBDIRS += \
    syntax-highlighting \
    katevi \
    peg-markdown-highlight \
    sonnet-data \
    sonnet-core

sonnet-data.subdir = sonnet/data
sonnet-core.subdir = sonnet/src/core

sonnet-core.depends = sonnet-data
