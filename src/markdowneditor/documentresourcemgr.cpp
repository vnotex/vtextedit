#include "documentresourcemgr.h"

using namespace vte;

void DocumentResourceMgr::addImage(const QString &p_name, const QPixmap &p_image) {
  m_images.insert(p_name, p_image);
}

bool DocumentResourceMgr::containsImage(const QString &p_name) const {
  return m_images.contains(p_name);
}

const QPixmap *DocumentResourceMgr::findImage(const QString &p_name) const {
  auto it = m_images.find(p_name);
  if (it != m_images.end()) {
    return &it.value();
  }

  return NULL;
}

void DocumentResourceMgr::clear() { m_images.clear(); }

void DocumentResourceMgr::removeImage(const QString &p_name) { m_images.remove(p_name); }
