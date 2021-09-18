QT += core gui widgets network svg

TARGET = VTextEditDemo
TEMPLATE = app

HEADERS += \
    helper.h \
    logger.h

SOURCES += main.cpp

RESOURCES += data/syntax/syntax.qrc \
    data/example_files/example_files.qrc

include($$PWD/../src/editor/editor_export.pri)

include($$PWD/../src/libs/syntax-highlighting/syntax-highlighting_export.pri)

macx {
    # Process VTextEdit framework
    vte_lib_name = VTextEdit
    vte_lib_dir = $${OUT_PWD}/../src/editor
    vte_lib_full_name = $${vte_lib_name}.framework/Versions/1/$${vte_lib_name}
    app_target = $${TARGET}.app/Contents/MacOS/$${TARGET}
    QMAKE_POST_LINK = \
        install_name_tool -add_rpath $${vte_lib_dir} $${app_target} && \
        install_name_tool -change $${vte_lib_full_name} @rpath/$${vte_lib_full_name} $${app_target} && \

    # Process VSyntaxHighlighting framework
    sh_lib_name = VSyntaxHighlighting
    sh_lib_dir = $${OUT_PWD}/../src/libs/syntax-highlighting
    sh_lib_full_name = $${sh_lib_name}.framework/Versions/1/$${sh_lib_name}
    QMAKE_POST_LINK += \
        install_name_tool -add_rpath $${sh_lib_dir} $${app_target} && \
        install_name_tool -change $${sh_lib_full_name} @rpath/$${sh_lib_full_name} $${app_target}
}
