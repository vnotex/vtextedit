OUT_FOLDER = $$absolute_path($$relative_path($$PWD, $$_PRO_FILE_PWD_), $$OUT_PWD)
lib_name = VTextEdit
macx {
    lib_framework_name = $${lib_name}.framework

    INCLUDEPATH += $${OUT_FOLDER}/$${lib_framework_name}/Headers
    LIBS += -F$${OUT_FOLDER} -framework $${lib_name}
} else {
    INCLUDEPATH *= $$PWD/include
    win32:CONFIG(release, debug|release) {
        LIBS += -L$$OUT_FOLDER/release -l$${lib_name}
    } else:win32:CONFIG(debug, debug|release) {
        LIBS += -L$$OUT_FOLDER/debug -l$${lib_name}
    } else:unix {
        LIBS += -L$$OUT_FOLDER -l$${lib_name}
    }
}
