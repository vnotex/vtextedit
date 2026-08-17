#include <vtextedit/previewmgr.h>

#include <QDebug>
#include <QDir>
#include <QTextDocument>
#include <QTextLayout>
#include <QUrl>
#include <QVector>

#include <vtextedit/markdownutils.h>
#include <vtextedit/texteditutils.h>
#include <vtextedit/textutils.h>

#include "../utils/networkutils.h"
#include "documentresourcemgr.h"

using namespace vte;

typedef PreviewData::Source Source;

// An upper bound on a declared `=WxH` size, in logical pixels per axis.
//
// The declared size is attacker-supplied in the sense that it comes from the
// document, and it is multiplied again by the device pixel ratio before
// allocation: `![](a.png =99999x)` at a scale factor of 2 would ask for a
// ~200000px-wide pixmap. v3 honored the size without a bound; we do not.
static const int c_maxPreviewImageDimension = 4096;

static int clampPreviewDimension(int p_value) {
  return qBound(0, p_value, c_maxPreviewImageDimension);
}

// Identity of a preview resource: the destination plus the declared size.
//
// Built by plain concatenation rather than chained QString::arg(). A
// destination keeps its percent-encoding, so `a%2F.png` contains what arg()
// reads as the `%2` placeholder, and a second arg() would substitute into it --
// making the composition non-injective and letting two distinct images share
// one cached pixmap. The URL is length-prefixed so that no destination
// containing `_` can imitate the size suffix either.
static QString previewResourceName(const QString &p_shortUrl, int p_width, int p_height) {
  return QString::number(p_shortUrl.size()) + QLatin1Char(':') + p_shortUrl + QLatin1Char('_') +
         QString::number(p_width) + QLatin1Char('_') + QString::number(p_height);
}

PreviewMgr::PreviewMgr(PreviewMgrInterface *p_interface, QObject *p_parent)
    : QObject(p_parent), m_interface(p_interface), m_previewData(Source::MaxSource) {}

void PreviewMgr::setPreviewEnabled(Source p_source, bool p_enabled) {
  auto &data = m_previewData[p_source];
  if (data.m_enabled != p_enabled) {
    data.m_enabled = p_enabled;

    if (!isAnyPreviewEnabled()) {
      clearPreview();
    } else {
      refreshPreview();
    }
  }
}

void PreviewMgr::setPreviewEnabled(bool p_enabled) {
  bool changed = false;
  for (int i = 0; i < m_previewData.size(); ++i) {
    auto &data = m_previewData[i];
    if (data.m_enabled != p_enabled) {
      changed = true;
      data.m_enabled = p_enabled;
    }
  }

  if (changed) {
    if (!p_enabled) {
      clearPreview();
    } else {
      refreshPreview();
    }
  }
}

void PreviewMgr::updateImageLinks(const QVector<md::ImageLinkInfo> &p_links) {
  auto &data = m_previewData[Source::ImageLink];
  if (!data.m_enabled) {
    return;
  }

  auto ts = ++data.m_timeStamp;
  previewImageLinks(ts, p_links);
}

void PreviewMgr::previewImageLinks(TimeStamp p_timeStamp,
                                   const QVector<md::ImageLinkInfo> &p_links) {
  QVector<ImageLink> imageLinks;
  buildImageLinksForLayout(p_links, imageLinks);

  OrderedIntSet affectedBlocks;

  updateBlockPreview(p_timeStamp, imageLinks, affectedBlocks);

  clearBlockObsoletePreview(p_timeStamp, Source::ImageLink, affectedBlocks);

  clearObsoleteImages(p_timeStamp, Source::ImageLink);

  relayout(affectedBlocks);
}

void PreviewMgr::buildImageLinksForLayout(const QVector<md::ImageLinkInfo> &p_links,
                                          QVector<ImageLink> &p_imageLinks) {
  p_imageLinks.clear();
  if (p_links.isEmpty()) {
    return;
  }

  p_imageLinks.reserve(p_links.size());
  auto doc = document();
  for (const auto &info : p_links) {
    const auto &reg = info.m_region;
    QTextBlock firstBlock = doc->findBlock(reg.m_startPos);
    if (!firstBlock.isValid()) {
      continue;
    }

    // Image link may cross multiple regions.
    QTextBlock lastBlock = doc->findBlock(reg.m_endPos - 1);
    if (!lastBlock.isValid()) {
      continue;
    }

    int firstBlockStart = firstBlock.position();
    int lastBlockStart = lastBlock.position();
    int lastBlockEnd = lastBlockStart + lastBlock.length() - 1;

    // Preview the image at the last block.
    ImageLink link(qMax(reg.m_startPos, lastBlockStart), reg.m_endPos, lastBlockStart,
                   lastBlock.blockNumber(),
                   calculateBlockMargin(firstBlock, m_interface->tabStopDistance()));
    if ((reg.m_startPos == firstBlockStart ||
         TextUtils::isSpace(firstBlock.text(), 0, reg.m_startPos - firstBlockStart)) &&
        (reg.m_endPos == lastBlockEnd ||
         TextUtils::isSpace(lastBlock.text(), reg.m_endPos - lastBlockStart,
                            lastBlockEnd - lastBlockStart))) {
      // Image block.
      link.m_isBlockwise = true;
    } else {
      // Inline image.
      link.m_isBlockwise = false;
    }

    // The destination arrives already resolved by cmark, so there is nothing
    // left to parse here. If it is spelled with `\` instead of `/`, skip it to
    // align with read mode -- a letter is not escapable in a Markdown
    // destination, so `vx_images\a.png` really does still contain a backslash
    // after cmark's unescaping, while `a\_b.png` correctly resolves to
    // `a_b.png` and is previewed.
    if (info.m_destination.isEmpty()) {
      continue;
    }
    if (info.m_destination.contains(QLatin1Char('\\'))) {
      qWarning() << "skipped local image with `\\` in path (use `/` instead)" << info.m_destination;
      continue;
    }

    link.m_linkShortUrl = info.m_destination;
    link.m_linkUrl = MarkdownUtils::linkUrlToPath(m_interface->basePath(), info.m_destination);
    link.m_width = clampPreviewDimension(info.m_width);
    link.m_height = clampPreviewDimension(info.m_height);
    if (link.m_linkUrl.isEmpty()) {
      continue;
    }

    p_imageLinks.append(link);
  }
}

QTextDocument *PreviewMgr::document() const { return m_interface->document(); }

QSize PreviewMgr::imageResourceSize(const QString &p_name) {
  auto resourceMgr = m_interface->documentResourceMgr();
  const QPixmap *img = resourceMgr->findImage(p_name);
  if (img) {
    // If the paint device's DevicePixelRatio is larger than 1, the editor will
    // scale the drawing automatically. So to make the preview image clear, we
    // scale the source image and draw it into a 1/2 rect. For a 100*50 image,
    // if we draw it in 100*50 rect, it will be zoom in like 200*100. We scale
    // the image first to 200*100, then draw it in 100*50 rect.
    return img->size() / m_interface->scaleFactor();
  }

  return QSize();
}

void PreviewMgr::updateBlockPreview(TimeStamp p_timeStamp, const QVector<ImageLink> &p_imageLinks,
                                    OrderedIntSet &p_affectedBlocks) {
  auto doc = document();
  for (const auto &link : p_imageLinks) {
    QTextBlock block = doc->findBlockByNumber(link.m_blockNumber);
    if (!block.isValid()) {
      continue;
    }

    QString name = imageResourceName(link);
    if (name.isEmpty()) {
      continue;
    }

    m_previewData[Source::ImageLink].m_images.insert(name, p_timeStamp);

    auto previewData = BlockPreviewData::get(block);
    auto data = new PreviewData(Source::ImageLink, p_timeStamp, link.m_startPos - link.m_blockPos,
                                link.m_endPos - link.m_blockPos, link.m_padding,
                                !link.m_isBlockwise, name, imageResourceSize(name), 0x0);
    bool tsUpdated = previewData->insert(data);
    if (!tsUpdated) {
      // No need to relayout the block if only timestamp is updated.
      p_affectedBlocks.insert(link.m_blockNumber, QMapDummyValue());
      m_interface->addPossiblePreviewBlock(link.m_blockNumber);
    }
  }
}

QString PreviewMgr::imageResourceName(const ImageLink &p_link) {
  // The declared size is part of the resource identity: the same file at
  // `=500x` and at `=250x` are two different pixmaps, and keying on the URL
  // alone would make whichever was rendered first win for both.
  QString name = previewResourceName(p_link.m_linkShortUrl, p_link.m_width, p_link.m_height);
  auto resourceMgr = m_interface->documentResourceMgr();
  if (resourceMgr->containsImage(name)) {
    return name;
  }

  // Add it to the resource.
  QPixmap image;
  QString imgPath = p_link.m_linkUrl;
  if (QFileInfo::exists(imgPath)) {
    // Local file.
    // Sometimes the suffix of the image may mislead the codec. Directly load
    // from the data and then load from file path.
    QFile file(imgPath);
    if (file.open(QIODevice::ReadOnly)) {
      image.loadFromData(file.readAll());
    }
    if (image.isNull()) {
      image = QPixmap(imgPath);
    }
    if (image.isNull()) {
      qWarning() << "failed to load local image for preview" << imgPath;
      return QString();
    }
  } else {
    // URL. Try to download it.
    // qrc:// files will touch this path.
    auto &pending = m_urlMap[imgPath];
    // Only the first pending entry for a URL issues a request; the rest ride
    // along on the same download and are all served when it completes.
    const bool alreadyRequested = !pending.isEmpty();
    bool known = false;
    for (const auto &entry : pending) {
      if (entry->m_name == name) {
        known = true;
        break;
      }
    }
    if (!known) {
      pending.append(
          QSharedPointer<UrlImageData>(new UrlImageData(name, p_link.m_width, p_link.m_height)));
    }
    if (!alreadyRequested) {
      downloader()->requestAsync(imgPath);
    }
    return QString();
  }

  resourceMgr->addImage(name, MarkdownUtils::scaleImage(image, p_link.m_width, p_link.m_height,
                                                        m_interface->scaleFactor()));
  return name;
}

QString PreviewMgr::imageResourceNameForSource(Source p_source, const PreviewItem &p_image) {
  QString name = QString::number((int)p_source) + "_" + p_image.m_name;
  auto resourceMgr = m_interface->documentResourceMgr();
  if (resourceMgr->containsImage(name)) {
    return name;
  }

  // Add it to the resource.
  if (p_image.m_image.isNull()) {
    return QString();
  }

  resourceMgr->addImage(name, p_image.m_image);
  return name;
}

void PreviewMgr::clearBlockObsoletePreview(TimeStamp p_timeStamp, Source p_source,
                                           OrderedIntSet &p_affectedBlocks) {
  auto doc = document();
  QVector<int> obsoleteBlocks;
  const QSet<int> &blocks = m_interface->getPossiblePreviewBlocks();
  for (auto blockNum : blocks) {
    QTextBlock block = doc->findBlockByNumber(blockNum);
    if (!block.isValid()) {
      obsoleteBlocks.append(blockNum);
      continue;
    }

    auto previewData = BlockPreviewData::get(block);
    if (previewData->clearObsoletePreview(p_timeStamp, p_source)) {
      p_affectedBlocks.insert(blockNum, QMapDummyValue());
    }

    if (previewData->getPreviewData().isEmpty()) {
      obsoleteBlocks.append(blockNum);
    }
  }

  m_interface->clearPossiblePreviewBlocks(obsoleteBlocks);
}

void PreviewMgr::clearObsoleteImages(TimeStamp p_timeStamp, Source p_source) {
  auto resourceMgr = m_interface->documentResourceMgr();
  auto &images = m_previewData[p_source].m_images;
  for (auto it = images.begin(); it != images.end();) {
    if (it.value() < p_timeStamp) {
      resourceMgr->removeImage(it.key());
      it = images.erase(it);
    } else {
      ++it;
    }
  }
}

void PreviewMgr::relayout(const OrderedIntSet &p_blocks) {
  if (p_blocks.isEmpty()) {
    return;
  }

  m_interface->relayout(p_blocks);

  // Make cursor visible.
  m_interface->ensureCursorVisible();
}

NetworkAccess *PreviewMgr::downloader() {
  if (!m_downloader) {
    m_downloader = new NetworkAccess(this);
    connect(m_downloader, &NetworkAccess::requestFinished, this, &PreviewMgr::imageDownloaded);
  }

  return m_downloader;
}

void PreviewMgr::imageDownloaded(const NetworkReply &p_data, const QString &p_url) {
  // Retire the pending entry FIRST, whatever happens next. `imageResourceName()`
  // treats a non-empty pending vector as "a request is already in flight" and
  // issues no new one, so bailing out before the erase would strand this URL
  // for the lifetime of the editor: previews disabled mid-download would mean
  // the image never appears again, even after they are re-enabled.
  auto it = m_urlMap.find(p_url);
  if (it == m_urlMap.end()) {
    return;
  }

  const auto pending = it.value();
  m_urlMap.erase(it);

  // Mainly used for image link preview.
  if (!m_previewData[Source::ImageLink].m_enabled) {
    return;
  }

  QPixmap image;
  image.loadFromData(p_data.m_data);
  if (image.isNull()) {
    return;
  }

  auto resourceMgr = m_interface->documentResourceMgr();
  bool added = false;
  // One download may serve several declared sizes of the same URL.
  for (const auto &data : pending) {
    if (data->m_name.isEmpty() || resourceMgr->containsImage(data->m_name)) {
      continue;
    }
    resourceMgr->addImage(data->m_name,
                          MarkdownUtils::scaleImage(image, data->m_width, data->m_height,
                                                    m_interface->scaleFactor()));
    added = true;
  }

  if (added) {
    emit requestUpdateImageLinks();
  }
}

bool PreviewMgr::isAnyPreviewEnabled() const {
  for (int i = 0; i < m_previewData.size(); ++i) {
    if (m_previewData[i].m_enabled) {
      return true;
    }
  }

  return false;
}

void PreviewMgr::refreshPreview() {
  if (!isAnyPreviewEnabled()) {
    return;
  }

  clearPreview();

  emit requestUpdateImageLinks();
  emit requestUpdateCodeBlocks();
  emit requestUpdateMathBlocks();
}

void PreviewMgr::clearPreview() {
  OrderedIntSet affectedBlocks;
  for (int i = 0; i < m_previewData.size(); ++i) {
    auto ts = ++m_previewData[i].m_timeStamp;
    clearBlockObsoletePreview(ts, static_cast<Source>(i), affectedBlocks);
    clearObsoleteImages(ts, static_cast<Source>(i));
  }

  relayout(affectedBlocks);
}

void PreviewMgr::checkBlocksForObsoletePreview(const QList<int> &p_blocks) {
  if (p_blocks.isEmpty()) {
    return;
  }

  auto doc = document();
  OrderedIntSet affectedBlocks;
  for (auto blockNum : p_blocks) {
    QTextBlock block = doc->findBlockByNumber(blockNum);
    if (!block.isValid()) {
      continue;
    }

    auto previewData = BlockPreviewData::get(block);
    if (previewData->getPreviewData().isEmpty()) {
      continue;
    }

    for (int i = 0; i < (int)Source::MaxSource; ++i) {
      if (previewData->getPreviewData().isEmpty()) {
        break;
      }

      auto ps = static_cast<Source>(i);
      if (previewData->clearObsoletePreview(m_previewData[i].m_timeStamp, ps)) {
        affectedBlocks.insert(blockNum, QMapDummyValue());
      }
    }
  }

  relayout(affectedBlocks);
}

void PreviewMgr::updateCodeBlocks(const QVector<QSharedPointer<PreviewItem>> &p_items) {
  updatePreviewSource(Source::CodeBlock, p_items);
}

void PreviewMgr::updateBlockPreview(TimeStamp p_timeStamp, Source p_source,
                                    const QVector<QSharedPointer<PreviewItem>> &p_items,
                                    OrderedIntSet &p_affectedBlocks) {
  auto doc = document();
  for (const auto &item : p_items) {
    if (item.isNull()) {
      continue;
    }

    QTextBlock block = doc->findBlockByNumber(item->m_blockNumber);
    if (!block.isValid()) {
      continue;
    }

    QString name = imageResourceNameForSource(p_source, *item);
    if (name.isEmpty()) {
      continue;
    }

    m_previewData[p_source].m_images.insert(name, p_timeStamp);

    auto previewData = BlockPreviewData::get(block);
    auto data =
        new PreviewData(p_source, p_timeStamp, item->m_startPos - item->m_blockPos,
                        item->m_endPos - item->m_blockPos, item->m_padding, !item->m_isBlockwise,
                        name, imageResourceSize(name), item->m_backgroundColor);
    bool tsUpdated = previewData->insert(data);
    if (!tsUpdated) {
      // No need to relayout the block if only timestamp is updated.
      p_affectedBlocks.insert(item->m_blockNumber, QMapDummyValue());
      m_interface->addPossiblePreviewBlock(item->m_blockNumber);
    }
  }
}

void PreviewMgr::updateMathBlocks(const QVector<QSharedPointer<PreviewItem>> &p_items) {
  updatePreviewSource(Source::MathBlock, p_items);
}

void PreviewMgr::updatePreviewSource(PreviewData::Source p_source,
                                     const QVector<QSharedPointer<PreviewItem>> &p_items) {
  auto &data = m_previewData[p_source];
  if (!data.m_enabled) {
    return;
  }

  auto ts = ++data.m_timeStamp;

  OrderedIntSet affectedBlocks;

  updateBlockPreview(ts, p_source, p_items, affectedBlocks);

  clearBlockObsoletePreview(ts, p_source, affectedBlocks);

  clearObsoleteImages(ts, p_source);

  relayout(affectedBlocks);
}

int PreviewMgr::calculateBlockMargin(const QTextBlock &p_block, int p_tabStopDistance) {
  return TextEditUtils::calculateBlockMargin(p_block, p_tabStopDistance);
}
