#include <vtextedit/previewdata.h>

#include <vtextedit/textblockdata.h>

using namespace vte;

PreviewImageData::PreviewImageData(int p_startPos, int p_endPos, int p_padding, bool p_inline,
                                   const QString &p_imageName, const QSize &p_imageSize,
                                   const QRgb &p_backgroundColor)
    : m_startPos(p_startPos), m_endPos(p_endPos), m_padding(p_padding), m_inline(p_inline),
      m_imageName(p_imageName), m_imageSize(p_imageSize), m_backgroundColor(p_backgroundColor) {}

bool PreviewImageData::operator<(const PreviewImageData &a) const {
  return m_endPos <= a.m_startPos;
}

bool PreviewImageData::operator==(const PreviewImageData &a) const {
  return m_startPos == a.m_startPos && m_endPos == a.m_endPos && m_padding == a.m_padding &&
         m_inline == a.m_inline && m_imageName == a.m_imageName && m_imageSize == a.m_imageSize &&
         m_backgroundColor == a.m_backgroundColor;
}

bool PreviewImageData::intersect(const PreviewImageData &a) const {
  return !(m_endPos <= a.m_startPos || m_startPos >= a.m_endPos);
}

bool PreviewImageData::contains(int p_positionInBlock) const {
  return p_positionInBlock >= m_startPos && p_positionInBlock < m_endPos;
}

QString PreviewImageData::toString() const {
  return QStringLiteral("previewed image (%1): [%2, %3) padding %4 inline %5 "
                        "(%6,%7) bg(%8)")
      .arg(m_imageName)
      .arg(m_startPos)
      .arg(m_endPos)
      .arg(m_padding)
      .arg(m_inline)
      .arg(m_imageSize.width())
      .arg(m_imageSize.height())
      .arg(m_backgroundColor);
}

PreviewData::PreviewData(PreviewData::Source p_source, TimeStamp p_timeStamp, int p_startPos,
                         int p_endPos, int p_padding, bool p_inline, const QString &p_imageName,
                         const QSize &p_imageSize, const QRgb &p_backgroundColor)
    : m_source(p_source), m_timeStamp(p_timeStamp),
      m_imageData(new PreviewImageData(p_startPos, p_endPos, p_padding, p_inline, p_imageName,
                                       p_imageSize, p_backgroundColor)) {}

PreviewData::~PreviewData() { delete m_imageData; }

const PreviewImageData *PreviewData::getImageData() const { return m_imageData; }

PreviewData::Source PreviewData::source() const { return m_source; }

TimeStamp PreviewData::timeStamp() const { return m_timeStamp; }

BlockPreviewData::~BlockPreviewData() {
  for (auto *data : m_data) {
    delete data;
  }
}

QSharedPointer<BlockPreviewData> BlockPreviewData::get(const QTextBlock &p_block) {
  auto blockData = TextBlockData::get(p_block);
  auto data = blockData->getBlockPreviewData();
  if (!data) {
    data.reset(new BlockPreviewData());
    blockData->setBlockPreviewData(data);
  }
  return data;
}

const QVector<PreviewData *> &BlockPreviewData::getPreviewData() const { return m_data; }

bool BlockPreviewData::insert(PreviewData *p_data) {
  bool tsUpdated = false;
  bool inserted = false;
  auto newImageData = p_data->getImageData();
  for (auto it = m_data.begin(); it != m_data.end();) {
    auto ele = *it;
    auto eleImageData = ele->getImageData();
    if (*newImageData < *eleImageData) {
      // Insert p_data here.
      m_data.insert(it, p_data);
      inserted = true;
      break;
    } else if (*newImageData == *eleImageData) {
      // Update the timestamp.
      delete ele;
      *it = p_data;
      inserted = true;
      tsUpdated = true;
      break;
    } else if (newImageData->intersect(*eleImageData)) {
      // Two preview intersect.
      delete ele;
      it = m_data.erase(it);
    } else {
      ++it;
    }
  }

  if (!inserted) {
    // Append it.
    m_data.append(p_data);
  }

  return tsUpdated;
}

bool BlockPreviewData::clearObsoletePreview(TimeStamp p_timeStamp, PreviewData::Source p_source) {
  bool deleted = false;
  for (auto it = m_data.begin(); it != m_data.end();) {
    auto ele = *it;
    if (ele->source() == p_source && ele->timeStamp() != p_timeStamp) {
      // Remove it.
      delete ele;
      it = m_data.erase(it);
      deleted = true;
    } else {
      ++it;
    }
  }

  return deleted;
}
