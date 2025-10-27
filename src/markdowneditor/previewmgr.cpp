#include <vtextedit/previewmgr.h>

#include <QDebug>
#include <QDir>
#include <QTextDocument>
#include <QTextLayout>
#include <QUrl>
#include <QVector>

#include <vtextedit/markdownutils.h>
#include <vtextedit/networkutils.h>
#include <vtextedit/texteditutils.h>
#include <vtextedit/textutils.h>

#include "documentresourcemgr.h"

using namespace vte;

typedef PreviewData::Source Source;

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

void PreviewMgr::updateImageLinks(const QVector<peg::ElementRegion> &p_regions) {
  auto &data = m_previewData[Source::ImageLink];
  if (!data.m_enabled) {
    return;
  }

  auto ts = ++data.m_timeStamp;
  previewImageLinks(ts, p_regions);
}

void PreviewMgr::previewImageLinks(TimeStamp p_timeStamp,
                                   const QVector<peg::ElementRegion> &p_regions) {
  QVector<ImageLink> imageLinks;
  fetchImageLinksFromRegions(p_regions, imageLinks);

  OrderedIntSet affectedBlocks;

  updateBlockPreview(p_timeStamp, imageLinks, affectedBlocks);

  clearBlockObsoletePreview(p_timeStamp, Source::ImageLink, affectedBlocks);

  clearObsoleteImages(p_timeStamp, Source::ImageLink);

  relayout(affectedBlocks);
}

void PreviewMgr::fetchImageLinksFromRegions(const QVector<peg::ElementRegion> &p_regions,
                                            QVector<ImageLink> &p_imageLinks) {
  p_imageLinks.clear();
  if (p_regions.isEmpty()) {
    return;
  }

  p_imageLinks.reserve(p_regions.size());
  auto doc = document();
  for (const auto &reg : p_regions) {
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

    QString text;
    if (firstBlock.blockNumber() == lastBlock.blockNumber()) {
      text = firstBlock.text().mid(reg.m_startPos - firstBlockStart, reg.m_endPos - reg.m_startPos);
    } else {
      text = firstBlock.text().mid(reg.m_startPos - firstBlockStart);

      QTextBlock block = firstBlock.next();
      while (block.isValid() && block.blockNumber() < lastBlock.blockNumber()) {
        text += "\n" + block.text();
        block = block.next();
      }

      text += "\n" + lastBlock.text().left(reg.m_endPos - lastBlockStart);
    }

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

    fetchImageLink(text, link);

    if (link.m_linkUrl.isEmpty() || link.m_linkShortUrl.isEmpty()) {
      continue;
    }

    p_imageLinks.append(link);
  }
}

QTextDocument *PreviewMgr::document() const { return m_interface->document(); }

void PreviewMgr::fetchImageLink(const QString &p_text, ImageLink &p_info) {
  QString shortUrl = MarkdownUtils::fetchImageLinkUrl(p_text, p_info.m_width, p_info.m_height);

  // If it is using `\` instead of `/`, skip it to align with read mode.
  if (shortUrl.contains(QLatin1Char('\\'))) {
    qWarning() << "skipped local image with `\\` in path (use `/` instead)" << shortUrl;
    p_info.m_linkShortUrl = p_info.m_linkUrl = QString();
    return;
  }

  p_info.m_linkShortUrl = shortUrl;
  if (shortUrl.isEmpty()) {
    p_info.m_linkUrl = shortUrl;
  } else {
    p_info.m_linkUrl = MarkdownUtils::linkUrlToPath(m_interface->basePath(), shortUrl);
  }
}

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
  // Add size info to the name.
  QString name = QStringLiteral("%1_%2_%3")
                     .arg(p_link.m_linkShortUrl, QString::number(p_link.m_width),
                          QString::number(p_link.m_height));
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
    downloader()->requestAsync(imgPath);

    QSharedPointer<UrlImageData> urlData(new UrlImageData(name, p_link.m_width, p_link.m_height));
    m_urlMap.insert(imgPath, urlData);
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
  // Mainly used for image link preview.
  if (!m_previewData[Source::ImageLink].m_enabled) {
    return;
  }

  auto it = m_urlMap.find(p_url);
  if (it == m_urlMap.end()) {
    return;
  }

  auto data = it.value();
  m_urlMap.erase(it);

  auto resourceMgr = m_interface->documentResourceMgr();
  if (resourceMgr->containsImage(data->m_name) || data->m_name.isEmpty()) {
    return;
  }

  QPixmap image;
  image.loadFromData(p_data.m_data);
  if (!image.isNull()) {
    resourceMgr->addImage(data->m_name,
                          MarkdownUtils::scaleImage(image, data->m_width, data->m_height,
                                                    m_interface->scaleFactor()));
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
