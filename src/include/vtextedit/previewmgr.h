#ifndef PREVIEWMGR_H
#define PREVIEWMGR_H

#include <QDebug>
#include <QHash>
#include <QObject>
#include <QPair>
#include <QPixmap>
#include <QSharedPointer>
#include <QString>
#include <QTextBlock>
#include <QVector>

#include "vtextedit_export.h"

#include <vtextedit/global.h>
#include <vtextedit/markdownhighlighterdata.h>
#include <vtextedit/orderedintset.h>
#include <vtextedit/previewdata.h>

class QTextDocument;

namespace vte {
class NetworkAccess;
struct NetworkReply;
class DocumentResourceMgr;

struct VTEXTEDIT_EXPORT PreviewItem {
  void clear() {
    m_startPos = m_endPos = m_blockPos = m_blockNumber = -1;
    m_padding = 0;
    m_image = QPixmap();
    m_name.clear();
    m_backgroundColor = 0;
    m_isBlockwise = false;
  };

  int m_startPos = -1;

  int m_endPos = -1;

  // Position of this block.
  int m_blockPos = -1;

  int m_blockNumber = -1;

  // Left padding of this block in pixels.
  int m_padding = 0;

  QPixmap m_image;

  // If @m_name are the same, then they are the same imges.
  QString m_name;

  // If not empty, we should draw a background before drawing this image.
  QRgb m_backgroundColor = 0x0;

  // Whether it is an image block.
  bool m_isBlockwise = false;
};

class PreviewMgrInterface {
public:
  virtual ~PreviewMgrInterface() {}

  virtual QTextDocument *document() const = 0;

  virtual int tabStopDistance() const = 0;

  virtual const QString &basePath() const = 0;

  virtual DocumentResourceMgr *documentResourceMgr() const = 0;

  virtual qreal scaleFactor() const = 0;

  // Add @p_blockNumber as a possible preview block.
  virtual void addPossiblePreviewBlock(int p_blockNumber) = 0;

  virtual const QSet<int> &getPossiblePreviewBlocks() const = 0;

  virtual void clearPossiblePreviewBlocks(const QVector<int> &p_blocksToClear) = 0;

  virtual void relayout(const OrderedIntSet &p_blocks) = 0;

  virtual void ensureCursorVisible() = 0;
};

// Manage inplace preview.
class VTEXTEDIT_EXPORT PreviewMgr : public QObject {
  Q_OBJECT
public:
  PreviewMgr(PreviewMgrInterface *p_interface, QObject *p_parent = nullptr);

  void setPreviewEnabled(PreviewData::Source p_source, bool p_enabled);

  void setPreviewEnabled(bool p_enabled);

  // Refresh all the preview.
  void refreshPreview();

  // Clear all the preview.
  void clearPreview();

  // Hand over the bytes of an image that the application already has in memory
  // (typically the payload it just uploaded to an image host), so that the
  // in-place preview of @p_url does not have to download it back.
  //
  // The key is normalized exactly like a link destination is, so pass the URL as
  // it will be spelled in the document. A payload that does not decode into an
  // image is dropped silently.
  //
  // This is a small bounded hand-off buffer, not a cache.
  void seedImageData(const QString &p_url, const QByteArray &p_data);

  // Helper function to export outside.
  static int calculateBlockMargin(const QTextBlock &p_block, int p_tabStopDistance);

public slots:
  void updateImageLinks(const QVector<md::ImageLinkInfo> &p_links);

  void updateCodeBlocks(const QVector<QSharedPointer<PreviewItem>> &p_items);

  void updateMathBlocks(const QVector<QSharedPointer<PreviewItem>> &p_items);

  // Check @p_blocks to see if there is any obsolete preview and clear them
  // if there is any.
  void checkBlocksForObsoletePreview(const QList<int> &p_blocks);

signals:
  // Request highlighter to update image links.
  void requestUpdateImageLinks();

  void requestUpdateCodeBlocks();

  void requestUpdateMathBlocks();

private slots:
  // Non-local image downloaded for preview.
  void imageDownloaded(const NetworkReply &p_data, const QString &p_url);

private:
  // Data of one single preview source.
  struct PreviewSourceData {
    bool m_enabled = false;

    TimeStamp m_timeStamp = 0;

    // Images inserted in the document resource manager.
    QHash<QString, TimeStamp> m_images;
  };

  struct ImageLink {
    ImageLink() = default;

    ImageLink(int p_startPos, int p_endPos, int p_blockPos, int p_blockNumber, int p_padding)
        : m_startPos(p_startPos), m_endPos(p_endPos), m_blockPos(p_blockPos),
          m_blockNumber(p_blockNumber), m_padding(p_padding), m_isBlockwise(false) {}

    friend QDebug operator<<(QDebug p_debug, const ImageLink &p_link) {
      p_debug << "ImageLink [" << p_link.m_startPos << p_link.m_endPos << ")"
              << "shortUrl" << p_link.m_linkShortUrl << "url" << p_link.m_linkUrl << "size"
              << p_link.m_width << "x" << p_link.m_height;
      return p_debug;
    }

    int m_startPos = -1;

    int m_endPos = -1;

    // Position of this block.
    int m_blockPos = -1;

    int m_blockNumber = -1;

    // Left padding of this block in pixels.
    int m_padding = 0;

    // Short URL within the () of ![]().
    // Used as the ID of the image.
    QString m_linkShortUrl;

    // Full URL of the link.
    QString m_linkUrl;

    // Declared size from the `=WxH` extension. 0 means unspecified for that
    // axis, in which case MarkdownUtils::scaleImage() keeps the aspect ratio.
    int m_width = 0;

    int m_height = 0;

    // Whether it is an image block.
    //
    // Deliberately computed here rather than taken from the walker's
    // m_standalone: this needs the live QTextDocument, its font metrics and the
    // tab stop distance, none of which the walker has. The two are kept
    // equivalent by testImageStandaloneMatchesPaintedPath().
    bool m_isBlockwise = false;
  };

  struct UrlImageData {
    UrlImageData(const QString &p_name, int p_width, int p_height)
        : m_name(p_name), m_width(p_width), m_height(p_height) {}

    QString m_name;

    int m_width = 0;

    int m_height = 0;
  };

  void previewImageLinks(TimeStamp p_timeStamp, const QVector<md::ImageLinkInfo> &p_links);

  // Join the parsed image links with the layout information only the live
  // document can supply: which blocks they occupy, the block's left padding,
  // and whether they stand alone on their line.
  void buildImageLinksForLayout(const QVector<md::ImageLinkInfo> &p_links,
                                QVector<ImageLink> &p_imageLinks);

  void updateBlockPreview(TimeStamp p_timeStamp, const QVector<ImageLink> &p_imageLinks,
                          OrderedIntSet &p_affectedBlocks);

  void updateBlockPreview(TimeStamp p_timeStamp, PreviewData::Source p_source,
                          const QVector<QSharedPointer<PreviewItem>> &p_items,
                          OrderedIntSet &p_affectedBlocks);

  QTextDocument *document() const;

  QSize imageResourceSize(const QString &p_name);

  // Get the name of the image in the resource manager.
  // Will add the image to the resource manager if not exists.
  // Returns empty if fail to add the image to the resource manager.
  QString imageResourceName(const ImageLink &p_link);

  QString imageResourceNameForSource(PreviewData::Source p_source, const PreviewItem &p_image);

  // Index of @p_normalizedPath in m_seededImages, or -1.
  int indexOfSeededImage(const QString &p_normalizedPath) const;

  void clearBlockObsoletePreview(TimeStamp p_timeStamp, PreviewData::Source p_source,
                                 OrderedIntSet &p_affectedBlocks);

  void clearObsoleteImages(TimeStamp p_timeStamp, PreviewData::Source p_source);

  void relayout(const OrderedIntSet &p_blocks);

  NetworkAccess *downloader();

  bool isAnyPreviewEnabled() const;

  void updatePreviewSource(PreviewData::Source p_source,
                           const QVector<QSharedPointer<PreviewItem>> &p_items);

  PreviewMgrInterface *m_interface = nullptr;

  QVector<PreviewSourceData> m_previewData;

  // Managed by QObject.
  NetworkAccess *m_downloader = nullptr;

  // Map from URL to the pending resource entries for that URL.
  // Used for downloading images.
  //
  // One URL may be pending at several declared sizes at once (`=500x` and
  // `=250x` in the same document), and each of those is a separate resource. A
  // single-value map would let the second insert overwrite the first, and the
  // erase on completion would then discard the survivor.
  QHash<QString, QVector<QSharedPointer<UrlImageData>>> m_urlMap;

  // Bounded, insertion-ordered hand-off buffer of images supplied by the
  // application through seedImageData(). Keyed by the normalized link path.
  QVector<QPair<QString, QPixmap>> m_seededImages;

  // Maximum number of entries kept in m_seededImages (FIFO eviction).
  static const int c_maxSeededImages = 4;
};
} // namespace vte
#endif // PREVIEWMGR_H
