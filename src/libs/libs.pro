TEMPLATE = subdirs

SUBDIRS += \
    syntax-highlighting \
    katevi \
    peg-markdown-highlight \
    hunspell \
    sonnet-data \
    sonnet-core \
    sonnet-hunspell

sonnet-data.subdir = sonnet/data
sonnet-core.subdir = sonnet/src/core
sonnet-hunspell.subdir = sonnet/src/plugins/hunspell

sonnet-core.depends = hunspell sonnet-data
sonnet-hunspell.depends = sonnet-core
