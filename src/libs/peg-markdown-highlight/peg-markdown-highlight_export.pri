INCLUDEPATH *= $$PWD

DEPENDPATH *= $$PWD

OUT_FOLDER = $$absolute_path($$relative_path($$PWD, $$_PRO_FILE_PWD_), $$OUT_PWD)
win32:CONFIG(release, debug|release) {
    LIBS += $$OUT_FOLDER/release/peg-markdown-highlight.lib
    # For static library, we need to add this depends to let Qt re-build the target
    # when there is a change in the library.
    PRE_TARGETDEPS += $$OUT_FOLDER/release/peg-markdown-highlight.lib
} else:win32:CONFIG(debug, debug|release) {
    LIBS += $$OUT_FOLDER/debug/peg-markdown-highlight.lib
    PRE_TARGETDEPS += $$OUT_FOLDER/debug/peg-markdown-highlight.lib
} else:unix {
    LIBS += $$OUT_FOLDER/libpeg-markdown-highlight.a
    PRE_TARGETDEPS += $$OUT_FOLDER/libpeg-markdown-highlight.a
}
