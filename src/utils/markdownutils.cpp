#include <vtextedit/markdownutils.h>

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTextBlock>
#include <QTextDocument>
#include <QUrl>

#include <markdowneditor/pegparser.h>
#include <vtextedit/texteditutils.h>
#include <vtextedit/textutils.h>
#include <vtextedit/vtextedit.h>

using namespace vte;

const QString MarkdownUtils::c_fencedCodeBlockStartRegExp =
    QStringLiteral("^(\\s*)([`~])\\2{2}((?:(?!\\2)[^\\r\\n])*)$");

const QString MarkdownUtils::c_fencedCodeBlockEndRegExp =
    QStringLiteral("^(\\s*)([`~])\\2{2}\\s*$");

const QString MarkdownUtils::c_imageTitleRegExp = QStringLiteral("[^\\[\\]]*");

const QString MarkdownUtils::c_imageAltRegExp = QStringLiteral("[^\"'()]*");

const QString MarkdownUtils::c_imageLinkRegExp =
    QStringLiteral("\\!\\[([^\\[\\]]*)\\]"
                   "\\(\\s*"
                   "([^\\)\"'\\s]+)"
                   "(\\s*(\"[^\"\\)\\n\\r]*\")|('[^'\\)\\n\\r]*'))?"
                   "(\\s*=(\\d*)x(\\d*))?"
                   "\\s*\\)");

const QString MarkdownUtils::c_linkRegExp =
    QStringLiteral("\\[([^\\[\\]]*)\\]"
                   "\\(\\s*"
                   "([^\\)\"'\\s]+)"
                   "(\\s*(\"[^\"\\)\\n\\r]*\")|('[^'\\)\\n\\r]*'))?"
                   "\\s*\\)");

// Constrain the main section number digits within 3 chars to avoid treating a
// date like 20210101 as a section number.
const QString MarkdownUtils::c_headerRegExp =
    QStringLiteral("^(#{1,6})(\\s+)((\\d{1,3}(?:\\.\\d+)*\\.?(?=\\s))?(\\s*)(?:\\S.*)?)$");

const QString MarkdownUtils::c_todoListRegExp =
    QStringLiteral("^(\\s*)([\\*\\-\\+])\\s+\\[([ x])\\]\\s*(.*)$");

const QString MarkdownUtils::c_orderedListRegExp = QStringLiteral("^(\\s*)(\\d+)\\.\\s+(.*)$");

const QString MarkdownUtils::c_unorderedListRegExp =
    QStringLiteral("^(\\s*)([\\*\\-\\+])\\s+(.*)$");

const QString MarkdownUtils::c_quoteRegExp = QStringLiteral("^(\\s*)>\\s+(.*)$");

QString MarkdownUtils::unindentCodeBlockText(const QString &p_text) {
  if (p_text.isEmpty()) {
    return p_text;
  }

  QStringList lines = p_text.split('\n');
  Q_ASSERT(lines.size() > 1);
  Q_ASSERT(isFencedCodeBlockStartMark(lines[0]));

  int nrSpaces = TextUtils::fetchIndentation(lines[0]);
  if (nrSpaces == 0) {
    return p_text;
  }

  QString res = lines[0].right(lines[0].size() - nrSpaces);
  for (int i = 1; i < lines.size(); ++i) {
    res = res + "\n" + TextUtils::unindentText(lines[i], nrSpaces);
  }

  return res;
}

bool MarkdownUtils::isFencedCodeBlockStartMark(const QString &p_text) {
  auto text = p_text.trimmed();
  return text.startsWith(QStringLiteral("```")) || text.startsWith(QStringLiteral("~~~"));
}

bool MarkdownUtils::hasImageLink(const QString &p_text) {
  QRegularExpression regExp(vte::MarkdownUtils::c_imageLinkRegExp);
  auto match = regExp.match(p_text);
  return match.hasMatch();
}

QString MarkdownUtils::fetchImageLinkUrl(const QString &p_text, int &p_width, int &p_height) {
  QRegularExpression regExp(c_imageLinkRegExp);

  p_width = p_height = -1;

  int index = p_text.indexOf(regExp);
  if (index == -1) {
    return QString();
  }

  QRegularExpressionMatch match;
  int lastIndex = p_text.lastIndexOf(regExp, -1, &match);
  if (lastIndex != index) {
    return QString();
  }

  QString tmp(match.captured(7));
  if (!tmp.isEmpty()) {
    p_width = tmp.toInt();
    if (p_width <= 0) {
      p_width = -1;
    }
  }

  tmp = match.captured(8);
  if (!tmp.isEmpty()) {
    p_height = tmp.toInt();
    if (p_height <= 0) {
      p_height = -1;
    }
  }

  return match.captured(2).trimmed();
}

QString MarkdownUtils::linkUrlToPath(const QString &p_basePath, const QString &p_url) {
  QString fullPath;
  QFileInfo info(p_basePath, TextUtils::purifyUrl(p_url));
  if (info.exists()) {
    if (info.isNativePath() || isQrcPath(p_basePath)) {
      // Local file.
      fullPath = QDir::cleanPath(info.absoluteFilePath());
    } else {
      fullPath = p_url;
    }
  } else {
    const auto decodedUrl = TextUtils::decodeUrl(p_url);
    QFileInfo dinfo(p_basePath, decodedUrl);
    if (dinfo.exists()) {
      if (dinfo.isNativePath() || isQrcPath(p_basePath)) {
        // Local file.
        fullPath = QDir::cleanPath(dinfo.absoluteFilePath());
      } else {
        fullPath = p_url;
      }
    } else {
      QUrl url(p_url);
      if (url.isLocalFile()) {
        fullPath = url.toLocalFile();
      } else {
        fullPath = url.toString();
      }
    }
  }

  return fullPath;
}

bool MarkdownUtils::isQrcPath(const QString &p_path) {
  return p_path.startsWith(QStringLiteral(":/"));
}

QPixmap MarkdownUtils::scaleImage(const QPixmap &p_img, int p_width, int p_height,
                                  qreal p_scaleFactor) {
  const Qt::TransformationMode tMode = Qt::SmoothTransformation;
  if (p_width > 0) {
    if (p_height > 0) {
      return p_img.scaled(p_width * p_scaleFactor, p_height * p_scaleFactor, Qt::IgnoreAspectRatio,
                          tMode);
    } else {
      return p_img.scaledToWidth(p_width * p_scaleFactor, tMode);
    }
  } else if (p_height > 0) {
    return p_img.scaledToHeight(p_height * p_scaleFactor, tMode);
  } else {
    if (p_scaleFactor < 1.1) {
      return p_img;
    } else {
      return p_img.scaledToWidth(p_img.width() * p_scaleFactor, tMode);
    }
  }
}

void MarkdownUtils::typeHeading(VTextEdit *p_edit, int p_level) {
  doOnSelectedLinesOrCurrentLine(p_edit, &MarkdownUtils::insertHeading, &p_level);
}

bool MarkdownUtils::insertHeading(QTextCursor &p_cursor, const QTextBlock &p_block, void *p_level) {
  if (!p_block.isValid()) {
    return false;
  }

  const int targetLevel = *static_cast<int *>(p_level);
  Q_ASSERT(targetLevel >= 0 && targetLevel <= 6);

  p_cursor.setPosition(p_block.position());

  // Test if this block contains title marks.
  QString text = p_block.text();
  bool textChanged = false;
  auto match = matchHeader(text);
  if (match.m_matched) {
    const int level = match.m_level;
    if (level == targetLevel) {
      return false;
    } else {
      // Remove the title mark.
      int length = level;
      if (targetLevel == 0) {
        // Remove the whole prefix till the heading content.
        length = match.m_level + match.m_spacesAfterMarker;
      }

      p_cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, length);
      p_cursor.removeSelectedText();
      textChanged = true;
    }
  }

  // Insert heading mark + " " at the front of the block.
  if (targetLevel > 0) {
    // Remove the spaces at front.
    if (textChanged) {
      text = p_block.text();
    }
    const int firstNonSpaceChar = TextUtils::firstNonSpace(text);
    p_cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor,
                          firstNonSpaceChar > -1 ? firstNonSpaceChar : text.length());

    const QString mark(targetLevel, '#');
    p_cursor.insertText(mark + " ");
  }

  // Go to the end of this block.
  p_cursor.movePosition(QTextCursor::EndOfBlock);
  return true;
}

// TODO: get more information from highlighter result.
void MarkdownUtils::typeMarker(VTextEdit *p_edit, const QString &p_startMarker,
                               const QString &p_endMarker, bool p_allowSpacesAtTwoEnds) {
  const int totalMarkersSize = p_startMarker.size() + p_endMarker.size();
  auto cursor = p_edit->textCursor();
  cursor.beginEditBlock();
  if (p_edit->hasSelection()) {
    bool done = false;
    const auto &selection = p_edit->getSelection();
    int start = selection.start();
    int end = selection.end();
    if (TextEditUtils::crossBlocks(p_edit, start, end)) {
      // Do not support markers corssing blocks.
      done = true;
      cursor.endEditBlock();
      return;
    }

    if (end - start >= totalMarkersSize) {
      const auto text = p_edit->selectedText();
      if (text.startsWith(p_startMarker) && text.endsWith(p_endMarker)) {
        // Remove the marker.
        done = true;
        cursor.clearSelection();

        cursor.setPosition(start, QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor,
                            p_startMarker.size());
        cursor.deleteChar();

        cursor.setPosition(end - p_startMarker.size(), QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor,
                            p_endMarker.size());
        cursor.deleteChar();
      }
    }

    if (!done) {
      cursor.clearSelection();
      cursor.setPosition(start, QTextCursor::MoveAnchor);
      cursor.insertText(p_startMarker);
      cursor.setPosition(end + p_startMarker.size(), QTextCursor::MoveAnchor);
      cursor.insertText(p_endMarker);
    }
  } else {
    const int pib = cursor.positionInBlock();
    const QString text = cursor.block().text();
    bool done = false;

    {
      // Use regexp to match current line to check if we already locate within
      // one pair of markers. If so, remove the markers. Otherwise, insert new
      // ones.
      QString regExp;
      if (p_allowSpacesAtTwoEnds) {
        if (p_endMarker.size() == 1) {
          regExp = QStringLiteral("%1[^%2]+%2")
                       .arg(QRegularExpression::escape(p_startMarker),
                            QRegularExpression::escape(p_endMarker));
        } else {
          regExp = QStringLiteral("%1(?:[^%2]|%2(?!%3))+%4")
                       .arg(QRegularExpression::escape(p_startMarker),
                            QRegularExpression::escape(p_endMarker[0]),
                            QRegularExpression::escape(p_endMarker.mid(1)),
                            QRegularExpression::escape(p_endMarker));
        }
      } else {
        if (p_endMarker.size() == 1) {
          regExp = QStringLiteral("%1[^%2\\s](?:\\s*[^%2\\s]*)*%2")
                       .arg(QRegularExpression::escape(p_startMarker),
                            QRegularExpression::escape(p_endMarker));
        } else {
          regExp = QStringLiteral("%1(?:[^%2\\s]|%2(?!%3))(?:\\s*(?:[^%2\\s]|%2(?!%3))*)*%4")
                       .arg(QRegularExpression::escape(p_startMarker),
                            QRegularExpression::escape(p_endMarker[0]),
                            QRegularExpression::escape(p_endMarker.mid(1)),
                            QRegularExpression::escape(p_endMarker));
        }
      }

      QRegularExpression reg(regExp);
      int pos = 0;
      while (pos < text.size()) {
        QRegularExpressionMatch match;
        int idx = text.indexOf(reg, pos, &match);
        if (idx == -1 || idx > pib) {
          break;
        }
        pos = idx + match.capturedLength();
        if (pib == pos - p_endMarker.size()) {
          // Just skip the end marker.
          done = true;
          cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor,
                              p_endMarker.size());
          break;
        } else if (pib >= idx && pib < pos) {
          // Cursor hits. Remove the markers.
          done = true;
          cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::MoveAnchor, pib - idx);
          cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor,
                              p_startMarker.size());
          cursor.deleteChar();

          cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor,
                              match.capturedLength() - totalMarkersSize);
          cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor,
                              p_endMarker.size());
          cursor.deleteChar();

          // Restore cursor.
          int cursorPos = cursor.block().position() + pib;
          if (pib > idx) {
            cursorPos -= p_startMarker.size();
          }
          cursor.setPosition(cursorPos);
          break;
        }
      }
    }

    if (!done && pib <= text.size() - p_endMarker.size()) {
      if (pib <= text.size() - totalMarkersSize) {
        if (text.mid(pib, totalMarkersSize) == p_startMarker + p_endMarker) {
          done = true;
          cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor,
                              totalMarkersSize);
          cursor.removeSelectedText();
        }
      }

      if (!done && text.mid(pib, p_endMarker.size()) == p_endMarker) {
        done = true;
        if (pib >= p_startMarker.size() &&
            text.mid(pib - p_startMarker.size(), p_startMarker.size()) == p_startMarker) {
          cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::MoveAnchor,
                              p_startMarker.size());
          cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor,
                              totalMarkersSize);
          cursor.removeSelectedText();
        } else {
          cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor,
                              p_endMarker.size());
        }
      }
    }

    if (!done) {
      cursor.insertText(p_startMarker + p_endMarker);
      cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::MoveAnchor,
                          p_endMarker.size());
    }
  }

  cursor.endEditBlock();
  p_edit->setTextCursor(cursor);
}

void MarkdownUtils::typeBold(VTextEdit *p_edit) {
  typeMarker(p_edit, QStringLiteral("**"), QStringLiteral("**"));
}

void MarkdownUtils::typeItalic(VTextEdit *p_edit) {
  typeMarker(p_edit, QStringLiteral("*"), QStringLiteral("*"));
}

void MarkdownUtils::typeStrikethrough(VTextEdit *p_edit) {
  typeMarker(p_edit, QStringLiteral("~~"), QStringLiteral("~~"));
}

void MarkdownUtils::typeMark(VTextEdit *p_edit) {
  typeMarker(p_edit, QStringLiteral("<mark>"), QStringLiteral("</mark>"));
}

void MarkdownUtils::typeUnorderedList(VTextEdit *p_edit) {
  doOnSelectedLinesOrCurrentLine(p_edit, &MarkdownUtils::insertUnorderedList, nullptr);
}

bool MarkdownUtils::insertUnorderedList(QTextCursor &p_cursor, const QTextBlock &p_block,
                                        void *p_data) {
  // 1. If we have selection, we process each line one by one;
  // 2. If current line is a todo list, turn it into a normal unordered list;
  // 3. If current line is an ordered list, turn it into an unordered list;
  // 4. If current line is an unordered list, turn it into a normal line;
  // 5. Insert an unordered list treating the front spaces as indentation.
  Q_UNUSED(p_data);
  p_cursor.setPosition(p_block.position());

  const auto text = p_block.text();

  // Check todo list.
  {
    QRegularExpression reg(c_todoListRegExp);
    auto match = reg.match(text);
    if (match.hasMatch()) {
      TextEditUtils::selectBlockUnderCursor(p_cursor);
      p_cursor.insertText(
          QStringLiteral("%1%2 %3").arg(match.captured(1), match.captured(2), match.captured(4)));
      return true;
    }
  }

  // Check ordered list.
  {
    QRegularExpression reg(c_orderedListRegExp);
    auto match = reg.match(text);
    if (match.hasMatch()) {
      TextEditUtils::selectBlockUnderCursor(p_cursor);
      p_cursor.insertText(QStringLiteral("%1* %2").arg(match.captured(1), match.captured(3)));
      return true;
    }
  }

  // Check unordered list.
  {
    QRegularExpression reg(c_unorderedListRegExp);
    auto match = reg.match(text);
    if (match.hasMatch()) {
      TextEditUtils::selectBlockUnderCursor(p_cursor);
      p_cursor.insertText(QStringLiteral("%1%2").arg(match.captured(1), match.captured(3)));
      return true;
    }
  }

  // Insert unordered list.
  {
    int indentation = TextUtils::fetchIndentation(text);
    p_cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor, indentation);
    p_cursor.insertText(QStringLiteral("* "));
    p_cursor.movePosition(QTextCursor::EndOfBlock);
    return true;
  }
}

void MarkdownUtils::doOnSelectedLinesOrCurrentLine(
    VTextEdit *p_edit, const std::function<bool(QTextCursor &, const QTextBlock &, void *)> &p_func,
    void *p_data) {
  auto doc = p_edit->document();
  auto cursor = p_edit->textCursor();
  auto firstBlock = cursor.block();
  auto lastBlock = firstBlock;

  if (p_edit->hasSelection()) {
    const auto &selection = p_edit->getSelection();
    firstBlock = doc->findBlock(selection.start());
    lastBlock = doc->findBlock(selection.end());
  }

  bool changed = false;

  cursor.beginEditBlock();
  cursor.clearSelection();
  while (firstBlock.isValid()) {
    changed = p_func(cursor, firstBlock, p_data) | changed;
    if (!(firstBlock < lastBlock)) {
      break;
    }
    firstBlock = firstBlock.next();
  }
  cursor.endEditBlock();

  if (changed) {
    p_edit->setTextCursor(cursor);
  }
}

void MarkdownUtils::typeOrderedList(VTextEdit *p_edit) {
  doOnSelectedLinesOrCurrentLine(p_edit, &MarkdownUtils::insertOrderedList, nullptr);
}

bool MarkdownUtils::insertOrderedList(QTextCursor &p_cursor, const QTextBlock &p_block,
                                      void *p_data) {
  // 1. If we have selection, we process each line one by one;
  // 2. If current line is an unordered list, turn it into an ordered list;
  // 4. If current line is an ordered list, turn it into a normal line;
  // 5. Insert an ordered list treating the front spaces as indentation.
  Q_UNUSED(p_data);

  p_cursor.setPosition(p_block.position());

  const auto text = p_block.text();

  // Check todo list.
  {
    QRegularExpression reg(c_todoListRegExp);
    auto match = reg.match(text);
    if (match.hasMatch()) {
      TextEditUtils::selectBlockUnderCursor(p_cursor);
      p_cursor.insertText(match.captured(1) + QStringLiteral("1. %1").arg(match.captured(4)));
      return true;
    }
  }

  // Check unordered list.
  {
    QRegularExpression reg(c_unorderedListRegExp);
    auto match = reg.match(text);
    if (match.hasMatch()) {
      TextEditUtils::selectBlockUnderCursor(p_cursor);
      p_cursor.insertText(match.captured(1) + QStringLiteral("1. %1").arg(match.captured(3)));
      return true;
    }
  }

  // Check ordered list.
  {
    QRegularExpression reg(c_orderedListRegExp);
    auto match = reg.match(text);
    if (match.hasMatch()) {
      TextEditUtils::selectBlockUnderCursor(p_cursor);
      p_cursor.insertText(QStringLiteral("%1%2").arg(match.captured(1), match.captured(3)));
      return true;
    }
  }

  // Insert ordered list.
  {
    int indentation = TextUtils::fetchIndentation(text);
    p_cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor, indentation);
    p_cursor.insertText(QStringLiteral("1. "));
    p_cursor.movePosition(QTextCursor::EndOfBlock);
    return true;
  }
}

void MarkdownUtils::typeTodoList(VTextEdit *p_edit, bool p_checked) {
  doOnSelectedLinesOrCurrentLine(p_edit, &MarkdownUtils::insertTodoList, &p_checked);
}

bool MarkdownUtils::insertTodoList(QTextCursor &p_cursor, const QTextBlock &p_block,
                                   void *p_checked) {
  // For unchecked list:
  // 1. If we have selection, we process each line one by one;
  // 2. If current line is a checked todo list, uncheck it;
  // 3. If current line is an unchecked todo list, turn it into a normal line;
  // 4. If current line is an unordered list, turn it into a todo list;
  // 5. If current line is an ordered list, turn it into a todo list;
  // 6. Insert a todo list treating the front spaces as indentation.
  p_cursor.setPosition(p_block.position());

  const auto text = p_block.text();
  const bool checked = *static_cast<bool *>(p_checked);
  const QString checkMark = checked ? QStringLiteral("[x]") : QStringLiteral("[ ]");

  // Check todo list.
  {
    QRegularExpression reg(c_todoListRegExp);
    auto match = reg.match(text);
    if (match.hasMatch()) {
      TextEditUtils::selectBlockUnderCursor(p_cursor);
      bool currentChecked = match.captured(3) == QStringLiteral("x");
      if (currentChecked == checked) {
        // Turn it into normal line.
        p_cursor.insertText(QStringLiteral("%1%2").arg(match.captured(1), match.captured(4)));
      } else {
        // Uncheck/Check it.
        p_cursor.insertText(
            QStringLiteral("%1%2 %3 %4")
                .arg(match.captured(1), match.captured(2), checkMark, match.captured(4)));
      }
      return true;
    }
  }

  // Check unordered list.
  {
    QRegularExpression reg(c_unorderedListRegExp);
    auto match = reg.match(text);
    if (match.hasMatch()) {
      TextEditUtils::selectBlockUnderCursor(p_cursor);
      p_cursor.insertText(
          QStringLiteral("%1%2 %3 %4")
              .arg(match.captured(1), match.captured(2), checkMark, match.captured(3)));
      return true;
    }
  }

  // Check ordered list.
  {
    QRegularExpression reg(c_orderedListRegExp);
    auto match = reg.match(text);
    if (match.hasMatch()) {
      TextEditUtils::selectBlockUnderCursor(p_cursor);
      p_cursor.insertText(
          QStringLiteral("%1* %2 %3").arg(match.captured(1), checkMark, match.captured(3)));
      return true;
    }
  }

  // Insert todo list.
  {
    int indentation = TextUtils::fetchIndentation(text);
    p_cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor, indentation);
    p_cursor.insertText(QStringLiteral("* %1 ").arg(checkMark));
    p_cursor.movePosition(QTextCursor::EndOfBlock);
    return true;
  }
}

void MarkdownUtils::typeCode(VTextEdit *p_edit) {
  typeMarker(p_edit, QStringLiteral("`"), QStringLiteral("`"), true);
}

void MarkdownUtils::typeCodeBlock(VTextEdit *p_edit) {
  typeBlockMarker(p_edit, QStringLiteral("```"), QStringLiteral("```"),
                  CursorPosition::StartMarker);
}

void MarkdownUtils::typeBlockMarker(VTextEdit *p_edit, const QString &p_startMarker,
                                    const QString &p_endMarker, CursorPosition p_cursorPosition) {
  // 1. If we have selection, insert markers to wrap selected lines;
  // 2. If we are inside empty block markers, delete these markers;
  // 3. Insert markers respecting the indentation and insert one empty line.
  auto cursor = p_edit->textCursor();

  cursor.beginEditBlock();
  if (p_edit->hasSelection()) {
    const auto &selection = p_edit->getSelection();
    int start = selection.start();
    int end = selection.end();

    cursor.clearSelection();
    cursor.setPosition(start);
    const QString indentSpaces = TextUtils::fetchIndentationSpaces(cursor.block().text());
    cursor.setPosition(end);
    TextEditUtils::insertBlock(cursor, false);
    cursor.insertText(indentSpaces + p_endMarker);

    QTextBlock endBlock = cursor.block();

    cursor.setPosition(start);
    TextEditUtils::insertBlock(cursor, true);
    cursor.insertText(indentSpaces + p_startMarker);

    if (p_cursorPosition != CursorPosition::StartMarker) {
      cursor.setPosition(endBlock.position());
    }
    cursor.movePosition(QTextCursor::EndOfBlock);
  } else {
    // We allow chars after start marker.
    QRegularExpression startReg(
        QStringLiteral("^(\\s*)%1").arg(QRegularExpression::escape(p_startMarker)));
    QRegularExpression endReg(
        QStringLiteral("^(\\s*)%1$").arg(QRegularExpression::escape(p_endMarker)));

    auto block = cursor.block();
    const auto currentText = block.text();

    bool done = false;

    // Locate at start block and try to delete empty block.
    {
      QTextBlock endBlock;
      auto match = startReg.match(currentText);
      if (match.hasMatch()) {
        // Check if it is an empty block marker.
        auto nextBlock = block.next();
        if (nextBlock.isValid()) {
          auto nextMatch = endReg.match(nextBlock.text());
          if (nextMatch.hasMatch()) {
            if (nextMatch.captured(1) == match.captured(1)) {
              // It is an empty marker block. Just delete it.
              endBlock = nextBlock;
            }
          } else if (nextBlock.text().isEmpty()) {
            // Try one block further only when current next block is empty.
            nextBlock = nextBlock.next();
            if (nextBlock.isValid()) {
              nextMatch = endReg.match(nextBlock.text());
              if (nextMatch.hasMatch() && nextMatch.captured(1) == match.captured(1)) {
                endBlock = nextBlock;
              }
            }
          }
        }

        if (endBlock.isValid()) {
          // We found a block marker [block, endBlock]. Delete it.
          done = true;
          cursor.setPosition(block.position());
          cursor.setPosition(endBlock.position() + endBlock.length() - 1, QTextCursor::KeepAnchor);
          cursor.deleteChar();
        }
      }
    }

    // Locate between start block and end block and try to delete empty block.
    if (!done) {
      auto previousBlock = block.previous();
      // Only delete empty marker block.
      if (previousBlock.isValid() && currentText.isEmpty()) {
        auto match = startReg.match(previousBlock.text());
        if (match.hasMatch()) {
          auto nextBlock = block.next();
          if (nextBlock.isValid()) {
            auto nextMatch = endReg.match(nextBlock.text());
            if (nextMatch.hasMatch() && match.captured(1) == nextMatch.captured(1)) {
              done = true;
              cursor.setPosition(previousBlock.position());
              cursor.setPosition(nextBlock.position() + nextBlock.length() - 1,
                                 QTextCursor::KeepAnchor);
              cursor.deleteChar();
            }
          }
        }
      }
    }

    // Locate at end block and try to delete empty block.
    if (!done) {
      QTextBlock startBlock;
      auto match = endReg.match(currentText);
      if (match.hasMatch()) {
        // Check if it is an empty block marker.
        auto previousBlock = block.previous();
        if (previousBlock.isValid()) {
          auto nextMatch = startReg.match(previousBlock.text());
          if (nextMatch.hasMatch()) {
            if (nextMatch.captured(1) == match.captured(1)) {
              startBlock = previousBlock;
            }
          } else {
            // Try one block further only when current previous block is empty.
            previousBlock = previousBlock.previous();
            if (previousBlock.isValid()) {
              nextMatch = startReg.match(previousBlock.text());
              if (nextMatch.hasMatch() && nextMatch.captured(1) == match.captured(1)) {
                startBlock = previousBlock;
              }
            }
          }
        }

        if (startBlock.isValid()) {
          // We found a block marker [startBlock, block]. Delete it.
          done = true;
          cursor.setPosition(startBlock.position());
          cursor.setPosition(block.position() + block.length() - 1, QTextCursor::KeepAnchor);
          cursor.deleteChar();
        }
      }
    }

    if (!done) {
      // Try to skip the end block marker if locate at one block before the end
      // block.
      auto nextBlock = block.next();
      if (nextBlock.isValid()) {
        auto match = endReg.match(nextBlock.text());
        if (match.hasMatch()) {
          done = true;
          cursor.setPosition(nextBlock.position());
          if (nextBlock.next().isValid()) {
            cursor.movePosition(QTextCursor::NextBlock);
          }
          cursor.movePosition(QTextCursor::EndOfBlock);
        }
      }
    }

    // Insert new block marker.
    if (!done) {
      done = true;
      // If current block is an empty block or containing only spaces, we insert
      // insert start marker right now. Otherwise, we insert at next block.
      auto indentationSpaces = TextUtils::fetchIndentationSpaces(currentText);
      if (indentationSpaces.size() == currentText.size()) {
        cursor.movePosition(QTextCursor::EndOfBlock);
        cursor.insertText(p_startMarker);

        TextEditUtils::insertBlock(cursor, false);
        cursor.insertText(indentationSpaces + p_endMarker);
      } else {
        indentationSpaces.clear();
        TextEditUtils::insertBlock(cursor, false);
        cursor.insertText(p_startMarker);
        TextEditUtils::insertBlock(cursor, false);
        cursor.insertText(p_endMarker);
      }

      switch (p_cursorPosition) {
      case CursorPosition::StartMarker:
        cursor.movePosition(QTextCursor::PreviousBlock);
        cursor.movePosition(QTextCursor::EndOfBlock);
        break;

      case CursorPosition::NewLinebetweenMarkers:
        TextEditUtils::insertBlock(cursor, true);
        if (!indentationSpaces.isEmpty()) {
          cursor.insertText(indentationSpaces);
        }
        break;

      case CursorPosition::EndMarker:
        break;
      }
    }
  }

  cursor.endEditBlock();
  p_edit->setTextCursor(cursor);
}

void MarkdownUtils::typeMath(VTextEdit *p_edit) {
  typeMarker(p_edit, QStringLiteral("$"), QStringLiteral("$"));
}

void MarkdownUtils::typeMathBlock(VTextEdit *p_edit) {
  typeBlockMarker(p_edit, QStringLiteral("$$"), QStringLiteral("$$"),
                  CursorPosition::NewLinebetweenMarkers);
}

void MarkdownUtils::typeQuote(VTextEdit *p_edit) {
  QuoteData data;
  doOnSelectedLinesOrCurrentLine(p_edit, &MarkdownUtils::insertQuote, &data);
}

bool MarkdownUtils::insertQuote(QTextCursor &p_cursor, const QTextBlock &p_block, void *p_data) {
  auto &data = *static_cast<QuoteData *>(p_data);
  p_cursor.setPosition(p_block.position());
  const auto text = p_block.text();
  if (data.m_isFirstLine) {
    // First line.
    data.m_isFirstLine = false;
    QRegularExpression reg(c_quoteRegExp);
    auto match = reg.match(text);
    if (match.hasMatch()) {
      // The first line is a quote already, so we need to un-quote them.
      data.m_insertQuote = false;
      TextEditUtils::selectBlockUnderCursor(p_cursor);
      p_cursor.insertText(QStringLiteral("%1%2").arg(match.captured(1), match.captured(2)));
    } else {
      // Quote them.
      data.m_insertQuote = true;
      int indentation = TextUtils::fetchIndentation(text);
      data.m_indentation = indentation;
      p_cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor, indentation);
      p_cursor.insertText(QStringLiteral("> "));
      p_cursor.movePosition(QTextCursor::EndOfBlock);
    }
  } else {
    if (data.m_insertQuote) {
      int indentation = TextUtils::fetchIndentation(text);
      indentation = qMin(indentation, data.m_indentation);
      p_cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor, indentation);
      if (indentation < data.m_indentation) {
        p_cursor.insertText(QString(data.m_indentation - indentation, ' '));
      }
      p_cursor.insertText(QStringLiteral("> "));
      p_cursor.movePosition(QTextCursor::EndOfBlock);
    } else {
      QRegularExpression reg(c_quoteRegExp);
      auto match = reg.match(text);
      if (match.hasMatch()) {
        TextEditUtils::selectBlockUnderCursor(p_cursor);
        p_cursor.insertText(QStringLiteral("%1%2").arg(match.captured(1), match.captured(2)));
      } else {
        // Need to un-quote it but it is not quoted yet. Ignore it.
        return false;
      }
    }
  }
  return true;
}

void MarkdownUtils::typeLink(VTextEdit *p_edit, const QString &p_linkText,
                             const QString &p_linkUrl) {
  p_edit->insertPlainText(QStringLiteral("[%1](%2)").arg(p_linkText, p_linkUrl));
}

QString MarkdownUtils::generateImageLink(const QString &p_title, const QString &p_url,
                                         const QString &p_altText, int p_width, int p_height) {
  QString scale;
  if (p_width > 0) {
    if (p_height > 0) {
      scale = QStringLiteral(" =%1x%2").arg(p_width).arg(p_height);
    } else {
      scale = QStringLiteral(" =%1x").arg(p_width);
    }
  } else if (p_height > 0) {
    scale = QStringLiteral(" =x%1").arg(p_height);
  }

  QString altText;
  if (!p_altText.isEmpty()) {
    altText = QStringLiteral(" \"%1\"").arg(p_altText);
  }

  return QStringLiteral("![%1](%2%3%4)").arg(p_title, p_url, altText, scale);
}

static bool markdownLinkCmp(const MarkdownLink &p_a, const MarkdownLink &p_b) {
  return p_a.m_urlInLinkPos > p_b.m_urlInLinkPos;
}

QVector<MarkdownLink> MarkdownUtils::fetchImagesFromMarkdownText(const QString &p_content,
                                                                 const QString &p_contentBasePath,
                                                                 MarkdownLink::TypeFlags p_flags) {
  QVector<MarkdownLink> images;

  const auto regions = fetchImageRegionsViaParser(p_content);
  QRegularExpression regExp(QRegularExpression::anchoredPattern(c_imageLinkRegExp));
  for (const auto &reg : regions) {
    QString linkText = p_content.mid(reg.m_startPos, reg.m_endPos - reg.m_startPos);
    auto match = regExp.match(linkText);
    if (!match.hasMatch()) {
      // Image links with reference format will not match.
      continue;
    }

    MarkdownLink link;
    link.m_urlInLink = match.captured(2).trimmed();
    ;
    link.m_urlInLinkPos =
        reg.m_startPos + linkText.indexOf(link.m_urlInLink, 4 + match.captured(1).size());

    QFileInfo info(linkUrlToPath(p_contentBasePath, link.m_urlInLink));
    if (info.exists()) {
      if (info.isNativePath()) {
        // Local file.
        link.m_path = QDir::cleanPath(info.absoluteFilePath());

        if (QDir::isRelativePath(link.m_urlInLink)) {
          if (pathContains(p_contentBasePath, link.m_path)) {
            link.m_type |= MarkdownLink::TypeFlag::LocalRelativeInternal;
          } else {
            link.m_type |= MarkdownLink::TypeFlag::LocalRelativeExternal;
          }
        } else {
          link.m_type |= MarkdownLink::TypeFlag::LocalAbsolute;
        }
      } else {
        link.m_type |= MarkdownLink::TypeFlag::QtResource;
        link.m_path = link.m_urlInLink;
      }
    } else {
      QUrl url(link.m_urlInLink);
      link.m_path = url.toString();
      link.m_type |= MarkdownLink::TypeFlag::Remote;
    }

    if (link.m_type & p_flags) {
      images.push_back(link);
    }
  }

  std::sort(images.begin(), images.end(), markdownLinkCmp);

  return images;
}

QVector<peg::ElementRegion> MarkdownUtils::fetchImageRegionsViaParser(const QString &p_content) {
  auto parserConfig = QSharedPointer<peg::PegParseConfig>::create();
  parserConfig->m_data = p_content.toUtf8();
  return peg::PegParser::parseImageRegions(parserConfig);
}

QString MarkdownUtils::relativePath(const QString &p_dir, const QString &p_path) {
  QDir dir(p_dir);
  return QDir::cleanPath(dir.relativeFilePath(p_path));
}

bool MarkdownUtils::pathContains(const QString &p_dir, const QString &p_path) {
  auto rel = relativePath(p_dir, p_path);
  if (rel.startsWith(QStringLiteral("../")) || rel == QStringLiteral("..")) {
    return false;
  }

  if (QFileInfo(rel).isAbsolute()) {
    return false;
  }

  return true;
}

MarkdownUtils::HeaderMatch MarkdownUtils::matchHeader(const QString &p_text) {
  QRegularExpression regExp(QRegularExpression::anchoredPattern(c_headerRegExp));
  auto match = regExp.match(p_text);
  if (!match.hasMatch()) {
    return HeaderMatch();
  } else {
    HeaderMatch headerMatch;
    headerMatch.m_matched = true;
    headerMatch.m_level = match.captured(1).length();
    headerMatch.m_spacesAfterMarker = match.captured(2).length();
    headerMatch.m_header = match.captured(3).trimmed();
    headerMatch.m_sequence = match.captured(4);
    headerMatch.m_spacesAfterSequence = match.captured(5).length();
    return headerMatch;
  }
}

bool MarkdownUtils::isTodoList(const QString &p_text, QChar &p_listMark, bool &p_empty) {
  if (p_text.isEmpty()) {
    return false;
  }

  QRegularExpression reg(c_todoListRegExp);
  auto match = reg.match(p_text);
  if (match.hasMatch()) {
    p_listMark = match.captured(2)[0];
    p_empty = match.captured(4).isEmpty();
    return true;
  }

  return false;
}

bool MarkdownUtils::isUnorderedList(const QString &p_text, QChar &p_listMark, bool &p_empty) {
  if (p_text.isEmpty()) {
    return false;
  }

  QRegularExpression reg(c_unorderedListRegExp);
  auto match = reg.match(p_text);
  if (match.hasMatch()) {
    p_listMark = match.captured(2)[0];
    p_empty = match.captured(3).isEmpty();
    return true;
  }

  return false;
}

bool MarkdownUtils::isOrderedList(const QString &p_text, QString &p_listNumber, bool &p_empty) {
  if (p_text.isEmpty()) {
    return false;
  }

  QRegularExpression reg(c_orderedListRegExp);
  auto match = reg.match(p_text);
  if (match.hasMatch()) {
    p_listNumber = match.captured(2);
    p_empty = match.captured(3).isEmpty();
    return true;
  }

  return false;
}

QString MarkdownUtils::setOrderedListNumber(QString p_text, int p_number) {
  Q_ASSERT(p_number > 0);
  QRegularExpression reg(c_orderedListRegExp);
  auto match = reg.match(p_text);
  if (match.hasMatch()) {
    const auto number = match.captured(2);
    if (number.toInt() == p_number) {
      return p_text;
    }
    p_text.replace(match.captured(1).size(), number.size(), QString::number(p_number));
  }
  return p_text;
}
