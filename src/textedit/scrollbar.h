#ifndef SCROLLBAR_H
#define SCROLLBAR_H

#include <QScrollBar>

namespace vte {
// ScrollBar with extra space at the end of the widget.
class ScrollBar : public QScrollBar {
  Q_OBJECT
public:
  explicit ScrollBar(QWidget *p_parent = nullptr);

  ScrollBar(Qt::Orientation p_orientation, QWidget *p_parent = nullptr);

private:
  void init();

  // Whether the extension below is the one currently changing the range, so
  // the handler does not recurse into itself. Held for the duration of that
  // call rather than armed for "the next" signal: the extension can work out
  // to the maximum the bar already has, in which case setMaximum() emits
  // nothing at all and an armed flag would go on to swallow the next genuine
  // range change instead.
  bool m_adjustingRange = false;
};
} // namespace vte

#endif // SCROLLBAR_H
