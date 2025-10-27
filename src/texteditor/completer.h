#ifndef COMPLETER_H
#define COMPLETER_H

#include <QCompleter>
#include <QPair>
#include <QStringList>
#include <QTextCursor>

class QStringListModel;
class QRect;
class QTextDocument;

namespace vte {
class CompleterInterface {
public:
  virtual ~CompleterInterface() {}

  virtual QTextCursor textCursor() const = 0;

  virtual QString contents() const = 0;

  virtual QWidget *widget() const = 0;

  virtual QString getText(int p_start, int p_end) const = 0;

  virtual void insertCompletion(int p_start, int p_end, const QString &p_completion) = 0;

  virtual qreal scaleFactor() const = 0;

  virtual QTextDocument *document() const = 0;
};

class Completer : public QCompleter {
  Q_OBJECT
public:
  Completer(QObject *p_parent = nullptr);

  void triggerCompletion(CompleterInterface *p_interface, const QStringList &p_candidates,
                         const QPair<int, int> &p_prefixRange, bool p_reversed,
                         const QRect &p_popupRect);

  bool isActive(CompleterInterface *p_interface) const;

  void next(bool p_reversed);

  void execute();

  void finishCompletion();

  // Helper function to find completion prefix.
  // Returns [start, end).
  static QPair<int, int> findCompletionPrefix(CompleterInterface *p_interface);

  // Helper function to generate completion candidates excluding the word
  // specified by [p_wordStart, p_wordEnd).
  static QStringList generateCompletionCandidates(CompleterInterface *p_interface, int p_wordStart,
                                                  int p_wordEnd, bool p_reversed);

protected:
  bool eventFilter(QObject *p_obj, QEvent *p_eve) Q_DECL_OVERRIDE;

private slots:
  void handleContentsChange(int p_position, int p_charsRemoved, int p_charsAdded);

private:
  bool selectRow(int p_row);

  void selectIndex(QModelIndex p_index);

  void cleanUp();

  void updateCompletionPrefix();

  // Will be set when requested to perform a completion.
  CompleterInterface *m_interface = nullptr;

  // Original prefix range.
  QPair<int, int> m_prefixRange;

  // Managed by QObject.
  QStringListModel *m_model = nullptr;

  static const char *c_popupProperty;
};
} // namespace vte

#endif // COMPLETER_H
