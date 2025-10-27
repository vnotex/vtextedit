#include <vtextedit/viconfig.h>

#include <katevi/interface/kateviconfig.h>

using namespace vte;

QJsonObject ViConfig::toJson() const {
  QJsonObject obj;
  obj[QStringLiteral("control_c_to_copy")] = m_controlCToCopy;
  return obj;
}

void ViConfig::fromJson(const QJsonObject &p_jobj) {
  m_controlCToCopy = p_jobj[QStringLiteral("control_c_to_copy")].toBool();
}

QSharedPointer<KateViI::KateViConfig> ViConfig::toKateViConfig() const {
  auto kateConfig = QSharedPointer<KateViI::KateViConfig>::create();

  if (m_controlCToCopy) {
    // TODO: do it via key mappings should be better.
    // On macOS, it is Command+C to copy.
    kateConfig->skipKey(Qt::Key_C, Qt::ControlModifier);
    kateConfig->skipKey(Qt::Key_X, Qt::ControlModifier);
  }

  return kateConfig;
}
