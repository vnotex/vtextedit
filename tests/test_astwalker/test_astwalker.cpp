#include "test_astwalker.h"

#include <QDir>
#include <QFile>

#include "markdownastwalker.h"
#include "markdownsyntaxstyles.h"

using namespace tests;

static const QStringList s_fixtureNames = {
    QStringLiteral("block_elements.md"),     QStringLiteral("inline_elements.md"),
    QStringLiteral("multiline_elements.md"), QStringLiteral("nested_elements.md"),
    QStringLiteral("edge_cases.md"),         QStringLiteral("extension_elements.md"),
    QStringLiteral("table_elements.md"),     QStringLiteral("math_elements.md")};

static QString readFile(const QString &p_path) {
  QFile f(p_path);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return QString();
  }
  return QString::fromUtf8(f.readAll());
}

static QString
serializeBlocksHighlights(const QVector<QVector<vte::md::HLUnit>> &p_blocksHighlights) {
  QStringList lines;
  for (int blockNum = 0; blockNum < p_blocksHighlights.size(); ++blockNum) {
    for (const auto &unit : p_blocksHighlights[blockNum]) {
      lines.append(QStringLiteral("%1:%2:%3:%4")
                       .arg(blockNum)
                       .arg(unit.start)
                       .arg(unit.length)
                       .arg(unit.styleIndex));
    }
  }
  return lines.join('\n') + '\n';
}

void TestASTWalker::verifyBlocksHighlights() {
  QDir goldenDir(QStringLiteral(GOLDEN_DIR));

  for (const auto &name : s_fixtureNames) {
    QString text = readFile(QStringLiteral(FIXTURES_DIR) + "/" + name);
    if (text.isEmpty()) {
      QSKIP(qPrintable(QStringLiteral("Fixture not found: ") + name));
    }

    QByteArray utf8 = text.toUtf8();
    // Count blocks: number of lines in text. Each newline-separated segment is a block.
    int numBlocks = 1;
    for (int i = 0; i < utf8.size(); ++i) {
      if (utf8[i] == '\n') {
        ++numBlocks;
      }
    }

    vte::md::ASTWalkResult result = vte::md::walkAndConvert(utf8, numBlocks);

    QString actual = serializeBlocksHighlights(result.blocksHighlights);

    QString baseName = name;
    baseName.replace(QStringLiteral(".md"), QString());
    QString goldenPath = goldenDir.filePath(baseName + ".blocks.golden");

    QString expected = readFile(goldenPath);
    if (expected.isEmpty()) {
      QSKIP(qPrintable(QStringLiteral("Golden file not found: ") + goldenPath));
    }

    QCOMPARE(actual, expected);
  }
}

void TestASTWalker::verifyRegions() {
  // Test that walkAndConvert produces non-empty regions for relevant fixtures.
  {
    QString text = readFile(QStringLiteral(FIXTURES_DIR) + "/block_elements.md");
    QVERIFY(!text.isEmpty());
    QByteArray utf8 = text.toUtf8();
    int numBlocks = 1;
    for (int i = 0; i < utf8.size(); ++i) {
      if (utf8[i] == '\n')
        ++numBlocks;
    }
    vte::md::ASTWalkResult result = vte::md::walkAndConvert(utf8, numBlocks);
    QVERIFY(!result.headerRegions.isEmpty());
    QVERIFY(!result.codeBlockRegions.isEmpty());
    QVERIFY(!result.hruleRegions.isEmpty());
  }

  {
    QString text = readFile(QStringLiteral(FIXTURES_DIR) + "/table_elements.md");
    QVERIFY(!text.isEmpty());
    QByteArray utf8 = text.toUtf8();
    int numBlocks = 1;
    for (int i = 0; i < utf8.size(); ++i) {
      if (utf8[i] == '\n')
        ++numBlocks;
    }
    vte::md::ASTWalkResult result = vte::md::walkAndConvert(utf8, numBlocks);
    QVERIFY(!result.tableRegions.isEmpty());
    QVERIFY(!result.tableHeaderRegions.isEmpty());
  }

  {
    QString text = readFile(QStringLiteral(FIXTURES_DIR) + "/math_elements.md");
    QVERIFY(!text.isEmpty());
    QByteArray utf8 = text.toUtf8();
    int numBlocks = 1;
    for (int i = 0; i < utf8.size(); ++i) {
      if (utf8[i] == '\n')
        ++numBlocks;
    }
    vte::md::ASTWalkResult result = vte::md::walkAndConvert(utf8, numBlocks);
    QVERIFY(!result.inlineEquationRegions.isEmpty());
    QVERIFY(!result.displayFormulaRegions.isEmpty());
  }

  // Test fast mode skips regions.
  {
    QString text = readFile(QStringLiteral(FIXTURES_DIR) + "/block_elements.md");
    QVERIFY(!text.isEmpty());
    QByteArray utf8 = text.toUtf8();
    int numBlocks = 1;
    for (int i = 0; i < utf8.size(); ++i) {
      if (utf8[i] == '\n')
        ++numBlocks;
    }
    vte::md::ASTWalkResult result = vte::md::walkAndConvert(utf8, numBlocks, 0, 0, true);
    QVERIFY(result.headerRegions.isEmpty());
    QVERIFY(result.codeBlockRegions.isEmpty());
    // But blocksHighlights should still be populated.
    bool hasAny = false;
    for (const auto &block : result.blocksHighlights) {
      if (!block.isEmpty()) {
        hasAny = true;
        break;
      }
    }
    QVERIFY(hasAny);
  }
}

void TestASTWalker::testFoldingRegionsHeadings() {
  // H1 on line 1, H2 on line 3, H3 on line 5 (1-based cmark lines).
  QByteArray md = "# Heading 1\n"
                  "\n"
                  "## Heading 2\n"
                  "\n"
                  "### Heading 3\n";
  int numBlocks = 5;
  auto result = vte::md::walkAndConvert(md, numBlocks);
  // Should have 3 folding regions for headings.
  int headingCount = 0;
  for (const auto &r : result.foldingRegions) {
    if (r.m_type == vte::md::Heading) {
      ++headingCount;
    }
  }
  QCOMPARE(headingCount, 3);

  // Check each heading's block and level.
  // Regions are sorted by startBlock.
  QVERIFY(result.foldingRegions.size() >= 3);
  const auto &h1 = result.foldingRegions[0];
  QCOMPARE(h1.m_type, vte::md::Heading);
  QCOMPARE(h1.m_startBlock, 0); // line 1 -> block 0
  QCOMPARE(h1.m_endBlock, 0);
  QCOMPARE(h1.m_level, 1);

  const auto &h2 = result.foldingRegions[1];
  QCOMPARE(h2.m_type, vte::md::Heading);
  QCOMPARE(h2.m_startBlock, 2); // line 3 -> block 2
  QCOMPARE(h2.m_endBlock, 2);
  QCOMPARE(h2.m_level, 2);

  const auto &h3 = result.foldingRegions[2];
  QCOMPARE(h3.m_type, vte::md::Heading);
  QCOMPARE(h3.m_startBlock, 4); // line 5 -> block 4
  QCOMPARE(h3.m_endBlock, 4);
  QCOMPARE(h3.m_level, 3);
}

void TestASTWalker::testFoldingRegionsCodeBlock() {
  // Fenced code block spanning lines 1-3.
  QByteArray md = "```\n"
                  "code\n"
                  "```\n";
  int numBlocks = 3;
  auto result = vte::md::walkAndConvert(md, numBlocks);

  bool found = false;
  for (const auto &r : result.foldingRegions) {
    if (r.m_type == vte::md::FencedCode) {
      QCOMPARE(r.m_startBlock, 0);
      QCOMPARE(r.m_endBlock, 2);
      QCOMPARE(r.m_level, 0);
      found = true;
      break;
    }
  }
  QVERIFY(found);
}

void TestASTWalker::testFoldingRegionsBlockquote() {
  QByteArray md = "> line1\n"
                  "> line2\n"
                  "> line3\n";
  int numBlocks = 3;
  auto result = vte::md::walkAndConvert(md, numBlocks);

  bool found = false;
  for (const auto &r : result.foldingRegions) {
    if (r.m_type == vte::md::Blockquote) {
      QCOMPARE(r.m_startBlock, 0);
      QCOMPARE(r.m_endBlock, 2);
      QCOMPARE(r.m_level, 0);
      found = true;
      break;
    }
  }
  QVERIFY(found);
}

void TestASTWalker::testFoldingRegionsTable() {
  QByteArray md = "| A | B |\n"
                  "| - | - |\n"
                  "| 1 | 2 |\n";
  int numBlocks = 3;
  auto result = vte::md::walkAndConvert(md, numBlocks);

  bool found = false;
  for (const auto &r : result.foldingRegions) {
    if (r.m_type == vte::md::Table) {
      QCOMPARE(r.m_startBlock, 0);
      QCOMPARE(r.m_endBlock, 2);
      QCOMPARE(r.m_level, 0);
      found = true;
      break;
    }
  }
  QVERIFY(found);
}

void TestASTWalker::testFoldingRegionsMathBlock() {
  QByteArray md = "$$\n"
                  "E=mc^2\n"
                  "$$\n";
  int numBlocks = 3;
  auto result = vte::md::walkAndConvert(md, numBlocks);

  bool found = false;
  for (const auto &r : result.foldingRegions) {
    if (r.m_type == vte::md::Math) {
      QCOMPARE(r.m_startBlock, 0);
      QCOMPARE(r.m_endBlock, 2);
      QCOMPARE(r.m_level, 0);
      found = true;
      break;
    }
  }
  QVERIFY(found);
}

void TestASTWalker::testFoldingRegionsFrontMatter() {
  QByteArray md = "---\n"
                  "title: Test\n"
                  "---\n"
                  "content\n";
  int numBlocks = 4;
  auto result = vte::md::walkAndConvert(md, numBlocks);

  bool found = false;
  for (const auto &r : result.foldingRegions) {
    if (r.m_type == vte::md::FrontMatter) {
      QCOMPARE(r.m_startBlock, 0);
      QCOMPARE(r.m_endBlock, 2);
      QCOMPARE(r.m_level, 0);
      found = true;
      break;
    }
  }
  QVERIFY(found);
}

void TestASTWalker::testFoldingRegionsMixed() {
  // Document with heading, code block, and blockquote.
  QByteArray md = "# Title\n"   // line 1 -> block 0
                  "\n"          // line 2 -> block 1
                  "```\n"       // line 3 -> block 2
                  "code\n"      // line 4 -> block 3
                  "```\n"       // line 5 -> block 4
                  "\n"          // line 6 -> block 5
                  "> quote1\n"  // line 7 -> block 6
                  "> quote2\n"; // line 8 -> block 7
  int numBlocks = 8;
  auto result = vte::md::walkAndConvert(md, numBlocks);

  // Should have at least 3 folding regions.
  QVERIFY(result.foldingRegions.size() >= 3);

  // Verify sorted by startBlock.
  for (int i = 1; i < result.foldingRegions.size(); ++i) {
    QVERIFY(result.foldingRegions[i].m_startBlock >= result.foldingRegions[i - 1].m_startBlock);
  }

  // Check types present.
  bool hasHeading = false, hasCode = false, hasBlockquote = false;
  for (const auto &r : result.foldingRegions) {
    if (r.m_type == vte::md::Heading && r.m_startBlock == 0) {
      hasHeading = true;
    }
    if (r.m_type == vte::md::FencedCode && r.m_startBlock == 2) {
      hasCode = true;
    }
    if (r.m_type == vte::md::Blockquote && r.m_startBlock == 6) {
      hasBlockquote = true;
    }
  }
  QVERIFY(hasHeading);
  QVERIFY(hasCode);
  QVERIFY(hasBlockquote);
}

// Regression: a styled span whose LAST character is a 4-byte UTF-8 char (an emoji,
// i.e. a UTF-16 surrogate pair) must not have its end position land in the MIDDLE of
// that surrogate pair. cmark reports end_column at the last BYTE of the last char;
// converting it with toDocPosition(el, ec) + 1 under-counted by 1 QChar for astral
// chars, slicing the emoji and rendering it as two "tofu" boxes in the editor.
void TestASTWalker::testHLUnitEndingInEmoji() {
  const unsigned int STYLE_H1 = 12;
  const unsigned int STYLE_H2 = 13;

  // Helper: return the first HLUnit with the given style across all blocks.
  auto findUnit = [](const vte::md::ASTWalkResult &r,
                     unsigned int style) -> QPair<bool, vte::md::HLUnit> {
    for (const auto &block : r.blocksHighlights) {
      for (const auto &unit : block) {
        if (unit.styleIndex == style) {
          return qMakePair(true, unit);
        }
      }
    }
    return qMakePair(false, vte::md::HLUnit());
  };

  // Case 1 — the exact bug report: "## 教学原则 💎💎".
  // QChars: "## "(3) + 教学原则(4) + " "(1) + 💎(2) + 💎(2) = 12.
  // Each 💎 (U+1F48E) is F0 9F 92 8E; 教学原则 are 3-byte CJK.
  {
    QByteArray md("## \xe6\x95\x99\xe5\xad\xa6\xe5\x8e\x9f\xe5\x88\x99"
                  " \xf0\x9f\x92\x8e\xf0\x9f\x92\x8e\n");
    auto result = vte::md::walkAndConvert(md, 2);
    auto found = findUnit(result, STYLE_H2);
    QVERIFY2(found.first, "H2 HLUnit not found");
    QCOMPARE((int)found.second.start, 0);
    // Must be 12 (whole heading). The bug produced 11, ending between the two
    // surrogates of the trailing 💎.
    QCOMPARE((int)found.second.length, 12);
  }

  // Case 2 — a single trailing emoji also splits: "# 💎".
  // QChars: "# "(2) + 💎(2) = 4.
  {
    QByteArray md("# \xf0\x9f\x92\x8e\n");
    auto result = vte::md::walkAndConvert(md, 2);
    auto found = findUnit(result, STYLE_H1);
    QVERIFY2(found.first, "H1 HLUnit not found");
    QCOMPARE((int)found.second.start, 0);
    QCOMPARE((int)found.second.length, 4);
  }
}

// A multiline inline construct must span exactly its source text -- neither
// stretched past its closing marker nor cut short of it. Three independent
// defects did both:
//
// 1. cmarkSpanFromCoords() clamped the corrected columns with
//    `ec = qMax(sc, ec)`, which is only meaningful while both ends sit on the
//    SAME line. An inline span that opens near the end of one line and closes
//    near the start of the next has end_column < start_column by construction,
//    so the clamp replaced the real end column with the start column of the
//    FIRST line and the highlight ran that many columns into the last line --
//    the "messy highlight" of the first bug report.
//
// 2. cmark's adjust_subj_node_newlines() reported the end column of a
//    multiline code span from a scan that EXCLUDES the closing delimiter, and
//    without block_offset, so the span ended `1 + prefix width` characters
//    early -- losing the closing backtick (case 4).
//
// 3. The continuation-line correction was gated on `blockOffset > 0`, so an
//    INDENTED continuation line of a top-level paragraph -- whose leading
//    whitespace the block parser skips just the same -- kept cmark's short
//    columns (the last entry of case 5). Its prefix accounting also credited a
//    LAZY continuation for indentation cmark never stripped, and ignored tabs
//    (case 6).
void TestASTWalker::testMultilineInlineInContainer() {
  const unsigned int STYLE_CODE = 4;
  const unsigned int STYLE_STRONG = 8;

  // Text of every unit of the given style, in block order. A first-line unit of
  // a multiline span may include the line terminator, which mid() clamps away.
  auto styleTexts = [](const QByteArray &p_md, unsigned int p_style) {
    const QStringList lines = QString::fromUtf8(p_md).split(QLatin1Char('\n'));
    auto result = vte::md::walkAndConvert(p_md, lines.size());
    QStringList texts;
    for (int b = 0; b < result.blocksHighlights.size(); ++b) {
      for (const auto &unit : result.blocksHighlights.at(b)) {
        if (unit.styleIndex != p_style) {
          continue;
        }
        texts.append(
            lines.value(b).mid(static_cast<int>(unit.start), static_cast<int>(unit.length)));
      }
    }
    return texts;
  };

  // Case 1 -- the bug report (its wording scrambled, its shape kept byte for
  // byte): an ordered list item whose paragraph wraps over four lines, with two
  // strong spans crossing a line break. The multibyte characters are incidental
  // (they only made the drift obvious on screen).
  {
    QByteArray md =
        "5. **2026-06-06 (`qmrtub`) \xE2\x80\x94 xolen \"kunel warid\" raludeg.** Rulmatvezop "
        "**Kevbowa/QRT \xE2\x86\x92 Nizarkelp\n"
        "   Zwarn/QRTOMN** quhk o zunqbadedy lomapt: uzk 3 ev wro pofe\xE2\x80\x99s raknuvo "
        "zetnaqilo qeul **vupa\n"
        "   kuzmoli pravonuqe** qom wro pofe (`PofeQD:...` qelvano kunel, ozam ur kunel zwolp "
        "qelvano kunel).\n"
        "   Uxfel QRTOMN ze qe-zwolpan warid nizarkelpud.\n";

    const QStringList expected{
        QString::fromUtf8("**2026-06-06 (`qmrtub`) \xE2\x80\x94 xolen \"kunel warid\" raludeg.**"),
        QString::fromUtf8("**Kevbowa/QRT \xE2\x86\x92 Nizarkelp"),
        QStringLiteral("   Zwarn/QRTOMN**"),
        QStringLiteral("**vupa"),
        QStringLiteral("   kuzmoli pravonuqe**"),
    };
    QCOMPARE(styleTexts(md, STYLE_STRONG), expected);
  }

  // Case 2 -- the same shape in a block quote, with no multibyte character at
  // all: the drift is caused by the container, not by the encoding.
  {
    QByteArray md = "> alpha beta gamma **delta\n"
                    "> epsilon** zeta\n";
    const QStringList expected{QStringLiteral("**delta"), QStringLiteral("> epsilon**")};
    QCOMPARE(styleTexts(md, STYLE_STRONG), expected);
  }

  // Case 3 -- control: a top-level paragraph with an unindented continuation
  // line, where no column correction applies at all.
  {
    QByteArray md = "alpha beta gamma **delta\n"
                    "epsilon** zeta\n";
    const QStringList expected{QStringLiteral("**delta"), QStringLiteral("epsilon**")};
    QCOMPARE(styleTexts(md, STYLE_STRONG), expected);
  }

  // Case 4 -- the second bug report (wording scrambled, shape preserved): an
  // inline CODE span crossing a line break stopped short of its closing
  // backtick.
  //
  // The end column of a multiline code span (and of a multiline inline HTML
  // tag) is produced by cmark's adjust_subj_node_newlines(), which measured it
  // from a scan that deliberately EXCLUDES the closing delimiter, and then
  // reported that raw count. It has to add the delimiter's width and the
  // container's block_offset -- every other column this parser reports includes
  // block_offset -- or the highlight ends `1 + prefix width` characters early,
  // dropping the closing backtick and the last characters of the content.
  {
    QByteArray md = "- **Nizarkelpud zunqbadedy**: zwolp wro QRTOMN pelvu qomavelt qom `WaridoQl\n"
                    "  41ce07f2-b918-4a63-85d7-2ff60c1ba934` ze pravonuqe zwolpan wro pofe "
                    "qelvan ur qom kuzmoli zew\n"
                    "  uxf'e zwolpa-qelvanilo, pofelu kunel zunqbadedy raknuv ozam vupa.\n";
    const QStringList expected{
        QStringLiteral("`WaridoQl"),
        QStringLiteral("  41ce07f2-b918-4a63-85d7-2ff60c1ba934`"),
    };
    QCOMPARE(styleTexts(md, STYLE_CODE), expected);
  }

  // Case 5 -- the same code span across the container shapes whose stripped
  // prefix widths differ: bullet, over-indented bullet, block quote, a lazy
  // continuation (no prefix at all) and top level (block_offset == 0).
  {
    const QVector<QPair<QByteArray, QStringList>> cases{
        {QByteArray("`aaaa\nbbbb` x\n"), {QStringLiteral("`aaaa"), QStringLiteral("bbbb`")}},
        {QByteArray("- `aaaa\n  bbbb` x\n"), {QStringLiteral("`aaaa"), QStringLiteral("  bbbb`")}},
        {QByteArray("-   `aaaa\n    bbbb` x\n"),
         {QStringLiteral("`aaaa"), QStringLiteral("    bbbb`")}},
        {QByteArray("> `aaaa\n> bbbb` x\n"), {QStringLiteral("`aaaa"), QStringLiteral("> bbbb`")}},
        {QByteArray("- `aaaa\nbbbb` x\n"), {QStringLiteral("`aaaa"), QStringLiteral("bbbb`")}},
        // Top level, indented continuation: no container prefix is stripped,
        // but the block parser still skips the leading whitespace before inline
        // parsing, so the reported column is short by the indent.
        {QByteArray("`aaaa\n   bbbb` x\n"), {QStringLiteral("`aaaa"), QStringLiteral("   bbbb`")}},
        // Multi-character delimiter: the whole closing run belongs to the span.
        {QByteArray("- ``aaaa\n  bbbb`` x\n"),
         {QStringLiteral("``aaaa"), QStringLiteral("  bbbb``")}},
    };

    for (const auto &c : cases) {
      QVERIFY2(styleTexts(c.first, STYLE_CODE) == c.second, c.first.constData());
    }
  }

  // Case 6 -- PARTIALLY indented lazy continuations, and a tab-indented one.
  //
  // A lazy continuation line does not carry its container's prefix, so cmark
  // appends it raw and the reported column needs no prefix credit at all;
  // crediting its partial indentation stretched the span one column per space.
  // A tab is leading whitespace like any other and counts as the one byte it
  // is, since these are byte columns.
  {
    QCOMPARE(styleTexts(QByteArray("- `aa\n bb` x\n"), STYLE_CODE),
             QStringList({QStringLiteral("`aa"), QStringLiteral(" bb`")}));
    QCOMPARE(styleTexts(QByteArray("> **aa\n bb** x\n"), STYLE_STRONG),
             QStringList({QStringLiteral("**aa"), QStringLiteral(" bb**")}));
    QCOMPARE(styleTexts(QByteArray("**aa\n\tbb** x\n"), STYLE_STRONG),
             QStringList({QStringLiteral("**aa"), QStringLiteral("\tbb**")}));
    qDebug() << "probeD" << styleTexts(QByteArray("> - `aa\n>  bb` x\n"), STYLE_CODE);
    qDebug() << "probeE" << styleTexts(QByteArray("- `aa\n\tbb` x\n"), STYLE_CODE);
    qDebug() << "probeF" << styleTexts(QByteArray("> > `aa\n>\tbb` x\n"), STYLE_CODE);
  }

  // Case 7 -- CHARACTERIZATION, not a specification. lineStrippedPrefixWidth()
  // approximates cmark's per-container prefix walk (see its header comment), so
  // these three shapes are still off by a byte or two. They are pinned here so
  // the gap is visible and cannot widen unnoticed; each expectation below is
  // byte-identical to what the code produced BEFORE this change, i.e. none of
  // them is a regression -- fix them by teaching cmark to report the prefix it
  // removed per line, then tighten these to the true source slices (commented).
  {
    // True: ">  bb`" -- whitespace after a matched '>' is credited in full,
    // though the block quote consumes at most one column of it.
    QCOMPARE(styleTexts(QByteArray("> - `aa\n>  bb` x\n"), STYLE_CODE),
             QStringList({QStringLiteral("`aa"), QStringLiteral(">  bb` ")}));
    // True: "\tbb`" -- a tab-indented list continuation. The tab is one byte
    // here but a tab stop to cmark, and may be only partly consumed.
    QCOMPARE(styleTexts(QByteArray("- `aa\n\tbb` x\n"), STYLE_CODE),
             QStringList({QStringLiteral("`aa"), QStringLiteral("\tbb")}));
    // True: ">\tbb`" -- the same, after a block-quote marker.
    QCOMPARE(styleTexts(QByteArray("> > `aa\n>\tbb` x\n"), STYLE_CODE),
             QStringList({QStringLiteral("`aa"), QStringLiteral(">\tbb` ")}));
  }
}

// Checks the walker's image classification against the rule PreviewMgr applies
// to the painted preview path: what precedes the element on its first line and
// what follows it on its last line must both be blank.
//
// CAVEAT, so nobody over-trusts this: the reference rule below is a THIRD
// implementation written here, not a call into
// PreviewMgr::buildImageLinksForLayout(). This test needs no QTextDocument, and
// PreviewMgr's version works on document blocks with TextUtils::isSpace() where
// this one works on raw source lines with QString::trimmed(). It therefore
// pins the walker's rule; it cannot detect PreviewMgr drifting away from it.
// The corpus is also all top-level unindented paragraphs.
//
// The multiline cases are the interesting ones. They used to be satisfied by
// exclusion -- cmark collapsed a multiline construct onto its last line, so
// isStandaloneOnLine()'s `startLine == endLine` guard rejected them outright.
// cmark now reports the true span and the walker applies the real whole-span
// rule, so these assert the rule rather than the workaround.
void TestASTWalker::testImageStandaloneMatchesPaintedPath() {
  const QVector<QByteArray> cases{
      // Alone on one line.
      QByteArray("![alt](img.png)\n"),
      // The alt text wraps onto a second line.
      QByteArray("![alt\ntext](img.png)\n"),
      QByteArray("![alt\ntext](img.png)\ntrailing\n"),
      // Something else shares the element's line.
      QByteArray("lead ![alt\ntext](img.png)\n"),
      QByteArray("![a](\nimg.png)\n"),
      QByteArray("text ![alt](img.png) more\n"),
  };

  for (const auto &markdown : cases) {
    const int blocks = markdown.count('\n') + 1;
    auto result = vte::md::walkAndConvert(markdown, blocks);
    QCOMPARE(result.imageElements.size(), 1);

    const auto &image = result.imageElements.first();
    const QString text = QString::fromUtf8(markdown);
    QVERIFY(image.m_startPos >= 0 && image.m_endPos <= text.size());
    QVERIFY(image.m_startPos < image.m_endPos);

    // Independently evaluate the painted-path rule over the element's whole
    // span: leading text on its first line, trailing text on its last line.
    const int lineStart =
        image.m_startPos == 0 ? 0 : text.lastIndexOf(QLatin1Char('\n'), image.m_startPos - 1) + 1;
    int lineEnd = text.indexOf(QLatin1Char('\n'), image.m_endPos - 1);
    if (lineEnd < 0) {
      lineEnd = text.size();
    }

    const bool paintedStandalone =
        text.mid(lineStart, image.m_startPos - lineStart).trimmed().isEmpty() &&
        text.mid(image.m_endPos, lineEnd - image.m_endPos).trimmed().isEmpty();

    QVERIFY2(image.m_standalone == paintedStandalone, markdown.constData());
  }
}

void TestASTWalker::testTableCellOffsets() {
  //             0123456789...
  QByteArray md = "|  a |\\| b\t|\n"
                  "| - | - |\n"
                  "| \xC3\xA9\xF0\x9F\x98\x80 | |\n";
  auto result = vte::md::walkAndConvert(md, 3);
  QCOMPARE(result.tableElements.size(), 1);

  const auto &table = result.tableElements.first();
  QCOMPARE(table.m_startBlock, 0);
  QCOMPARE(table.m_rows.size(), 3);

  const QString line0 = QString::fromUtf8("|  a |\\| b\t|");
  const auto &header = table.m_rows.at(0);
  QCOMPARE(header.m_cells.size(), 2);
  QCOMPARE(header.m_cellOffsets.size(), 2);
  QCOMPARE(header.m_cells.at(0), QStringLiteral("a"));
  QCOMPARE(line0.mid(header.m_cellOffsets.at(0), header.m_cells.at(0).size()),
           header.m_cells.at(0));
  QCOMPARE(header.m_cells.at(1), QStringLiteral("\\| b"));
  QCOMPARE(line0.mid(header.m_cellOffsets.at(1), header.m_cells.at(1).size()),
           header.m_cells.at(1));

  // Surrogate-safe QChar offsets.
  const QString line2 = QString::fromUtf8("| \xC3\xA9\xF0\x9F\x98\x80 | |");
  const auto &body = table.m_rows.at(2);
  QCOMPARE(body.m_cells.size(), 2);
  QCOMPARE(line2.mid(body.m_cellOffsets.at(0), body.m_cells.at(0).size()), body.m_cells.at(0));
  QVERIFY(body.m_cells.at(1).isEmpty());
}

void TestASTWalker::testTableCellHighlights() {
  QByteArray md = "| **a** | x |\n"
                  "| - | - |\n"
                  "| t `b` | |\n";
  auto result = vte::md::walkAndConvert(md, 3);
  QCOMPARE(result.tableElements.size(), 1);

  const auto &table = result.tableElements.first();
  QCOMPARE(table.m_rows.size(), 3);

  const auto &header = table.m_rows.at(0);
  QCOMPARE(header.m_cellHighlights.size(), 2);
  QCOMPARE(header.m_cellHighlights.at(0).size(), 1);
  // Cell-local, covering exactly "**a**".
  QCOMPARE(static_cast<int>(header.m_cellHighlights.at(0).first().start), 0);
  QCOMPARE(static_cast<int>(header.m_cellHighlights.at(0).first().length), 5);
  QVERIFY(header.m_cellHighlights.at(1).isEmpty());

  const auto &body = table.m_rows.at(2);
  QCOMPARE(body.m_cellHighlights.size(), 2);
  QCOMPARE(body.m_cellHighlights.at(0).size(), 1);
  // "t `b`" - the code span starts at cell-local offset 2.
  QCOMPARE(static_cast<int>(body.m_cellHighlights.at(0).first().start), 2);
  QCOMPARE(static_cast<int>(body.m_cellHighlights.at(0).first().length), 3);

  // No row-wide table style leaks into a cell.
  for (const auto &row : table.m_rows) {
    for (int c = 0; c < row.m_cellHighlights.size(); ++c) {
      for (const auto &unit : row.m_cellHighlights.at(c)) {
        const int style = static_cast<int>(unit.styleIndex);
        QVERIFY(style != STYLE_TABLE);
        QVERIFY(style != STYLE_TABLEHEADER);
        QVERIFY(style != STYLE_BLOCKQUOTE);
        QVERIFY(unit.length > 0);
        QVERIFY(static_cast<int>(unit.start + unit.length) <= row.m_cells.at(c).size());
      }
    }
  }
}

void TestASTWalker::testTableCellHighlightsInBlockquote() {
  QByteArray md = "> | **a** | b |\n"
                  "> | - | - |\n"
                  "> | c | |\n";
  auto result = vte::md::walkAndConvert(md, 3);
  QCOMPARE(result.tableElements.size(), 1);

  const auto &table = result.tableElements.first();
  QCOMPARE(table.m_startBlock, 0);

  const QString line0 = QStringLiteral("> | **a** | b |");
  const auto &header = table.m_rows.at(0);
  QCOMPARE(header.m_prefix, QStringLiteral("> "));
  QCOMPARE(line0.mid(header.m_cellOffsets.at(0), header.m_cells.at(0).size()),
           header.m_cells.at(0));
  QCOMPARE(header.m_cellHighlights.at(0).size(), 1);
  QCOMPARE(static_cast<int>(header.m_cellHighlights.at(0).first().start), 0);
  QCOMPARE(static_cast<int>(header.m_cellHighlights.at(0).first().length), 5);
}

void TestASTWalker::testTableCellHighlightsInListAndRaggedRow() {
  QByteArray md = "- item\n"
                  "\n"
                  "  | **a** | b |\n"
                  "  | - | - |\n"
                  "  | *c* |\n";
  auto result = vte::md::walkAndConvert(md, 5);
  QCOMPARE(result.tableElements.size(), 1);

  const auto &table = result.tableElements.first();
  QCOMPARE(table.m_startBlock, 2);
  QCOMPARE(table.m_rows.size(), 3);

  const QString line = QStringLiteral("  | **a** | b |");
  const auto &header = table.m_rows.at(0);
  QCOMPARE(header.m_prefix, QStringLiteral("  "));
  QCOMPARE(line.mid(header.m_cellOffsets.at(0), header.m_cells.at(0).size()), header.m_cells.at(0));
  QCOMPARE(header.m_cellHighlights.at(0).size(), 1);
  QCOMPARE(static_cast<int>(header.m_cellHighlights.at(0).first().start), 0);
  QCOMPARE(static_cast<int>(header.m_cellHighlights.at(0).first().length), 5);

  // A ragged body row keeps its own width, and the matrices stay parallel.
  const auto &body = table.m_rows.at(2);
  QCOMPARE(body.m_cells.size(), 1);
  QCOMPARE(body.m_cellOffsets.size(), 1);
  QCOMPARE(body.m_cellHighlights.size(), 1);
  QCOMPARE(body.m_cellHighlights.at(0).size(), 1);
  QCOMPARE(static_cast<int>(body.m_cellHighlights.at(0).first().start), 0);
  QCOMPARE(static_cast<int>(body.m_cellHighlights.at(0).first().length), 3);

  // A plain row still gets one (empty) entry per cell.
  const auto &delimiter = table.m_rows.at(1);
  QCOMPARE(delimiter.m_cellHighlights.size(), delimiter.m_cells.size());
}

void TestASTWalker::testHeadingElements() {
  // The rendered title, not the raw markdown line.
  {
    QByteArray md = "## A **bold** `x`\n";
    auto result = vte::md::walkAndConvert(md, 1);
    QCOMPARE(result.headingElements.size(), 1);
    const auto &h = result.headingElements.first();
    QCOMPARE(h.m_level, 2);
    QCOMPARE(h.m_title, QStringLiteral("A bold x"));
    QCOMPARE(h.m_anchorText, QStringLiteral("A bold x"));
  }

  // A link contributes only its text, never its destination.
  {
    auto result = vte::md::walkAndConvert(QByteArray("## [a](b)\n"), 1);
    QCOMPARE(result.headingElements.size(), 1);
    QCOMPARE(result.headingElements.first().m_title, QStringLiteral("a"));
    QCOMPARE(result.headingElements.first().m_anchorText, QStringLiteral("a"));
  }

  // Entities are decoded exactly once, by cmark. No re-escaping anywhere.
  {
    auto result = vte::md::walkAndConvert(QByteArray("## a &amp; b\n"), 1);
    QCOMPARE(result.headingElements.first().m_title, QStringLiteral("a & b"));
  }
  {
    auto result = vte::md::walkAndConvert(QByteArray("## &amp;lt;\n"), 1);
    QCOMPARE(result.headingElements.first().m_title, QStringLiteral("&lt;"));
  }
  {
    auto result = vte::md::walkAndConvert(QByteArray("## &amp;amp;\n"), 1);
    QCOMPARE(result.headingElements.first().m_title, QStringLiteral("&amp;"));
  }

  // Inline HTML is a leaf whose literal is the tag: it is dropped, matching
  // the browser's textContent.
  {
    auto result = vte::md::walkAndConvert(QByteArray("## <b>x</b>\n"), 1);
    QCOMPARE(result.headingElements.first().m_title, QStringLiteral("x"));
  }

  // An image contributes nothing; its alt text is not textContent. The anchor
  // input keeps the trailing space of the preceding text token, exactly as
  // markdown-it-anchor's token concatenation does.
  {
    auto result = vte::md::walkAndConvert(QByteArray("## ![a](b)\n"), 1);
    QCOMPARE(result.headingElements.size(), 1);
    QCOMPARE(result.headingElements.first().m_title, QString());
  }
  {
    auto result = vte::md::walkAndConvert(QByteArray("## a ![x](y)\n"), 1);
    QCOMPARE(result.headingElements.first().m_title, QStringLiteral("a"));
    QCOMPARE(result.headingElements.first().m_anchorText, QStringLiteral("a "));
  }

  // An empty heading yields empty strings rather than no element.
  {
    auto result = vte::md::walkAndConvert(QByteArray("##\n"), 1);
    QCOMPARE(result.headingElements.size(), 1);
    QCOMPARE(result.headingElements.first().m_title, QString());
    QCOMPARE(result.headingElements.first().m_anchorText, QString());
  }

  // Setext headings are published too, at the title line.
  {
    QByteArray md = "Title\n=====\n";
    auto result = vte::md::walkAndConvert(md, 2);
    QCOMPARE(result.headingElements.size(), 1);
    QCOMPARE(result.headingElements.first().m_level, 1);
    QCOMPARE(result.headingElements.first().m_title, QStringLiteral("Title"));
    QCOMPARE(result.headingElements.first().m_startPos, 0);
  }

  // A soft break is whitespace in the rendered title but contributes nothing
  // to the anchor input, because markdown-it drops the softbreak token.
  {
    QByteArray md = "Foo\nbar\n---\n";
    auto result = vte::md::walkAndConvert(md, 3);
    QCOMPARE(result.headingElements.size(), 1);
    QCOMPARE(result.headingElements.first().m_title, QStringLiteral("Foo bar"));
    QCOMPARE(result.headingElements.first().m_anchorText, QStringLiteral("Foobar"));
  }

  // A hard break shares that production code; assert it directly rather than
  // relying on the soft-break case to cover both.
  {
    QByteArray md = "Foo  \nbar\n---\n";
    auto result = vte::md::walkAndConvert(md, 3);
    QCOMPARE(result.headingElements.size(), 1);
    QCOMPARE(result.headingElements.first().m_title, QStringLiteral("Foo bar"));
    QCOMPARE(result.headingElements.first().m_anchorText, QStringLiteral("Foobar"));
  }

  // A `#`-looking line inside a fenced code block is not a heading.
  {
    QByteArray md = "```\n# not a heading\n```\n";
    auto result = vte::md::walkAndConvert(md, 3);
    QVERIFY(result.headingElements.isEmpty());
  }

  // Order follows the document, and a fast parse publishes nothing.
  {
    QByteArray md = "# one\n\n## two\n\n### three\n";
    auto result = vte::md::walkAndConvert(md, 5);
    QCOMPARE(result.headingElements.size(), 3);
    QCOMPARE(result.headingElements.at(0).m_title, QStringLiteral("one"));
    QCOMPARE(result.headingElements.at(1).m_title, QStringLiteral("two"));
    QCOMPARE(result.headingElements.at(2).m_title, QStringLiteral("three"));
    QVERIFY(result.headingElements.at(0).m_startPos < result.headingElements.at(1).m_startPos);
    QVERIFY(result.headingElements.at(1).m_startPos < result.headingElements.at(2).m_startPos);

    auto fast = vte::md::walkAndConvert(md, 5, 0, 0, true);
    QVERIFY(fast.headingElements.isEmpty());
  }
}

void TestASTWalker::testHeadingElementsDivergence() {
  // Preview-only markdown-it plugins are invisible to cmark, so the anchor
  // text keeps the literal source here where markdown-it-anchor would not.
  // Pinned so a future change to either side is deliberate rather than
  // discovered.
  {
    auto result = vte::md::walkAndConvert(QByteArray("## :smile:\n"), 1);
    QCOMPARE(result.headingElements.size(), 1);
    QCOMPARE(result.headingElements.first().m_title, QStringLiteral(":smile:"));
    QCOMPARE(result.headingElements.first().m_anchorText, QStringLiteral(":smile:"));
  }

  // Inline math is a fork extension node: cmark strips the `$` delimiters and
  // the literal is the expression source. markdown-it's texmath produces a
  // token type the anchor plugin drops entirely, so the two disagree here.
  {
    auto result = vte::md::walkAndConvert(QByteArray("## $x$\n"), 1);
    QCOMPARE(result.headingElements.size(), 1);
    QCOMPARE(result.headingElements.first().m_title, QStringLiteral("x"));
    QCOMPARE(result.headingElements.first().m_anchorText, QStringLiteral("x"));
  }

  // Footnotes: the preview renders a numbered `[n]` marker, which cmark cannot
  // reproduce here, and markdown-it-anchor drops the token from the slug. Both
  // node forms are skipped; in particular the inline form must NOT splice its
  // body into the heading.
  {
    auto result = vte::md::walkAndConvert(QByteArray("## Head [^x]\n\n[^x]: note\n"), 3);
    QCOMPARE(result.headingElements.size(), 1);
    QCOMPARE(result.headingElements.first().m_title, QStringLiteral("Head"));
    QCOMPARE(result.headingElements.first().m_anchorText, QStringLiteral("Head "));
  }
  {
    auto result = vte::md::walkAndConvert(QByteArray("## Head ^[note]\n"), 1);
    QCOMPARE(result.headingElements.size(), 1);
    QCOMPARE(result.headingElements.first().m_title, QStringLiteral("Head"));
    QCOMPARE(result.headingElements.first().m_anchorText, QStringLiteral("Head "));
  }
}

// Per-cell syntax highlighting parses each cell payload as its own cmark
// document. The 300-cell ceiling is PER TABLE, so before the document-wide
// budget a file of many medium tables paid that cost once per table with no
// upper bound at all.
//
// The fixture is deliberately many small tables rather than one huge one: a
// single 300x300 table trips the per-table guard first and would never reach a
// document-wide budget, so it cannot test this.
void TestASTWalker::testHtmlCellHighlightBudgetIsDocumentWide() {
  // CellHighlightBudget::m_remaining.
  const int budget = 5000;
  const int cellsPerTable = 4;
  const int tablesWithinBudget = budget / cellsPerTable;

  // One table past the budget, so the boundary itself is covered.
  const int tableCount = tablesWithinBudget + 1;

  QByteArray md;
  for (int i = 0; i < tableCount; ++i) {
    md += "para\n\n";
    md += "<table>\n";
    md += "<tr><td><!--vte-md:**a**--><p><strong>a</strong></p></td>"
          "<td><!--vte-md:**b**--><p><strong>b</strong></p></td></tr>\n";
    md += "<tr><td><!--vte-md:**c**--><p><strong>c</strong></p></td>"
          "<td><!--vte-md:**d**--><p><strong>d</strong></p></td></tr>\n";
    md += "</table>\n\n";
  }

  const int numBlocks = md.count('\n') + 1;
  auto result = vte::md::walkAndConvert(md, numBlocks);
  QCOMPARE(result.tableElements.size(), tableCount);

  int highlighted = 0;
  int bare = 0;
  for (const auto &table : result.tableElements) {
    // Every table is captured and every cell is present either way: the budget
    // only drops the syntax RUNS, never the content.
    QCOMPARE(table.m_rows.size(), 2);
    for (const auto &row : table.m_rows) {
      QCOMPARE(row.m_cells.size(), 2);
      for (const auto &runs : row.m_cellHighlights) {
        if (runs.isEmpty()) {
          ++bare;
        } else {
          ++highlighted;
        }
      }
    }
  }

  // Exact counts either side of the boundary. A table is charged WHOLE, so the
  // split lands on a table edge, never inside one.
  QCOMPARE(highlighted, tablesWithinBudget * cellsPerTable);
  QCOMPARE(bare, cellsPerTable);

  // And a document comfortably inside the budget is completely unaffected.
  QByteArray small = "para\n\n<table>\n<tr><td><!--vte-md:**a**--><p><strong>a</strong></p></td>"
                     "</tr>\n</table>\n";
  auto smallResult = vte::md::walkAndConvert(small, small.count('\n') + 1);
  QCOMPARE(smallResult.tableElements.size(), 1);
  QVERIFY(!smallResult.tableElements.first().m_rows.first().m_cellHighlights.first().isEmpty());

  // A table which would never highlight in the first place must not consume
  // the budget. An HTML-only table (no `<!--vte-md:-->` payload) has literal
  // text cells and performs no snippet parse at all, so putting a run of them
  // in front of an eligible table must not change that table's outcome.
  //
  // Without this the result would depend on where in the document the
  // ineligible tables happened to sit, which is not a policy anyone could
  // reason about.
  QByteArray mixed;
  for (int i = 0; i < tablesWithinBudget; ++i) {
    mixed += "para\n\n<table>\n<tr><td>plain</td><td>plain</td></tr>\n"
             "<tr><td>plain</td><td>plain</td></tr>\n</table>\n\n";
  }
  mixed += "para\n\n<table>\n<tr><td><!--vte-md:**z**--><p><strong>z</strong></p></td></tr>\n"
           "</table>\n";

  auto mixedResult = vte::md::walkAndConvert(mixed, mixed.count('\n') + 1);
  QCOMPARE(mixedResult.tableElements.size(), tablesWithinBudget + 1);
  const auto &trailing = mixedResult.tableElements.last();
  QVERIFY2(!trailing.m_rows.first().m_cellHighlights.first().isEmpty(),
           "HTML-only tables consumed the per-cell highlighting budget");
}

QTEST_MAIN(tests::TestASTWalker)
