#include "scrollbar.h"

using namespace vte;

ScrollBar::ScrollBar(QWidget *p_parent) : QScrollBar(p_parent) { init(); }

ScrollBar::ScrollBar(Qt::Orientation p_orientation, QWidget *p_parent)
    : QScrollBar(p_orientation, p_parent) {
  init();
}

void ScrollBar::init() {
  connect(this, &QScrollBar::rangeChanged, this, [this](int p_min, int p_max) {
    if (m_skipNextRangeChange) {
      m_skipNextRangeChange = false;
      return;
    }

    if (p_max == p_min) {
      // No need for a scroll bar.
      return;
    }

    // Add extra space to the slider so that we could scroll the
    // bottom of the content widget up.
    m_skipNextRangeChange = true;
    int newMax = p_max + pageStep() - singleStep() * 3;
    setMaximum(newMax);
  });
}
