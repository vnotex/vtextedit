INCLUDEPATH *= $$PWD/src/include

DEPENDPATH *= $$PWD/src
DEPENDPATH *= $$PWD/src/include
DEPENDPATH *= $$PWD/src/modes
DEPENDPATH *= $$PWD/src/emulatedcommandbar

OUT_FOLDER = $$absolute_path($$relative_path($$PWD, $$_PRO_FILE_PWD_), $$OUT_PWD)
win32:CONFIG(release, debug|release) {
    LIBS += $$OUT_FOLDER/release/KateVi.lib
    # For static library, we need to add this depends to let Qt re-build the target
    # when there is a change in the library.
    PRE_TARGETDEPS += $$OUT_FOLDER/release/KateVi.lib
} else:win32:CONFIG(debug, debug|release) {
    LIBS += $$OUT_FOLDER/debug/KateVi.lib
    PRE_TARGETDEPS += $$OUT_FOLDER/debug/KateVi.lib
} else:unix {
    LIBS += $$OUT_FOLDER/libKateVi.a
    PRE_TARGETDEPS += $$OUT_FOLDER/libKateVi.a
}
