#ifndef VTEXTEDIT_VICONFIG_H
#define VTEXTEDIT_VICONFIG_H

#include <QJsonObject>
#include <QSharedPointer>

#include "vtextedit_export.h"

namespace KateViI {
class KateViConfig;
}

namespace vte {
class VTEXTEDIT_EXPORT ViConfig {
public:
  ViConfig() = default;

  QJsonObject toJson() const;
  void fromJson(const QJsonObject &p_jobj);

  QSharedPointer<KateViI::KateViConfig> toKateViConfig() const;

  bool m_controlCToCopy = false;
};
} // namespace vte

#endif
