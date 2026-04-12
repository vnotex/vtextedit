# vtextedit Agent Development Guide

## Build/Lint/Test Commands

* **Build**: `mkdir build && cd build && cmake .. && cmake --build . --config Release`
* **Build (Qt 5)**: Pass `-DQT_DEFAULT_MAJOR_VERSION=5` to cmake
* **Format**: Use `.clang-format` (2-space indent, 100-column limit, `PointerAlignment: Right`, `BreakBeforeBraces: Attach`). A pre-commit hook at `scripts/pre-commit` auto-formats staged `.cpp/.h` files (excludes `libs/`)
* **Run All Tests**: `cd build && ctest` (after building)
* **Run Single Test**: `cd build && ctest -R test_textfolding` (pattern matching)
* **Tests are enabled by default** — `add_subdirectory(tests)` is present in root `CMakeLists.txt`
* **Test targets**: `test_textfolding`, `test_utils`

## C++ Code Style Guidelines

### Standards & Tooling

* C++14, Qt 5/6 dual-support via `QT_DEFAULT_MAJOR_VERSION` cache variable
* Qt modules required: Core, Gui, Network, Svg, Widgets, LinguistTools; optional: Core5Compat (Qt 6)
* `CMAKE_AUTOMOC`/`AUTOUIC`/`AUTORCC` enabled (Qt 6 uses `qt_standard_project_setup()`)

### Naming Conventions

* **Namespace**: `vte` for all library classes. Tests use `tests` namespace
* **Classes**: CamelCase — `VTextEdit`, `TextFolding`, `TextBlockRange`. Nested classes allowed (`VTextEdit::Selection`)
* **Methods**: camelCase — `getContentsSeq()`, `isValid()`, `hasFoldedFolding()`
* **Getters**: Prefer `get` prefix for accessors returning owned data (`getTextEdit()`, `getSelection()`, `getConfig()`). Short-form accessors without prefix are used for Qt-conventional names (`document()`, `theme()`, `statusWidget()`) and boolean queries (`isReadOnly()`, `isModified()`, `isCompletionActive()`)
* **Members**: Prefix `m_` for private/protected members — `m_doc`, `m_start`, `m_config`
* **Parameters**: Prefix `p_` for all function parameters — `p_parent`, `p_config`, `p_event`
* **Pointers/References**: Right-aligned — `int *ptr`, `const QString &p_text` (enforced by `.clang-format`)
* **Static members**: Prefix `s_` — `s_instanceCount`

### Include Order

Includes are grouped by scope (separated by blank lines where applicable):

1. Own public header (`<vtextedit/vtextedit.h>`)
2. Internal non-public headers (`<inputmode/...>`, `<vtextedit/...>` other public headers)
3. Third-party library headers (`<katevi/...>`)
4. Local private headers (`"autoindenthelper.h"`, `"scrollbar.h"`)
5. Qt system headers (`<QTextBlock>`, `<QTimer>`, etc.)

Note: actual ordering varies slightly between files. Follow the pattern in the file you are editing.

### Header Guards & Declarations

* Public headers: `#ifndef VTEXTEDIT_CLASSNAME_H` — e.g., `VTEXTEDIT_VTEXTEDIT_H`, `VTEXTEDIT_GLOBAL_H`
* Internal headers: shorter guards — `TEXTFOLDING_H`, `VTE_NONCOPYABLE_H`
* Test headers: `TESTS_TEST_CLASSNAME_H`
* Forward-declare classes to minimize includes — `class QTextDocument;`, `class QMenu;`

### Qt Patterns

* `Q_OBJECT` macro for all `QObject`-derived classes
* `Q_DECL_OVERRIDE` instead of C++11 `override` keyword
* `Q_DECLARE_FLAGS` / `Q_DECLARE_OPERATORS_FOR_FLAGS` for flag enums
* `Q_ASSERT` for debug assertions
* Signals/slots with `connect()`; no `SIGNAL()`/`SLOT()` macro style
* Qt containers: `QVector`, `QHash`, `QList`, `QString`, `QPair`

### Memory & Ownership

* `QSharedPointer` for config objects and shared ownership — `QSharedPointer<TextEditorConfig>`
* `QScopedPointer` for exclusive ownership
* Raw pointers for Qt parent-child managed objects (parent deletes child)
* `Noncopyable` base class available at `src/utils/noncopyable.h` for move-only types

### Exports

* `VTEXTEDIT_EXPORT` macro (from `vtextedit_export.h`) on all public API classes
* Defined as `Q_DECL_EXPORT` when building (`VTEXTEDIT_LIB` defined), `Q_DECL_IMPORT` when consuming
* Tests compile with `VTEXTEDIT_STATIC_DEFINE` (empty macro) to link sources directly

## Testing

* Framework: QtTest (`#include <QtTest>`, link `Qt::Test`)
* Test classes inherit `QObject`, test methods are `private slots`
* Entry point via `QTEST_MAIN(tests::TestClassName)` at end of `.cpp`
* Assertions: `QVERIFY()`, `QCOMPARE()`, `Q_ASSERT()`
* Setup/teardown: `initTestCase()` / `cleanupTestCase()` (once), `cleanup()` (per test)
* Tests live in `tests/test_<name>/` with their own `CMakeLists.txt`
* Each test compiles needed source files directly (no library dependency) with `VTEXTEDIT_STATIC_DEFINE`
* Shared test utilities in `tests/utils/`

## Architecture

### Overview

vtextedit is a reusable Qt widget library providing rich text editing components for [VNote](https://github.com/vnotex/vnote). Licensed LGPL-3.0.

### Class Hierarchy

```
QTextEdit
  └── VTextEdit           (src/textedit/) — base edit widget, cursor, selection, input method
QWidget
  └── VTextEditor         (src/texteditor/) — wraps VTextEdit, adds syntax highlight, Vi mode, folding, completion
        └── VMarkdownEditor (src/markdowneditor/) — adds Markdown parsing, in-place preview
```

Note: `VTextEditor` is a `QWidget` that **contains** a `VTextEdit`, not a subclass of it.

### Source Layout

| Directory | Purpose |
|---|---|
| `src/include/vtextedit/` | Public API headers (installed) |
| `src/textedit/` | `VTextEdit` implementation — base editor, scrollbar, auto-indent |
| `src/texteditor/` | `VTextEditor` implementation — folding, syntax, completer, extra selections, indicators border |
| `src/markdowneditor/` | `VMarkdownEditor` — PEG parser, preview, document layout, code block highlight |
| `src/inputmode/` | Input mode abstraction — Normal, Vi (`katevi`), VSCode modes |
| `src/spellcheck/` | Spell check integration (Hunspell via Sonnet) |
| `src/utils/` | Shared utilities — `Noncopyable`, text/markdown/network utils |
| `src/data/` | Translations (`.ts`/`.qm`) and theme resources (`.qrc`) |
| `demo/` | Standalone demo application |
| `tests/` | QtTest-based unit tests |

### Third-Party Libraries (`libs/`)

| Library | Purpose | Source |
|---|---|---|
| `syntax-highlighting` | KDE Syntax Highlighting (KSyntaxHighlighting) | git submodule (vnotex fork) |
| `katevi` | KDE Vi input mode engine | bundled |
| `peg-markdown-highlight` | PEG-based Markdown parser | bundled |
| `sonnet` | Spell checking framework | git submodule (vnotex fork) |
| `hunspell` | Spell check dictionary backend | git submodule (vnotex fork) |

### Config Pattern

Configs are heap-allocated and shared via `QSharedPointer`:

```cpp
auto editorConfig = QSharedPointer<TextEditorConfig>::create();
auto markdownConfig = QSharedPointer<MarkdownEditorConfig>::create(editorConfig);
auto paras = QSharedPointer<TextEditorParameters>::create();
auto editor = new VMarkdownEditor(markdownConfig, paras, parent);
```

`MarkdownEditorConfig` wraps a `TextEditorConfig` (composition, not inheritance).