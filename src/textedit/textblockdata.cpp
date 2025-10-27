#include <vtextedit/textblockdata.h>

using namespace vte;

static const int s_braceDepthStateShift = 8;

TextBlockData::TextBlockData() {}

TextBlockData::~TextBlockData() {}

TextBlockData *TextBlockData::get(const QTextBlock &p_block) {
  if (!p_block.isValid()) {
    return nullptr;
  }

  auto data = static_cast<TextBlockData *>(p_block.userData());
  if (!data) {
    data = new TextBlockData();
    // QTextDocument will own the data object.
    const_cast<QTextBlock &>(p_block).setUserData(data);
  }

  return data;
}

KSyntaxHighlighting::State TextBlockData::getSyntaxState() const { return m_syntaxState; }

void TextBlockData::setSyntaxState(KSyntaxHighlighting::State p_state) { m_syntaxState = p_state; }

int TextBlockData::getFoldingIndent() const { return m_foldingIndent; }

void TextBlockData::setFoldingIndent(int p_indent) { m_foldingIndent = p_indent; }

int TextBlockData::getBraceDepth(const QTextBlock &p_block) {
  int state = p_block.userState();
  if (state == -1) {
    return 0;
  }
  return state >> s_braceDepthStateShift;
}

void TextBlockData::setBraceDepth(QTextBlock &p_block, int p_depth) {
  int state = p_block.userState();
  if (state == -1) {
    state = 0;
  }
  const int mask = (1 << s_braceDepthStateShift) - 1;
  state = state & mask;
  p_block.setUserState((p_depth << s_braceDepthStateShift) | state);
}

void TextBlockData::addBraceDepth(QTextBlock &p_block, int p_delta) {
  if (p_delta != 0) {
    setBraceDepth(p_block, getBraceDepth(p_block) + p_delta);
  }
}

int TextBlockData::getFoldingIndent(const QTextBlock &p_block) {
  return get(p_block)->getFoldingIndent();
}

void TextBlockData::setFoldingIndent(const QTextBlock &p_block, int p_indent) {
  get(p_block)->setFoldingIndent(p_indent);
}

void TextBlockData::clearFoldings() { m_foldings.clear(); }

void TextBlockData::addFolding(int p_offset, int p_value) {
  m_foldings.push_back(Folding(p_offset, p_value));
}

const QVector<TextBlockData::Folding> &TextBlockData::getFoldings() const { return m_foldings; }

bool TextBlockData::isMarkedAsFoldingStart() const { return m_markedAsFoldingStart; }

void TextBlockData::setMarkedAsFoldingStart(bool p_set) { m_markedAsFoldingStart = p_set; }

const QSharedPointer<PegHighlightBlockData> &TextBlockData::getPegHighlightBlockData() const {
  return m_pegHighlightData;
}

void TextBlockData::setPegHighlightBlockData(const QSharedPointer<PegHighlightBlockData> &p_data) {
  m_pegHighlightData = p_data;
}

const QSharedPointer<BlockLayoutData> &TextBlockData::getBlockLayoutData() const {
  return m_blockLayoutData;
}

void TextBlockData::setBlockLayoutData(const QSharedPointer<BlockLayoutData> &p_data) {
  m_blockLayoutData = p_data;
}

const QSharedPointer<BlockPreviewData> &TextBlockData::getBlockPreviewData() const {
  return m_blockPreviewData;
}

void TextBlockData::setBlockPreviewData(const QSharedPointer<BlockPreviewData> &p_data) {
  m_blockPreviewData = p_data;
}

const QSharedPointer<BlockSpellCheckData> &TextBlockData::getBlockSpellCheckData() const {
  return m_blockSpellCheckData;
}

void TextBlockData::setBlockSpellCheckData(const QSharedPointer<BlockSpellCheckData> &p_data) {
  m_blockSpellCheckData = p_data;
}
