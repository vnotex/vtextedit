#ifndef NETWORKUTILS_H
#define NETWORKUTILS_H

// INTERNAL header. Deliberately NOT under src/include/vtextedit and NOT
// exported: NetworkAccess is an implementation detail of PreviewMgr's image
// downloader. Consumers that need HTTP helpers should provide their own.
//
// Dropping VTEXTEDIT_EXPORT is only half the job: it removes
// __declspec(dllexport) on MSVC, but GCC/Clang default to PUBLIC symbol
// visibility, so the vtable, typeinfo, moc functions and out-of-line members
// would still land in the ELF/Mach-O dynamic symbol table. Q_DECL_HIDDEN
// (__attribute__((visibility("hidden"))); a no-op on MSVC) closes that gap.
//
// Verify after changing this file:
//   Windows: dumpbin /EXPORTS VTextEdit.dll | findstr NetworkAccess
//   Linux:   nm -D -C libVTextEdit.so | grep -E 'vte::Network'
//   macOS:   nm -gU -C libVTextEdit.dylib | grep -E 'vte::Network'
// All three should report no vte::NetworkAccess / NetworkReply / NetworkUtils
// members (PreviewMgr's own exported members may still mention the types in
// their mangled names; that is expected and harmless).
#include <QByteArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPair>
#include <QUrl>
#include <QVector>

namespace vte {
class Q_DECL_HIDDEN NetworkUtils {
public:
  NetworkUtils() = delete;

  static QNetworkRequest networkRequest(const QUrl &p_url);

  static QString networkErrorStr(QNetworkReply::NetworkError p_err);
};

struct Q_DECL_HIDDEN NetworkReply {
  QString errorStr() const;

  QNetworkReply::NetworkError m_error = QNetworkReply::HostNotFoundError;

  QByteArray m_data;
};

class Q_DECL_HIDDEN NetworkAccess : public QObject {
  Q_OBJECT
public:
  typedef QVector<QPair<QByteArray, QByteArray>> RawHeaderPairs;

  explicit NetworkAccess(QObject *p_parent = nullptr);

  void requestAsync(const QUrl &p_url);

  static NetworkReply request(const QUrl &p_url);

  static NetworkReply request(const QUrl &p_url, const RawHeaderPairs &p_rawHeader);

  static NetworkReply put(const QUrl &p_url, const RawHeaderPairs &p_rawHeader,
                          const QByteArray &p_data);

  static NetworkReply post(const QUrl &p_url, const RawHeaderPairs &p_rawHeader,
                           const QByteArray &p_data);

  static NetworkReply deleteResource(const QUrl &p_url, const RawHeaderPairs &p_rawHeader,
                                     const QByteArray &p_data);

signals:
  // Url is the original url of the request.
  void requestFinished(const NetworkReply &p_reply, const QString &p_url);

private:
  static void handleReply(QNetworkReply *p_reply, NetworkReply &p_myReply);

  static NetworkReply sendRequest(const QUrl &p_url, const RawHeaderPairs &p_rawHeader,
                                  const QByteArray &p_action, const QByteArray &p_data);

  QNetworkAccessManager m_netAccessMgr;
};
} // namespace vte

#endif // NETWORKUTILS_H
