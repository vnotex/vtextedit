#ifndef NETWORKUTILS_H
#define NETWORKUTILS_H

#include "vtextedit_export.h"

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPair>
#include <QUrl>
#include <QVector>

namespace vte {
class VTEXTEDIT_EXPORT NetworkUtils {
public:
  NetworkUtils() = delete;

  static QNetworkRequest networkRequest(const QUrl &p_url);

  static QString networkErrorStr(QNetworkReply::NetworkError p_err);
};

struct VTEXTEDIT_EXPORT NetworkReply {
  QString errorStr() const;

  QNetworkReply::NetworkError m_error = QNetworkReply::HostNotFoundError;

  QByteArray m_data;
};

class VTEXTEDIT_EXPORT NetworkAccess : public QObject {
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
