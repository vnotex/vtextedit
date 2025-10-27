#ifndef PREVIEWDATA_H
#define PREVIEWDATA_H

#include <QRgb>
#include <QSize>
#include <QString>
#include <QVector>

#include <vtextedit/global.h>
#include <vtextedit/vtextedit_export.h>

class QTextBlock;

namespace vte {
// Preview image data.
struct VTEXTEDIT_EXPORT PreviewImageData {
  PreviewImageData() = default;

  PreviewImageData(int p_startPos, int p_endPos, int p_padding, bool p_inline,
                   const QString &p_imageName, const QSize &p_imageSize,
                   const QRgb &p_backgroundColor);

  bool operator<(const PreviewImageData &a) const;

  bool operator==(const PreviewImageData &a) const;

  bool intersect(const PreviewImageData &a) const;

  bool contains(int p_positionInBlock) const;

  QString toString() const;

  // Start position of text corresponding to the image within block.
  int m_startPos = -1;

  // End position of text corresponding to the image within block.
  int m_endPos = -1;

  // Padding of the image. Only valid for block image.
  int m_padding = 0;

  // Whether it is inline image or block image.
  bool m_inline = false;

  // Image name in the resource manager.
  QString m_imageName;

  // Image size of the image. Cache for performance.
  QSize m_imageSize;

  // Forced background before drawing this image.
  QRgb m_backgroundColor = 0x0;
};

class VTEXTEDIT_EXPORT PreviewData {
public:
  enum Source { ImageLink, CodeBlock, MathBlock, MaxSource };

  PreviewData() = default;

  PreviewData(PreviewData::Source p_source, TimeStamp p_timeStamp, int p_startPos, int p_endPos,
              int p_padding, bool p_inline, const QString &p_imageName, const QSize &p_imageSize,
              const QRgb &p_backgroundColor);

  ~PreviewData();

  const PreviewImageData *getImageData() const;

  PreviewData::Source source() const;

  TimeStamp timeStamp() const;

private:
  // Source of this preview.
  Source m_source = Source::ImageLink;

  // Timestamp for this preview.
  TimeStamp m_timeStamp = 0;

  // Image data of this preview.
  PreviewImageData *m_imageData = nullptr;
};

class VTEXTEDIT_EXPORT BlockPreviewData {
public:
  BlockPreviewData() = default;

  ~BlockPreviewData();

  const QVector<PreviewData *> &getPreviewData() const;

  // Insert @p_data into previews, preserving the order.
  // Returns true if only timestamp is updated.
  bool insert(PreviewData *p_data);

  // Return true if there have obsolete preview being deleted.
  bool clearObsoletePreview(TimeStamp p_timeStamp, PreviewData::Source p_source);

  static QSharedPointer<BlockPreviewData> get(const QTextBlock &p_block);

private:
  // Sorted by m_startPos, with no two element's position intersected.
  QVector<PreviewData *> m_data;
};
} // namespace vte
#endif // PREVIEWDATA_H
