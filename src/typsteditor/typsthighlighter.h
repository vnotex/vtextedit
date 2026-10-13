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
// Source edits map the last parser ranges until current results replace them.
// Rehighlighting, theme refreshes and zoom never modify source or undo history.
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
    int m_end = 0;
    int m_maxEnd = 0;
    QString m_kind;
  };

  void mapRanges(int p_position, int p_removed, int p_added);

  QHash<QString, QTextCharFormat> m_formats;
  // Source ordered, with prefix maximum ends for nested-range overlap lookup.
  QVector<Range> m_ranges;
  QTimer m_refreshTimer;
  quint64 m_generation = 0;
  bool m_acceptHighlights = false;
};
} // namespace vte

#endif // VTE_TYPSTHIGHLIGHTER_H
