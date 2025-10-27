#ifndef VTEXTEDIT_TEXTBLOCKDATA_H
#define VTEXTEDIT_TEXTBLOCKDATA_H

#include "vtextedit_export.h"

#include <QSharedPointer>
#include <QTextBlockUserData>
#include <QVector>

#include <State>

namespace vte {
class PegHighlightBlockData;
struct BlockLayoutData;
class BlockPreviewData;
struct BlockSpellCheckData;

class VTEXTEDIT_EXPORT TextBlockData : public QTextBlockUserData {
public:
  struct Folding {
    Folding(int p_offset, int p_value) : m_offset(p_offset), m_value(p_value) {}

    Folding() = default;

    bool isOpen() const {
      Q_ASSERT(m_value != 0);
      return m_value > 0;
    }

    int m_offset = 0;

    // Positive value means starting a folding, while
    // negative value means ending a folding.
    // The absolute value equals to the folding id.
    int m_value = 0;
  };

  ~TextBlockData();

  KSyntaxHighlighting::State getSyntaxState() const;
  void setSyntaxState(KSyntaxHighlighting::State p_state);

  int getFoldingIndent() const;
  void setFoldingIndent(int p_indent);

  void clearFoldings();

  void addFolding(int p_offset, int p_value);

  const QVector<TextBlockData::Folding> &getFoldings() const;

  bool isMarkedAsFoldingStart() const;
  void setMarkedAsFoldingStart(bool p_set);

  const QSharedPointer<PegHighlightBlockData> &getPegHighlightBlockData() const;
  void setPegHighlightBlockData(const QSharedPointer<PegHighlightBlockData> &p_data);

  const QSharedPointer<BlockLayoutData> &getBlockLayoutData() const;
  void setBlockLayoutData(const QSharedPointer<BlockLayoutData> &p_data);

  const QSharedPointer<BlockPreviewData> &getBlockPreviewData() const;
  void setBlockPreviewData(const QSharedPointer<BlockPreviewData> &p_data);

  const QSharedPointer<BlockSpellCheckData> &getBlockSpellCheckData() const;
  void setBlockSpellCheckData(const QSharedPointer<BlockSpellCheckData> &p_data);

  // Get or create a TextBlockData for @p_block.
  static TextBlockData *get(const QTextBlock &p_block);

  // Brace depth.
  static int getBraceDepth(const QTextBlock &p_block);
  static void setBraceDepth(QTextBlock &p_block, int p_depth);
  static void addBraceDepth(QTextBlock &p_block, int p_delta);

  // Folding indent.
  static int getFoldingIndent(const QTextBlock &p_block);
  static void setFoldingIndent(const QTextBlock &p_block, int p_indent);

private:
  TextBlockData();

  // Syntax state of previous block.
  KSyntaxHighlighting::State m_syntaxState;

  int m_foldingIndent = 0;

  // Syntax foldings of this block.
  QVector<Folding> m_foldings;

  // Whether marked as syntax folding start.
  bool m_markedAsFoldingStart = false;

  // Data for PegMarkdownHighlighter.
  QSharedPointer<PegHighlightBlockData> m_pegHighlightData;

  // Layout data of this block when using TextDocumentLayout.
  QSharedPointer<BlockLayoutData> m_blockLayoutData;

  // Preview data of this block.
  QSharedPointer<BlockPreviewData> m_blockPreviewData;

  // Misspelling words of this block.
  QSharedPointer<BlockSpellCheckData> m_blockSpellCheckData;
};
} // namespace vte

#endif
