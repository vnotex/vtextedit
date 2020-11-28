INCLUDEPATH *= $$PWD/include

OUT_FOLDER = $$absolute_path($$relative_path($$PWD, $$_PRO_FILE_PWD_), $$OUT_PWD)
win32:CONFIG(release, debug|release) {
    LIBS += -L$$OUT_FOLDER/release -lvtextedit
} else:win32:CONFIG(debug, debug|release) {
    LIBS += -L$$OUT_FOLDER/debug -lvtextedit
} else:unix {
    LIBS += -L$$OUT_FOLDER -lVTextEdit
}

LIBS_FOLDER = $$PWD/../libs
include($$LIBS_FOLDER/syntax-highlighting/syntax-highlighting_export.pri)
