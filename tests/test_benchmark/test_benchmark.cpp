#include "test_benchmark.h"

#include <cmarkadapter.h>
#include <highlightelement.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QDateTime>
#include <QTextStream>

#include <algorithm>

using namespace tests;

static const char *styleNames[NUM_HIGHLIGHT_STYLES] = {
    "LINK",            // 0
    "AUTO_LINK_URL",   // 1
    "AUTO_LINK_EMAIL", // 2
    "IMAGE",           // 3
    "CODE",            // 4
    "HTML",            // 5
    "HTML_ENTITY",     // 6
    "EMPH",            // 7
    "STRONG",          // 8
    "LIST_BULLET",     // 9
    "LIST_ENUMERATOR", // 10
    "COMMENT",         // 11
    "H1",              // 12
    "H2",              // 13
    "H3",              // 14
    "H4",              // 15
    "H5",              // 16
    "H6",              // 17
    "BLOCKQUOTE",      // 18
    "VERBATIM",        // 19
    "HTMLBLOCK",       // 20
    "HRULE",           // 21
    "REFERENCE",       // 22
    "FENCEDCODEBLOCK", // 23
    "NOTE",            // 24
    "STRIKE",          // 25
    "FRONTMATTER",     // 26
    "DISPLAYFORMULA",  // 27
    "INLINEEQUATION",  // 28
    "MARK",            // 29
    "TABLE",           // 30
    "TABLEHEADER",     // 31
    "TABLEBORDER",     // 32
};

static QString readFixture(const QString &p_name)
{
    QFile f(QStringLiteral(FIXTURES_DIR) + "/" + p_name);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    return QString::fromUtf8(f.readAll());
}

void TestBenchmark::initTestCase()
{
}

void TestBenchmark::benchmarkParse()
{
    // Read all 8 fixture files.
    QStringList fixtureNames = {
        "block_elements.md",
        "inline_elements.md",
        "multiline_elements.md",
        "nested_elements.md",
        "edge_cases.md",
        "extension_elements.md",
        "table_elements.md",
        "math_elements.md"
    };

    QString combined;
    for (const auto &name : fixtureNames) {
        QString content = readFixture(name);
        QVERIFY2(!content.isEmpty(),
                 qPrintable(QString("Failed to read fixture: %1").arg(name)));
        combined += content + "\n";
    }

    // Pad to ~1000 lines by repeating.
    int lineCount = combined.count('\n');
    QString doc;
    doc.reserve(combined.size() * (1000 / lineCount + 1));
    while (doc.count('\n') < 1000) {
        doc += combined;
    }

    QByteArray utf8 = doc.toUtf8();
    int docLines = doc.count('\n');
    int docBytes = utf8.size();

    // Count elements in one parse.
    int bucketCounts[NUM_HIGHLIGHT_STYLES] = {};
    int totalElements = 0;
    {
        HighlightElement **result = parseCmark(utf8);
        QVERIFY(result != nullptr);
        for (int i = 0; i < NUM_HIGHLIGHT_STYLES; i++) {
            HighlightElement *e = result[i];
            while (e) {
                bucketCounts[i]++;
                totalElements++;
                e = e->next;
            }
        }
        freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
    }

    // Run 100 iterations, measure times.
    const int iterations = 100;
    QVector<qint64> times;
    times.reserve(iterations);
    for (int iter = 0; iter < iterations; iter++) {
        QElapsedTimer timer;
        timer.start();
        HighlightElement **result = parseCmark(utf8);
        qint64 elapsed = timer.elapsed();
        QVERIFY(result != nullptr);
        freeHighlightElements(result, NUM_HIGHLIGHT_STYLES);
        times.append(elapsed);
    }

    std::sort(times.begin(), times.end());
    qint64 minTime = times.first();
    qint64 maxTime = times.last();
    qint64 sum = 0;
    for (auto t : times) {
        sum += t;
    }
    double avgTime = static_cast<double>(sum) / iterations;

    qDebug() << "Average parse time:" << avgTime << "ms"
             << "min:" << minTime << "max:" << maxTime
             << "total elements:" << totalElements;

    // Write evidence file.
    QDir evidenceDir(QStringLiteral(EVIDENCE_DIR));
    if (!evidenceDir.exists()) {
        evidenceDir.mkpath(".");
    }

    QFile out(evidenceDir.filePath("task-1-benchmark-before.txt"));
    QVERIFY2(out.open(QIODevice::WriteOnly | QIODevice::Text),
             qPrintable(QString("Cannot write evidence: %1").arg(out.fileName())));

    QTextStream ts(&out);
    ts << "Benchmark: Before Pipeline Rethink\n";
    ts << "Date: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
    ts << "Document: " << docLines << " lines, " << docBytes << " bytes\n";
    ts << "Iterations: " << iterations << "\n";
    ts << QString("Average Parse Time: %1 ms\n").arg(avgTime, 0, 'f', 2);
    ts << "Min Parse Time: " << minTime << " ms\n";
    ts << "Max Parse Time: " << maxTime << " ms\n";
    ts << "Total Elements (33 buckets): " << totalElements << "\n";
    ts << "Per-bucket counts:\n";
    for (int i = 0; i < NUM_HIGHLIGHT_STYLES; i++) {
        ts << QString("  [%1] %2: %3\n").arg(i).arg(styleNames[i]).arg(bucketCounts[i]);
    }

    out.close();
    qDebug() << "Evidence written to:" << out.fileName();
}

QTEST_MAIN(tests::TestBenchmark)
