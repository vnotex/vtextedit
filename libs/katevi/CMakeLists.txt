cmake_minimum_required(VERSION 3.16)
project(KateVi VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(QT_DEFAULT_MAJOR_VERSION 6 CACHE STRING "Qt version to use (5 or 6), defaults to 6")
find_package(Qt${QT_DEFAULT_MAJOR_VERSION} REQUIRED COMPONENTS Core Gui Widgets)
find_package(Qt${QT_DEFAULT_MAJOR_VERSION} OPTIONAL_COMPONENTS Core5Compat)

if ((QT_DEFAULT_MAJOR_VERSION GREATER 5))
    qt_standard_project_setup()
else()
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)
endif()

add_library(KateVi STATIC
    src/command.cpp src/command.h
    src/completion.cpp
    src/completionrecorder.cpp src/completionrecorder.h
    src/completionreplayer.cpp src/completionreplayer.h
    src/emulatedcommandbar/activemode.cpp src/emulatedcommandbar/activemode.h
    src/emulatedcommandbar/commandmode.cpp src/emulatedcommandbar/commandmode.h
    src/emulatedcommandbar/completer.cpp src/emulatedcommandbar/completer.h
    src/emulatedcommandbar/emulatedcommandbar.cpp
    src/globalstate.cpp
    src/history.cpp src/history.h
    src/include/katevi/completion.h
    src/include/katevi/definitions.h
    src/include/katevi/emulatedcommandbar.h
    src/include/katevi/global.h
    src/include/katevi/globalstate.h
    src/include/katevi/inputmodemanager.h
    src/include/katevi/interface/command.h
    src/include/katevi/interface/cursor.h
    src/include/katevi/interface/kateviconfig.h
    src/include/katevi/interface/katevieditorinterface.h
    src/include/katevi/interface/kateviinputmode.h
    src/include/katevi/interface/kcompletion.h
    src/include/katevi/interface/mark.h
    src/include/katevi/interface/markinterface.h
    src/include/katevi/interface/movingcursor.h
    src/include/katevi/interface/range.h
    src/include/katevi/katevi_export.h
    src/inputmodemanager.cpp
    src/jumps.cpp src/jumps.h
    src/kateviconfig.cpp
    src/keymapper.cpp src/keymapper.h
    src/keyparser.cpp src/keyparser.h
    src/lastchangerecorder.cpp src/lastchangerecorder.h
    src/macrorecorder.cpp src/macrorecorder.h
    src/macros.cpp src/macros.h
    src/mappings.cpp src/mappings.h
    src/marks.cpp src/marks.h
    src/modes/insertvimode.cpp src/modes/insertvimode.h
    src/modes/modebase.cpp src/modes/modebase.h
    src/modes/normalvimode.cpp src/modes/normalvimode.h
    src/modes/replacevimode.cpp src/modes/replacevimode.h
    src/modes/visualvimode.cpp src/modes/visualvimode.h
    src/motion.cpp src/motion.h
    src/range.cpp src/range.h
    src/registers.cpp src/registers.h
    src/searcher.cpp src/searcher.h
    src/viutils.cpp src/viutils.h
)
target_include_directories(KateVi PUBLIC
    src
    src/include
)

target_compile_definitions(KateVi PUBLIC
    KATEVI_STATIC_DEFINE
)

target_link_libraries(KateVi PUBLIC
    Qt::Core
    Qt::Gui
    Qt::Widgets
)

if((QT_DEFAULT_MAJOR_VERSION GREATER 5))
    target_link_libraries(KateVi PUBLIC
        Qt::Core5Compat
    )
endif()
