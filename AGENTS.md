# vtextedit Agent Development Guide

## Build/Lint/Test Commands

* **Build**: `mkdir build && cd build && cmake .. && cmake --build . --config Release`
* **Format**: Use `.clang-format` with 2-space indent, 100 column limit, PointerAlignment: Right
* **Run All Tests**: `cd build && ctest` (after building)
* **Run Single Test**: `cd build && ctest -R test_textfolding` (pattern matching test name)
* **Enable Tests**: Uncomment or ensure `add_subdirectory(tests)` exists in root CMakeLists.txt

## C++ Code Style Guidelines

* **Standards**: C++14, Qt 5/6 framework with CMAKE_AUTOMOC/AUTOUIC/AUTORCC enabled
* **Includes**: System includes first, then Qt includes (`<Q...>`), then vtextedit public headers (`<vtextedit/...>`), then local headers (`"..."`). See `vtextedit.cpp:1-22`
* **Namespace**: Use `vte` namespace for all library classes (NOT `vnotex`)
* **Headers**: Use `#ifndef VTEXTEDIT_CLASSNAME_H` guards, forward declare classes (e.g., `class QTextDocument;`)
* **Classes**: CamelCase (e.g., `VTextEdit`, `TextFolding`). Nested classes allowed (e.g., `VTextEdit::Selection`)
* **Methods**: camelCase (e.g., `getContentsSeq()`, `isValid()`). Use `get` prefix for getters
* **Members**: Prefix private/protected members with `m_` (e.g., `m_doc`, `m_start`)
* **Pointers**: Right-aligned (`int *ptr`, not `int* ptr`)
* **Qt Patterns**: Use Q_OBJECT macro for QObject-derived classes, signals/slots, Qt containers (QVector, QString)
* **Memory**: Prefer Qt smart pointers (QScopedPointer, QSharedPointer), raw pointers for Qt parent-owned objects
* **Testing**: Use QtTest framework (`#include <QtTest>`, inherit from QObject, private slots for test methods)
* **Exports**: Use `VTEXTEDIT_EXPORT` macro for public API classes
* **Formatting**: Follow `.clang-format`: 2-space indent, braces on same line (Attach), no spaces in parens

## Architecture Notes
* **Library**: vtextedit is a reusable Qt widget library for VNote
* **Main Classes**: `VTextEdit` (base), `VTextEditor` (with syntax), `VMarkdownEditor` (with preview)
* **Config Pattern**: Use QSharedPointer for config objects (e.g., `QSharedPointer<TextEditorConfig>`)
* **Subdirs**: `src/textedit/` (base), `src/texteditor/` (editor), `src/markdowneditor/` (markdown), `src/inputmode/` (vi/vscode modes)