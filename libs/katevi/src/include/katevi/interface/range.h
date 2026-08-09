#ifndef KATEVII_RANGE_H
#define KATEVII_RANGE_H

#include <katevi/katevi_export.h>

#include <katevi/interface/cursor.h>

namespace KateViI {
class KATEVI_EXPORT Range {
public:
  Range() = default;

  Range(int p_startLine, int p_startColumn, int p_endLine, int p_endColumn)
      : m_start(qMin(Cursor(p_startLine, p_startColumn), Cursor(p_endLine, p_endColumn))),
        m_end(qMax(Cursor(p_startLine, p_startColumn), Cursor(p_endLine, p_endColumn))) {}

  Range(const Cursor &start, const Cursor &end)
      : m_start(qMin(start, end)), m_end(qMax(start, end)) {}

  Cursor start() const { return m_start; }

  Cursor end() const { return m_end; }

  bool isValid() const { return start().isValid() && end().isValid(); }

  bool onSingleLine() const { return start().line() == end().line(); }

  int columnWidth() const { return end().column() - start().column(); }

  void setRange(const Range &range) {
    m_start = range.start();
    m_end = range.end();
  }

  void setRange(const Cursor &start, const Cursor &end) {
    if (start > end) {
      setRange(Range(end, start));
    } else {
      setRange(Range(start, end));
    }
  }

  static Range invalid() { return Range(Cursor::invalid(), Cursor::invalid()); }

public:
  friend bool operator<(const Range &r1, const Range &r2) { return r1.end() < r2.start(); }

  /**
   * Writes this range to the debug output in a nicely formatted way.
   */
  friend QDebug operator<<(QDebug s, const Range &range) {
    s << "[" << " (" << range.m_start.line() << ", " << range.m_start.column() << ")"
      << " -> " << " (" << range.m_end.line() << ", " << range.m_end.column() << ")"
      << "]";
    return s;
  }

private:
  Cursor m_start;
  Cursor m_end;
};
} // namespace KateViI

#endif // KATEVII_RANGE_H
