#ifndef HELPER_H
#define HELPER_H

#include <QString>
#include <QFile>

class Helper
{
public:
    static QString getCppText()
    {
        return readFile(QStringLiteral(":/demo/data/example_files/example.cpp"));
    }

    static QString getMarkdownText()
    {
        return readFile(QStringLiteral(":/demo/data/example_files/example.md"));
    }

    static QString getText()
    {
        return readFile(QStringLiteral(":/demo/data/example_files/example.txt"));
    }

private:
    static QString readFile(const QString &p_filePath)
    {
        QFile file(p_filePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QString();
        }

        auto bytes = file.readAll();
        return QString::fromUtf8(bytes);
    }
};

#endif
