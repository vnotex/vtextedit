#ifndef KATEVII_CURSOR_H
#define KATEVII_CURSOR_H

#include <katevi/katevi_export.h>

#include <QDebug>
#include <QString>

namespace KateViI {
class KATEVI_EXPORT Cursor {
public:
  Cursor() = default;

  Cursor(int p_line, int p_column) : m_line(p_line), m_column(p_column) {}

  Q_DECL_CONSTEXPR bool isValid() const { return m_line >= 0 && m_column >= 0; }

  // Returns the cursor position as string in the format "(line, column)".
  QString toString() const {
    return QLatin1Char('(') + QString::number(m_line) + QLatin1String(", ") +
           QString::number(m_column) + QLatin1Char(')');
  }

  Q_DECL_CONSTEXPR int line() const { return m_line; }

  void setLine(int p_line) { m_line = p_line; }

  Q_DECL_CONSTEXPR int column() const { return m_column; }

  void setColumn(int p_column) { m_column = p_column; }

  void setPosition(int line, int column) {
    m_line = line;
    m_column = column;
  }

  static Cursor invalid() { return Cursor(-1, -1); }

  static Cursor start() { return Cursor(); }

public:
  friend bool operator>(const Cursor &c1, const Cursor &c2) {
    return c1.line() > c2.line() || (c1.line() == c2.line() && c1.m_column > c2.m_column);
  }

  friend bool operator>=(const Cursor &c1, const Cursor &c2) {
    return c1.line() > c2.line() || (c1.line() == c2.line() && c1.m_column >= c2.m_column);
  }

  friend bool operator<=(const Cursor &c1, const Cursor &c2) { return !(c1 > c2); }

  friend bool operator<(const Cursor &c1, const Cursor &c2) { return !(c1 >= c2); }

  friend bool operator==(const Cursor &c1, const Cursor &c2) {
    return c1.line() == c2.line() && c1.column() == c2.column();
  }

  friend QDebug operator<<(QDebug s, const Cursor &cursor) {
    s.nospace() << "(" << cursor.line() << ", " << cursor.column() << ")";
    return s.space();
  }

  friend bool operator!=(const Cursor &c1, const Cursor &c2) { return !(c1 == c2); }

private:
  int m_line = 0;
  int m_column = 0;
};
} // namespace KateViI

#endif // KATEVII_CURSOR_H
