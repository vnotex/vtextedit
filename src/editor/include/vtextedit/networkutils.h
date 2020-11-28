#ifndef NETWORKUTILS_H
#define NETWORKUTILS_H

#include "vtextedit_export.h"

#include <QObject>
#include <QUrl>
#include <QByteArray>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>

namespace vte
{
    class NetworkUtils
    {
    public:
        NetworkUtils() = delete;
    };

    class VTEXTEDIT_EXPORT Downloader : public QObject
    {
        Q_OBJECT
    public:
        explicit Downloader(QObject *p_parent = nullptr);

        void downloadAsync(const QUrl &p_url);

        static QByteArray download(const QUrl &p_url);

    signals:
        // Url is the original url of the request.
        void downloadFinished(const QByteArray &p_data, const QString &p_url);

    private:
        static void handleReply(QNetworkReply *p_reply, QByteArray &p_data);

        QNetworkAccessManager m_netAccessMgr;
    };
}

#endif // NETWORKUTILS_H
