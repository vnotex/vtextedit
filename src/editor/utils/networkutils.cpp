#include <vtextedit/networkutils.h>

#include "utils.h"

using namespace vte;

Downloader::Downloader(QObject *p_parent)
    : QObject(p_parent)
{
    connect(&m_netAccessMgr, &QNetworkAccessManager::finished,
            this, [this](QNetworkReply *p_reply) {
                QByteArray data;
                Downloader::handleReply(p_reply, data);
                // The url() of the reply may be redirected and different from that of the request.
                emit downloadFinished(data, p_reply->request().url().toString());
            });
}

static QNetworkRequest networkRequest(const QUrl &p_url)
{
    QNetworkRequest request(p_url);
    /*
    QSslConfiguration config = QSslConfiguration::defaultConfiguration();
    config.setProtocol(QSsl::SslV3);
    config.setPeerVerifyMode(QSslSocket::VerifyNone);
    request.setSslConfiguration(config);
    */

    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    return request;
}

void Downloader::downloadAsync(const QUrl &p_url)
{
    if (!p_url.isValid()) {
        return;
    }

    m_netAccessMgr.get(networkRequest(p_url));
}

QByteArray Downloader::download(const QUrl &p_url)
{
    QByteArray data;
    if (!p_url.isValid()) {
        return data;
    }

    bool finished = false;
    QNetworkAccessManager netAccessMgr;
    connect(&netAccessMgr, &QNetworkAccessManager::finished,
            [&data, &finished](QNetworkReply *p_reply) {
                Downloader::handleReply(p_reply, data);
                finished = true;
            });

    netAccessMgr.get(networkRequest(p_url));

    while (!finished) {
        Utils::sleepWait(100);
    }

    return data;
}

void Downloader::handleReply(QNetworkReply *p_reply, QByteArray &p_data)
{
    if (p_reply->error() != QNetworkReply::NoError) {
        qWarning() << "download reply error" << p_reply->error() << p_reply->request().url();
    }

    p_data = p_reply->readAll();
    p_reply->deleteLater();
}
