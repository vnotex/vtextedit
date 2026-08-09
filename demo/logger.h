#ifndef LOGGER_H
#define LOGGER_H

#include <QString>
#include <QDebug>
#include <QFileInfo>

class Logger
{
public:
    Logger() = delete;

    static void log(QtMsgType p_type, const QMessageLogContext &p_context, const QString &p_msg)
    {
        QByteArray localMsg = p_msg.toUtf8();
        QString header;
        switch (p_type) {
        case QtDebugMsg:
            header = "Debug:";
            break;

        case QtInfoMsg:
            header = "Info:";
            break;

        case QtWarningMsg:
            header = "Warning:";
            break;

        case QtCriticalMsg:
            header = "Critical:";
            break;

        case QtFatalMsg:
            header = "Fatal:";

        default:
            break;
        }

        QString srcFile = QFileInfo(p_context.file).fileName();
        std::string fileStr = srcFile.toStdString();
        const char *file = fileStr.c_str();
        switch (p_type) {
        case QtDebugMsg:
        case QtInfoMsg:
        case QtWarningMsg:
        case QtCriticalMsg:
            fprintf(stderr, "%s(%s:%u) %s\n",
                    header.toStdString().c_str(), file, p_context.line, localMsg.constData());
            break;
        case QtFatalMsg:
            fprintf(stderr, "%s(%s:%u) %s\n",
                    header.toStdString().c_str(), file, p_context.line, localMsg.constData());
            abort();
        }

        fflush(stderr);
    }
};

#endif
