include($$PWD/include/include.pri)

include($$PWD/modes/modes.pri)

include($$PWD/emulatedcommandbar/emulatedcommandbar.pri)

SOURCES += \
    $$PWD/registers.cpp \
    $$PWD/globalstate.cpp \
    $$PWD/history.cpp \
    $$PWD/macros.cpp \
    $$PWD/mappings.cpp \
    $$PWD/completion.cpp \
    $$PWD/keyparser.cpp \
    $$PWD/inputmodemanager.cpp \
    $$PWD/range.cpp \
    $$PWD/command.cpp \
    $$PWD/motion.cpp \
    $$PWD/jumps.cpp \
    $$PWD/searcher.cpp \
    $$PWD/keymapper.cpp \
    $$PWD/viutils.cpp \
    $$PWD/marks.cpp \
    $$PWD/lastchangerecorder.cpp \
    $$PWD/macrorecorder.cpp \
    $$PWD/completionrecorder.cpp \
    $$PWD/completionreplayer.cpp \
    $$PWD/kateviconfig.cpp \
# $$PWD/cmds.cpp

HEADERS += \
    $$PWD/registers.h \
    $$PWD/history.h \
    $$PWD/macros.h \
    $$PWD/mappings.h \
    $$PWD/keyparser.h \
    $$PWD/range.h \
    $$PWD/command.h \
    $$PWD/motion.h \
    $$PWD/jumps.h \
    $$PWD/searcher.h \
    $$PWD/keymapper.h \
    $$PWD/viutils.h \
    $$PWD/marks.h \
    $$PWD/lastchangerecorder.h \
    $$PWD/macrorecorder.h \
    $$PWD/completionrecorder.h \
    $$PWD/completionreplayer.h \
# $$PWD/cmds.h
