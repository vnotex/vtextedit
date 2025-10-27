#ifndef EXTRASELECTIONMGR_H
#define EXTRASELECTIONMGR_H

#include <QBrush>
#include <QObject>
#include <QTextCharFormat>
#include <QTextEdit>

class QTimer;

namespace vte {
class ExtraSelectionInterface {
public:
  virtual ~ExtraSelectionInterface() {}

  virtual QTextCursor textCursor() const = 0;

  virtual QString selectedText() const = 0;

  virtual void setExtraSelections(const QList<QTextEdit::ExtraSelection> &p_selections) = 0;

  virtual QList<QTextCursor> findAllText(const QString &p_text, bool p_isRegularExpression,
                                         bool p_caseSensitive) = 0;
};

class ExtraSelectionMgr : public QObject {
  Q_OBJECT
public:
  enum SelectionType { CursorLine = 0, TrailingSpace, Tab, SelectedText, MaxBuiltInSelection };

  explicit ExtraSelectionMgr(ExtraSelectionInterface *p_interface, QObject *p_parent = nullptr);

  void setExtraSelectionEnabled(int p_type, bool p_enabled);

  void setExtraSelectionFormat(int p_type, const QColor &p_foreground, const QColor &p_background,
                               bool p_isFullWidth);

  void setHighlightCursorVisualLineEnabled(bool p_enabled);

  // Register an external extra selection (disabled by default).
  // Return the type of registered extra selection, which could be use
  // to access the properties of it later.
  int registerExtraSelection();

  // Set selections of a given extra selection type.
  // Only valid for external extra selection.
  void setSelections(int p_type, const QList<QTextCursor> &p_selections);

public slots:
  void handleCursorPositionChange();

  void handleContentsChange();

  void handleSelectionChange();

  void updateAllExtraSelections();

private slots:
  void applyExtraSelections();

private:
  void initBuiltInExtraSelections();

  void kickOffExtraSelections();

  bool isExtraSelectionEnabled(SelectionType p_type) const;

  // Update CursorLine and CursorBlock extra selections.
  void highlightCursorLine(bool p_applyNow = true);

  // Trailing space, tabs.
  void highlightWhitespace(bool p_applyNow = true);

  // Return true if need update.
  bool highlightTrailingSpace();

  // Return true if need update.
  bool highlightWhitespaceInternal(const QString &p_text, SelectionType p_type,
                                   const std::function<bool(const QTextCursor &)> &p_filter);

  // Highlight selected text.
  void highlightSelectedText(bool p_applyNow = true);

  void updateOnExtraSelectionChange(int p_type);

  // @p_filter: used to check every selection. Should return false if it
  // is not wanted.
  void
  findAllTextAsExtraSelection(const QString &p_text, bool p_isRegularExpression,
                              bool p_caseSensitive, SelectionType p_type,
                              const QTextCharFormat &p_format,
                              const std::function<bool(const QTextCursor &)> &p_filter = nullptr);

  struct ExtraSelection {
    bool m_enabled = false;
    QColor m_foreground;
    QColor m_background;
    bool m_isFullWidth = false;
    QList<QTextEdit::ExtraSelection> m_selections;

    QTextCharFormat format() const {
      QTextCharFormat fmt;
      if (m_foreground.isValid()) {
        fmt.setForeground(m_foreground);
      }
      if (m_background.isValid()) {
        fmt.setBackground(m_background);
      }
      if (m_isFullWidth) {
        fmt.setProperty(QTextFormat::FullWidthSelection, true);
      }
      return fmt;
    }
  };

  struct CursorData {
    void update(const QTextCursor &p_cursor) {
      m_blockNumber = p_cursor.blockNumber();
      m_positionInBlock = p_cursor.positionInBlock();
      m_columnNumber = p_cursor.columnNumber();
    }

    int m_blockNumber = -1;
    int m_positionInBlock = -1;
    int m_columnNumber = -1;
  };

  ExtraSelectionInterface *m_interface = nullptr;

  QVector<ExtraSelection> m_extraSelections;

  CursorData m_lastCursor;

  // Highlight cursor visual line or block.
  bool m_highlightCursorVisualLineEnabled = true;

  // Managed by QObject.
  QTimer *m_extraSelectionTimer = nullptr;

  // Managed by QObject.
  QTimer *m_whitespaceHighlightTimer = nullptr;

  // Managed by QObject.
  QTimer *m_selectedTextHighlightTimer = nullptr;

  // Whether cursor is right behind trailing space.
  bool m_cursorBehindTrailingSpace = false;
};
} // namespace vte

#endif // EXTRASELECTIONMGR_H
