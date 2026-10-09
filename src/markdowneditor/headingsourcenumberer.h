#ifndef HEADINGSOURCENUMBERER_H
#define HEADINGSOURCENUMBERER_H

#include <vtextedit/vmarkdowneditor.h>

#include <QElapsedTimer>
#include <QMap>
#include <QObject>
#include <QTextCursor>
#include <QTimer>

class QTextDocument;

namespace vte {
class MarkdownHighlighterResult;

// Source scheduling and state stay in an ordinary QObject child, not in the
// layout of an exported editor or configuration class.
class HeadingSourceNumberer final : public QObject {
  Q_OBJECT

public:
  explicit HeadingSourceNumberer(VMarkdownEditor *p_editor);

  void setProvider(VMarkdownEditor::HeadingSectionNumberProvider p_provider);
  void setActive(bool p_active);
  VMarkdownEditor::HeadingSectionNumberRemovalResult remove();

  // Preserve the source edit's viewport until explicit user interaction.
  bool restoreViewportAfterHighlight();

protected:
  bool eventFilter(QObject *p_object, QEvent *p_event) Q_DECL_OVERRIDE;

private:
  struct SourceHeading;

  struct Replacement {
    int m_position = 0;
    QString m_before;
    QString m_after;
    bool m_separator = false;
  };

  struct Prefix {
    QString m_original;
    QString m_generated;
    QChar m_leftCharacter;
    bool m_separator = false;
  };

  struct OwnedPrefix {
    Prefix m_prefix;
    QTextCursor m_start;
    QTextCursor m_end;
    QTextCursor m_left;
  };

  struct PrefixSnapshot {
    Prefix m_prefix;
    int m_position = 0;
    bool m_hasLeft = false;
  };

  struct Plan {
    QSharedPointer<MarkdownHighlighterResult> m_result;
    QVector<Replacement> m_replacements;
    quint64 m_generation = 0;
    quint64 m_providerGeneration = 0;
    int m_revision = -1;
    int m_eligible = 0;
    bool m_valid = false;
  };

  void observeDocument();
  void cancel();
  void requestNormalization(bool p_sourceEdit);
  void contentsChange(int p_position, int p_removed, int p_added);
  void contentsChanged();
  void queueAttempt();
  void attempt();
  bool canApply();

  bool isCurrent(const QSharedPointer<MarkdownHighlighterResult> &p_result) const;
  bool isCurrent(const Plan &p_plan) const;
  bool resolveHeading(const md::HeadingInfo &p_heading, SourceHeading &p_source) const;
  bool prepare(const QSharedPointer<MarkdownHighlighterResult> &p_result);
  void publishCurrent();
  void apply();
  bool valid(const OwnedPrefix &p_owned) const;
  OwnedPrefix anchorPrefix(const Prefix &p_prefix, int p_position, bool p_hasLeft) const;
  void validateOwnership();
  void rememberOwnership();
  void restoreOwnership(int p_undoSteps);
  void resetHistory();
  void pruneHistory(int p_undoSteps);
  void writeReplacements(const QVector<Replacement> &p_replacements, bool p_removing, bool p_join);
  int mapPosition(const QVector<Replacement> &p_replacements, int p_position,
                  bool p_followInsertion) const;

  VMarkdownEditor *m_editor;
  QTextDocument *m_doc;
  VMarkdownEditor::HeadingSectionNumberProvider m_provider;
  QTimer m_timer;
  QElapsedTimer m_idle;
  Plan m_plan;
  QVector<OwnedPrefix> m_owned;
  QMap<int, QVector<PrefixSnapshot>> m_history;
  QVector<PrefixSnapshot> m_snapshot;
  quint64 m_generation = 0;
  quint64 m_providerGeneration = 0;
  int m_revision = 0;
  int m_characters = 1;
  int m_undoSteps = 0;
  int m_redoSteps = 0;
  int m_triggerUndoSteps = 0;
  bool m_active = false;
  bool m_pending = false;
  bool m_queued = false;
  bool m_applying = false;
  bool m_preparing = false;
  bool m_republish = false;
  bool m_reset = false;
  bool m_sourceChanged = false;
  bool m_newUndoCommand = false;
  bool m_replaySuppressed = false;
  bool m_joinSourceEdit = false;
  bool m_restoreViewport = false;
  int m_horizontalScroll = 0;
  int m_verticalScroll = 0;
};
} // namespace vte

#endif // HEADINGSOURCENUMBERER_H
