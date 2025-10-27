#include <katevi/interface/kateviconfig.h>

#include <QKeySequence>

using namespace KateViI;

KateViConfig::Key::Key(int p_key, Qt::KeyboardModifiers p_modifiers)
    : m_key(p_key), m_modifiers(p_modifiers) {}

KateViConfig::Key::Key(const QString &p_key) {
  QKeySequence seq(p_key);
  if (seq.count() == 0) {
    return;
  }

  const int keyMask = 0x01FFFFFF;
  m_key = seq[0] & keyMask;
  m_modifiers = static_cast<Qt::KeyboardModifiers>(seq[0] & (~keyMask));
}

bool KateViConfig::Key::operator==(const Key &p_other) const {
  return m_key == p_other.m_key && m_modifiers == p_other.m_modifiers;
}

bool KateViConfig::Key::operator<(const Key &p_other) const {
  if (m_key < p_other.m_key) {
    return true;
  } else if (m_key > p_other.m_key) {
    return false;
  } else {
    return m_modifiers < p_other.m_modifiers;
  }
}

QString KateViConfig::Key::toString() const {
  QKeySequence seq(m_key | m_modifiers);
  return seq.toString();
}

KateViConfig::KateViConfig() { initSkippedKeys(); }

int KateViConfig::tabWidth() const { return m_tabWidth; }

void KateViConfig::setTabWidth(int p_width) { m_tabWidth = p_width; }

bool KateViConfig::wordCompletionRemoveTail() const { return m_wordCompletionRemoveTailEnabled; }

bool KateViConfig::stealShortcut() const { return m_stealShortcut; }

bool KateViConfig::shouldSkipKey(int p_key, Qt::KeyboardModifiers p_modifiers) const {
  if (m_skippedKeys.find(Key(p_key, p_modifiers)) != m_skippedKeys.end()) {
    return true;
  }

  return false;
}

void KateViConfig::initSkippedKeys() {
  m_skippedKeys.emplace(Qt::Key_Tab, Qt::ControlModifier);
  m_skippedKeys.emplace(Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier);
}

void KateViConfig::skipKey(int p_key, Qt::KeyboardModifiers p_modifiers) {
  m_skippedKeys.emplace(p_key, p_modifiers);
}
