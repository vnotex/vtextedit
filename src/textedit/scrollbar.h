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

  bool m_skipNextRangeChange = false;
};
} // namespace vte

#endif // SCROLLBAR_H
