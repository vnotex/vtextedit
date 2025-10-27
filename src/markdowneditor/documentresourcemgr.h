#ifndef DOCUMENTRESOURCEMGR_H
#define DOCUMENTRESOURCEMGR_H

#include <QHash>
#include <QPixmap>
#include <QString>

namespace vte {
class DocumentResourceMgr {
public:
  // Add an image to the resource with @p_name as the key.
  // If @p_name already exists in the resources, it will update it.
  void addImage(const QString &p_name, const QPixmap &p_image);

  // Remove image @p_name.
  void removeImage(const QString &p_name);

  // Whether the resources contains image with name @p_name.
  bool containsImage(const QString &p_name) const;

  const QPixmap *findImage(const QString &p_name) const;

  void clear();

private:
  // All the images resources.
  // QPixmap is implicit data shared.
  QHash<QString, QPixmap> m_images;
};
} // namespace vte

#endif // DOCUMENTRESOURCEMGR_H
