TEMPLATE = subdirs

SUBDIRS += \
    syntax-highlighting \
    katevi \
    peg-markdown-highlight \
    hunspell \
    sonnet-core \
    sonnet-hunspell

sonnet-core.subdir = sonnet/src/core
sonnet-hunspell.subdir = sonnet/src/plugins/hunspell

sonnet-core.depends = hunspell
sonnet-hunspell.depends = sonnet-core
