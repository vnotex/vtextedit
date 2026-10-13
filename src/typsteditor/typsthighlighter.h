#ifndef VTE_TYPSTHIGHLIGHTER_H
#define VTE_TYPSTHIGHLIGHTER_H

#include <QHash>
#include <QTimer>
#include <vtextedit/vsyntaxhighlighter.h>

#include "typstcompiledsnapshot.h"

namespace vte {
class TextEditorConfig;

// Formatting consumes only the asynchronous official-parser result. Block user
// data belongs to the shared editor/layout/spell-check infrastructure.
// Source edits and compiler-generation changes invalidate ranges; rehighlighting,
// theme refreshes and zoom preserve them without modifying source or undo history.
class TypstHighlighter : public VSyntaxHighlighter {
  Q_OBJECT
public:
  TypstHighlighter(QTextDocument *p_document, const TextEditorConfig &p_config);

  void setGeneration(quint64 p_generation);
  void setHighlights(quint64 p_generation, const QVector<TypstHighlight> &p_highlights,
                     const QString &p_activeFile);
  void invalidate();
  void refreshTheme(const TextEditorConfig &p_config, int p_zoomDelta = 0);

signals:
  void highlightingUpdated();

protected:
  void highlightBlock(const QString &p_text) Q_DECL_OVERRIDE;

private:
  struct Range {
    int m_start = 0;
    int m_length = 0;
    QString m_kind;
  };

  QHash<QString, QTextCharFormat> m_formats;
  QHash<int, QVector<Range>> m_blocks;
  QTimer m_refreshTimer;
  quint64 m_generation = 0;
  bool m_acceptHighlights = false;
};
} // namespace vte

#endif // VTE_TYPSTHIGHLIGHTER_H
