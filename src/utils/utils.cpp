#include "utils.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QToolButton>

using namespace vte;

void Utils::sleepWait(int p_milliseconds) {
  if (p_milliseconds <= 0) {
    return;
  }

  QElapsedTimer t;
  t.start();
  while (t.elapsed() < p_milliseconds) {
    QCoreApplication::processEvents();
  }
}

void Utils::removeMenuIndicator(QToolButton *p_btn) {
  p_btn->setStyleSheet("QToolButton::menu-indicator { image: none; }");
}

bool Utils::isFilePath(const QString &p_name) {
  return p_name.contains(QRegularExpression(QStringLiteral("[\\\\/]")));
}
