#include <vtextedit/networkutils.h>

#include <QMetaEnum>

#include "utils.h"

using namespace vte;

QNetworkRequest NetworkUtils::networkRequest(const QUrl &p_url)
{
    QNetworkRequest request(p_url);
    /*
    QSslConfiguration config = QSslConfiguration::defaultConfiguration();
    config.setProtocol(QSsl::SslV3);
    config.setPeerVerifyMode(QSslSocket::VerifyNone);
    request.setSslConfiguration(config);
    */

    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, true);
    return request;
}

QString NetworkUtils::networkErrorStr(QNetworkReply::NetworkError p_err)
{
    static const auto indexOfEnum = QNetworkReply::staticMetaObject.indexOfEnumerator("NetworkError");
    const auto metaEnum = QNetworkReply::staticMetaObject.enumerator(indexOfEnum);
    return metaEnum.key(p_err);
}

QString NetworkReply::errorStr() const
{
    return NetworkUtils::networkErrorStr(m_error);
}

NetworkAccess::NetworkAccess(QObject *p_parent)
    : QObject(p_parent)
{
    connect(&m_netAccessMgr, &QNetworkAccessManager::finished,
            this, [this](QNetworkReply *p_reply) {
                NetworkReply reply;
                NetworkAccess::handleReply(p_reply, reply);
                // The url() of the reply may be redirected and different from that of the request.
                emit requestFinished(reply, p_reply->request().url().toString());
            });
}

void NetworkAccess::requestAsync(const QUrl &p_url)
{
    if (!p_url.isValid()) {
        return;
    }

    m_netAccessMgr.get(NetworkUtils::networkRequest(p_url));
}

NetworkReply NetworkAccess::request(const QUrl &p_url)
{
    return request(p_url, RawHeaderPairs());
}

NetworkReply NetworkAccess::request(const QUrl &p_url, const RawHeaderPairs &p_rawHeader)
{
    return sendRequest(p_url, p_rawHeader, "GET", QByteArray());
}

NetworkReply NetworkAccess::put(const QUrl &p_url, const RawHeaderPairs &p_rawHeader, const QByteArray &p_data)
{
    return sendRequest(p_url, p_rawHeader, "PUT", p_data);
}

NetworkReply NetworkAccess::post(const QUrl &p_url, const RawHeaderPairs &p_rawHeader, const QByteArray &p_data)
{
    return sendRequest(p_url, p_rawHeader, "POST", p_data);
}

NetworkReply NetworkAccess::deleteResource(const QUrl &p_url, const RawHeaderPairs &p_rawHeader, const QByteArray &p_data)
{
    return sendRequest(p_url, p_rawHeader, "DELETE", p_data);
}

NetworkReply NetworkAccess::sendRequest(const QUrl &p_url,
                                        const RawHeaderPairs &p_rawHeader,
                                        const QByteArray &p_action,
                                        const QByteArray &p_data)
{
    NetworkReply reply;
    if (!p_url.isValid()) {
        return reply;
    }

    bool finished = false;
    QNetworkAccessManager netAccessMgr;
    connect(&netAccessMgr, &QNetworkAccessManager::finished,
            [&reply, &finished](QNetworkReply *p_reply) {
                NetworkAccess::handleReply(p_reply, reply);
                finished = true;
            });

    auto nq(NetworkUtils::networkRequest(p_url));
    for (const auto &header : p_rawHeader) {
        nq.setRawHeader(header.first, header.second);
    }

    netAccessMgr.sendCustomRequest(nq, p_action, p_data);

    while (!finished) {
        Utils::sleepWait(100);
    }

    return reply;
}

void NetworkAccess::handleReply(QNetworkReply *p_reply, NetworkReply &p_myReply)
{
    p_myReply.m_error = p_reply->error();
    p_myReply.m_data = p_reply->readAll();

    if (p_myReply.m_error != QNetworkReply::NoError) {
        qWarning() << "request reply error" << p_myReply.m_error << p_reply->request().url();
    }

    p_reply->deleteLater();
}
