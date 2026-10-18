#include <vtextedit/vtypsteditor.h>

#include <markdowneditor/textdocumentlayout.h>
#include <vtextedit/typsteditorconfig.h>
#include <vtextedit/vtextedit.h>
#include <vtextedit/vtexteditor.h>

#include "typstcompilerbridge.h"
#include "typsthighlighter.h"
#include "typstpreviewhost.h"
#include "typstpreviewpane.h"
#include "typstsourceeditor.h"

#include <QDir>
#include <QListWidget>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSplitter>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

using namespace vte;

namespace {
QString normalizedFile(const QString &p_file) {
  return QDir::cleanPath(QDir::fromNativeSeparators(p_file));
}

QString normalizedText(QString p_text) {
  p_text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
  p_text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
  return p_text;
}
} // namespace

class VTypstEditor::Private {
public:
  Private(VTypstEditor *p_owner, const QSharedPointer<TypstEditorConfig> &p_config,
          const QSharedPointer<TextEditorParameters> &p_parameters)
      : q(p_owner), config(QSharedPointer<TypstEditorConfig>::create(
                        p_config ? *p_config : TypstEditorConfig())) {
    normalizeConfig();
    activeFile = config->m_mainFile;
    auto *layout = new QVBoxLayout(q);
    layout->setContentsMargins(0, 0, 0, 0);
    splitter = new QSplitter(Qt::Horizontal, q);
    source = new TypstSourceEditor(
        config->m_textEditorConfig,
        p_parameters ? p_parameters : QSharedPointer<TextEditorParameters>::create(), splitter);
    source->setObjectName(QStringLiteral("typstSource"));
    source->enableInternalContextMenu();
    pane = new TypstPreviewPane(*config, splitter);
    pane->setObjectName(QStringLiteral("typstPdf"));
    splitter->addWidget(source);
    splitter->addWidget(pane);
    splitter->setChildrenCollapsible(false);
    layout->addWidget(splitter, 1);
    diagnosticList = new QListWidget(q);
    diagnosticList->setObjectName(QStringLiteral("typstDiagnostics"));
    diagnosticList->setMaximumHeight(120);
    layout->addWidget(diagnosticList);
    diagnosticList->hide();
    pane->hide();

    bridge = new TypstCompilerBridge(q);
    host = new TypstPreviewHost(source, *config, q);
    host->setActiveFile(activeFile);
    host->setEnabled(isInplacePreviewActive());

    QObject::connect(pane, &TypstPreviewPane::verticalScrollChanged, q, [this]() {
      emit q->pdfNavigationChanged();
      syncScroll(false);
    });
    auto *sourceScroll = source->getTextEdit()->verticalScrollBar();
    QObject::connect(sourceScroll, &QScrollBar::valueChanged, q,
                     [this]() { syncScroll(true); });
    QObject::connect(sourceScroll, &QScrollBar::rangeChanged, q,
                     [this]() { deferScrollSync(); });
    QObject::connect(pane->verticalScrollBar(), &QScrollBar::rangeChanged, q,
                     [this]() { deferScrollSync(); });
    QObject::connect(source->typstLayout(), &TextDocumentLayout::widgetPreviewGeometryChanged, q,
                     [this]() { deferScrollSync(); });
    cursorHighlightTimer.setSingleShot(true);
    cursorHighlightTimer.setInterval(120);
    QObject::connect(&cursorHighlightTimer, &QTimer::timeout, q,
                     [this]() { requestCursorHighlight(); });
    QObject::connect(source->getTextEdit(), &QTextEdit::cursorPositionChanged, q, [this]() {
      invalidateCursorHighlight();
      cursorHighlightPending = isSideBySideActive() && !changingMode && !syncingScroll;
      if (cursorHighlightPending) {
        cursorHighlightTimer.start();
      }
    });
    QObject::connect(pane, &TypstPreviewPane::updateAvailableChanged, q,
                     &VTypstEditor::pdfUpdateAvailableChanged);
    QObject::connect(source, &VTextEditor::focusIn, q, [this]() {
      // Hiding a focused preview transfers focus; presentation changes are not edit gestures.
      if (!changingMode && mode == Mode::Edit) {
        pane->publishPending();
      }
    });
    QObject::connect(source->document(), &QTextDocument::contentsChange, q,
                     [this](int, int p_removed, int p_added) {
                       if (p_removed || p_added) {
                         updateInput();
                       }
                     });
    QObject::connect(bridge, &TypstCompilerBridge::generationChanged, q,
                     [this](quint64 p_epoch, quint64 p_generation) {
                       const bool hadHeadings =
                           outlineSnapshot && !outlineSnapshot->m_headings.isEmpty();
                       outlineSnapshot.clear();
                       invalidateCursorHighlight();
                       host->setCurrentGeneration(p_epoch, p_generation);
                       source->typstHighlighter()->setGeneration(p_generation);
                       pane->setCurrentGeneration(p_epoch, p_generation);
                       pdfDelivered = 0;
                       pendingNavigationSnapshot = 0;
                       if (hadHeadings) {
                         emit q->headingsUpdated({});
                         if (p_epoch != bridge->epoch() || p_generation != bridge->generation()) {
                           return;
                         }
                       }
                       setStatus(CompileStatus::Compiling,
                                 VTypstEditor::tr("Preview is stale; compiling changes…"));
                     });
    QObject::connect(bridge, &TypstCompilerBridge::stateChanged, q,
                     [this](TypstCompilerBridge::State p_state, const QString &p_message) {
                       CompileStatus state = CompileStatus::Idle;
                       switch (p_state) {
                       case TypstCompilerBridge::State::Starting:
                       case TypstCompilerBridge::State::Compiling:
                         state = CompileStatus::Compiling;
                         break;
                       case TypstCompilerBridge::State::Ready:
                         state = CompileStatus::Ready;
                         break;
                       case TypstCompilerBridge::State::Error:
                         state = CompileStatus::Error;
                         break;
                       case TypstCompilerBridge::State::Unavailable:
                         state = CompileStatus::Unavailable;
                         break;
                       case TypstCompilerBridge::State::Idle:
                         break;
                       }
                       setStatus(state, p_message);
                     });
    QObject::connect(bridge, &TypstCompilerBridge::highlightsReady, source->typstHighlighter(),
                     &TypstHighlighter::setHighlights);
    QObject::connect(bridge, &TypstCompilerBridge::diagnosticsReady, q,
                     [this](const QVector<TypstDiagnostic> &p_diagnostics) {
                       diagnosticRecords = p_diagnostics;
                       diagnosticMessages.clear();
                       diagnosticList->clear();
                       for (const auto &diagnostic : p_diagnostics) {
                         const auto message = diagnostic.m_source.m_file.isEmpty()
                                                  ? diagnostic.m_message
                                                  : diagnostic.m_source.m_file +
                                                        QStringLiteral(": ") + diagnostic.m_message;
                         diagnosticMessages.append(message);
                         diagnosticList->addItem(message);
                       }
                       diagnosticList->setVisible(!diagnosticMessages.isEmpty());
                       emit q->diagnosticsChanged(diagnosticMessages);
                     });
    QObject::connect(diagnosticList, &QListWidget::itemActivated, q,
                     [this](QListWidgetItem *p_item) {
                       const int index = diagnosticList->row(p_item);
                       if (index >= 0 && index < diagnosticRecords.size()) {
                         const auto &range = diagnosticRecords[index].m_source;
                         if (range.isValid()) {
                           navigateSource(range.m_file, range.m_start, range.m_end);
                         }
                       }
                     });
    QObject::connect(
        bridge, &TypstCompilerBridge::snapshotReady, q, [this](TypstSnapshot p_snapshot) {
          if (!p_snapshot || p_snapshot != bridge->latestSnapshot()) {
            return;
          }
          if (isInplacePreviewActive()) {
            host->setSnapshot(p_snapshot);
          }
          ensurePdf();
          int detached = 0;
          for (const auto &preview : p_snapshot->m_previews) {
            if (!preview.m_source.isValid() || preview.m_source.m_file != activeFile ||
                preview.m_status == QStringLiteral("unresolved") ||
                preview.m_status == QStringLiteral("unmapped")) {
              ++detached;
            }
          }
          if (detached) {
            setStatus(CompileStatus::Ready,
                      VTypstEditor::tr(
                          "Preview ready; %1 generated or imported objects remain in page output.")
                          .arg(detached));
          }
          if (p_snapshot == bridge->latestSnapshot()) {
            outlineSnapshot = p_snapshot;
            emit q->headingsUpdated(p_snapshot->m_headings);
          }
        });
    QObject::connect(bridge, &TypstCompilerBridge::pdfReady, q,
                     [this](const TypstSnapshot &p_snapshot, const QByteArray &p_pdf) {
                       // Receipt is independent of visibility and selection-deferred publication.
                       pdfDelivered = p_snapshot->m_id;
                       pane->setSnapshot(p_snapshot, p_pdf);
                     });
    QObject::connect(pane, &TypstPreviewPane::snapshotPublished, q, [this](quint64 p_id) {
      bridge->setDisplayedSnapshot(pane->snapshot());
      emit q->pdfNavigationChanged();
      if (pendingNavigationSnapshot == p_id) {
        const QScopedValueRollback<bool> guard(syncingScroll, true);
        pane->goToPage(pendingNavigationPage, pendingNavigationPoint);
        pendingNavigationSnapshot = 0;
      }
      if (cursorHighlightPending) {
        cursorHighlightTimer.start();
      }
    });
    QObject::connect(host, &TypstPreviewHost::rasterRequested, bridge,
                     &TypstCompilerBridge::requestRaster);
    QObject::connect(bridge, &TypstCompilerBridge::pageImageReady, host,
                     &TypstPreviewHost::setPageImage);
    QObject::connect(bridge, &TypstCompilerBridge::rasterCancelled, host,
                     &TypstPreviewHost::cancelRaster);
    QObject::connect(pane, &TypstPreviewPane::sourceLocationRequested, bridge,
                     &TypstCompilerBridge::outputToSource);
    QObject::connect(bridge, &TypstCompilerBridge::sourceLocationReady, q,
                     [this](const QString &p_file, int p_start, int p_end) {
                       navigateSource(p_file, p_start, p_end);
                     });
    QObject::connect(bridge, &TypstCompilerBridge::outputLocationReady, q,
                     [this](quint64 p_id, int p_page, const QPointF &p_point) {
                       navigateOutput(p_id, p_page, p_point);
                     });
    QObject::connect(bridge, &TypstCompilerBridge::cursorLocationReady, q,
                     [this](quint64 p_request, quint64 p_snapshot, int p_page,
                            const QPointF &p_point) {
                       if (p_request != cursorHighlightRequest || !isSideBySideActive()) {
                         return;
                       }
                       const auto snapshot = currentDisplayedSnapshot();
                       if (!snapshot || snapshot->m_id != p_snapshot) {
                         return;
                       }
                       cursorHighlightPending = false;
                       const QScopedValueRollback<bool> guard(syncingScroll, true);
                       pane->flashSourceLocation(p_snapshot, p_page, p_point);
                     });
    QObject::connect(pane, &TypstPreviewPane::externalLinkRequested, q,
                     &VTypstEditor::externalLinkRequested);
    QObject::connect(bridge, &TypstCompilerBridge::externalLinkRequested, q,
                     &VTypstEditor::externalLinkRequested);
    QObject::connect(host, &TypstPreviewHost::errorOccurred, q,
                     [this](const QString &p_error) { setStatus(CompileStatus::Error, p_error); });
    QObject::connect(pane, &TypstPreviewPane::errorOccurred, q,
                     [this](const QString &p_error) { setStatus(CompileStatus::Error, p_error); });
    QObject::connect(bridge, &TypstCompilerBridge::operationFailed, q,
                     [this](const QString &, const QString &p_error) {
                       setStatus(CompileStatus::Error, p_error);
                     });
    updateInput();
  }

  ~Private() {
    cursorHighlightTimer.stop();
    QObject::disconnect(bridge, nullptr, q, nullptr);
    QObject::disconnect(pane, nullptr, q, nullptr);
    QObject::disconnect(source, nullptr, q, nullptr);
    QObject::disconnect(source->getTextEdit(), nullptr, q, nullptr);
    QObject::disconnect(source->getTextEdit()->verticalScrollBar(), nullptr, q, nullptr);
    // Stop the producer, then source reservations, before either underlying document dies.
    delete bridge;
    delete host;
    delete pane;
    delete source;
  }

  void normalizeConfig() {
    config->m_projectRoot = QDir(config->m_projectRoot).absolutePath();
    config->m_mainFile = normalizedFile(config->m_mainFile);
    if (!config->m_textEditorConfig) {
      config->m_textEditorConfig = QSharedPointer<TextEditorConfig>::create();
    }
  }

  void updateInput() {
    // toPlainText is LF source; getText applies the host's save-time line-ending policy.
    overlays.insert(activeFile, source->document()->toPlainText());
    bridge->updateProject(*config, activeFile, overlays);
  }

  void setStatus(CompileStatus p_status, const QString &p_message) {
    status = p_status;
    message = p_message;
    emit q->compileStatusChanged(status, message);
  }

  bool isInplacePreviewActive() const { return mode == Mode::Edit && inplacePreviewEnabled; }
  bool isPdfVisible() const { return mode == Mode::Read || livePreviewEnabled; }
  bool isSideBySideActive() const { return mode == Mode::Edit && livePreviewEnabled; }

  void deferScrollSync() {
    // Layout and range changes may clamp or restore source/PDF scroll positions later this turn.
    if (!scrollLayoutChanging) {
      scrollLayoutChanging = true;
      QTimer::singleShot(0, q, [this]() { scrollLayoutChanging = false; });
    }
  }

  void setPresentation(Mode p_mode, bool p_inplace, bool p_live) {
    const bool modeChanged = mode != p_mode;
    const bool inplaceChanged = inplacePreviewEnabled != p_inplace;
    const bool liveChanged = livePreviewEnabled != p_live;
    if (!modeChanged && !inplaceChanged && !liveChanged) {
      return;
    }
    const bool hadCards = isInplacePreviewActive();
    const bool hadPdf = isPdfVisible();
    const bool wasSideBySide = isSideBySideActive();
    const QScopedValueRollback<bool> modeGuard(changingMode, true);
    const QScopedValueRollback<bool> scrollGuard(syncingScroll, true);
    mode = p_mode;
    inplacePreviewEnabled = p_inplace;
    livePreviewEnabled = p_live;

    // Flag changes in Read only remember preferences; no layout, focus, or compiler work.
    if (modeChanged || hadCards != isInplacePreviewActive() || hadPdf != isPdfVisible()) {
      deferScrollSync();
      if (wasSideBySide != isSideBySideActive()) {
        if (wasSideBySide) {
          splitSizes = splitter->sizes();
        }
        invalidateCursorHighlight();
        cursorHighlightPending = false;
      }
      if (hadCards != isInplacePreviewActive()) {
        host->setEnabled(isInplacePreviewActive());
      }
      if (modeChanged) {
        source->setReadOnly(readOnly || mode == Mode::Read);
        source->setVisible(mode == Mode::Edit);
      }
      if (hadPdf != isPdfVisible()) {
        pane->setVisible(isPdfVisible());
      }
      if (!wasSideBySide && isSideBySideActive()) {
        splitter->setSizes(splitSizes.isEmpty() ? QList<int>{q->width() / 2, q->width() / 2}
                                               : splitSizes);
      }
      if (!hadCards && isInplacePreviewActive()) {
        if (const auto snapshot = bridge->latestSnapshot()) {
          host->setSnapshot(snapshot);
        }
      }
      if (!hadPdf && isPdfVisible()) {
        ensurePdf();
      }
    }
    // Keep host-triggered focus changes inside the same pending-PDF/scroll guards.
    if (modeChanged) {
      emit q->modeChanged(mode);
    }
    if (inplaceChanged) {
      emit q->inplacePreviewEnabledChanged(inplacePreviewEnabled);
    }
    if (liveChanged) {
      emit q->livePreviewEnabledChanged(livePreviewEnabled);
    }
  }

  void ensurePdf() {
    if (!isPdfVisible()) {
      return;
    }
    const auto snapshot = bridge->latestSnapshot();
    if (snapshot && pdfDelivered != snapshot->m_id) {
      bridge->requestPdf(snapshot->m_id);
    }
  }

  void invalidateCursorHighlight() {
    ++cursorHighlightRequest;
    cursorHighlightTimer.stop();
    pane->clearSourceHighlight();
  }

  TypstSnapshot currentDisplayedSnapshot() const {
    const auto latest = bridge->latestSnapshot();
    const auto displayed = pane->snapshot();
    return latest && displayed && latest->m_epoch == bridge->epoch() &&
                   latest->m_generation == bridge->generation() &&
                   displayed->m_epoch == latest->m_epoch &&
                   displayed->m_generation == latest->m_generation && displayed->m_id == latest->m_id
               ? displayed
               : TypstSnapshot();
  }

  void requestCursorHighlight() {
    if (!cursorHighlightPending || !isSideBySideActive() || !currentDisplayedSnapshot()) {
      return;
    }
    // Keep pending until a current reply arrives; compilation may invalidate this request.
    bridge->sourceToOutput(activeFile, source->getTextEdit()->textCursor().position(),
                           cursorHighlightRequest);
  }

  void syncScroll(bool p_fromSource) {
    if (changingMode || syncingScroll || scrollLayoutChanging || source->typstLayout()->isBusy() ||
        !isSideBySideActive() || !source->isVisible() || !pane->isVisible() || !pane->snapshot()) {
      return;
    }
    auto *from = source->getTextEdit()->verticalScrollBar();
    auto *to = pane->verticalScrollBar();
    if (!p_fromSource) {
      qSwap(from, to);
    }
    const qreal fromRange = qreal(from->maximum()) - from->minimum();
    const qreal toRange = qreal(to->maximum()) - to->minimum();
    if (fromRange <= 0 || toRange <= 0) {
      return;
    }
    const QScopedValueRollback<bool> guard(syncingScroll, true);
    const qreal progress = (qreal(from->value()) - from->minimum()) / fromRange;
    to->setValue(qRound(to->minimum() + progress * toRange));
  }

  void navigateSource(const QString &p_file, int p_start, int p_end) {
    if (p_file != activeFile) {
      emit q->sourceLocationRequested(p_file, p_start, p_end);
      return;
    }
    const int length = source->document()->characterCount() - 1;
    if (p_start < 0 || p_end < p_start || p_end > length) {
      return;
    }
    const QScopedValueRollback<bool> guard(syncingScroll, true);
    QTextCursor cursor(source->document());
    cursor.setPosition(p_start);
    cursor.setPosition(p_end, QTextCursor::KeepAnchor);
    source->getTextEdit()->setTextCursor(cursor);
    if (mode == Mode::Read) {
      // Seed the viewport before enabling cards: their deferred layout preserves this anchor.
      source->getTextEdit()->ensureCursorVisible();
      q->setMode(Mode::Edit);
    }
    source->getTextEdit()->ensureCursorVisible();
    source->getTextEdit()->setFocus();
  }

  void navigateOutput(quint64 p_snapshot, int p_page, const QPointF &p_point) {
    const auto latest = bridge->latestSnapshot();
    if (!isPdfVisible() || !latest || latest->m_id != p_snapshot) {
      return;
    }
    const QScopedValueRollback<bool> guard(syncingScroll, true);
    if (pane->snapshot() && pane->snapshot()->m_id == p_snapshot &&
        pane->snapshot()->m_epoch == latest->m_epoch) {
      pane->goToPage(p_page, p_point);
    } else {
      pendingNavigationSnapshot = p_snapshot;
      pendingNavigationPage = p_page;
      pendingNavigationPoint = p_point;
      ensurePdf();
    }
  }

  VTypstEditor *q;
  QSharedPointer<TypstEditorConfig> config;
  TypstSourceEditor *source = nullptr;
  TypstPreviewPane *pane = nullptr;
  TypstPreviewHost *host = nullptr;
  TypstCompilerBridge *bridge = nullptr;
  TypstSnapshot outlineSnapshot;
  QSplitter *splitter = nullptr;
  QListWidget *diagnosticList = nullptr;
  QVector<TypstDiagnostic> diagnosticRecords;
  QStringList diagnosticMessages;
  QMap<QString, QString> overlays;
  QString activeFile;
  QList<int> splitSizes;
  Mode mode = Mode::Edit;
  bool inplacePreviewEnabled = true;
  bool livePreviewEnabled = false;
  CompileStatus status = CompileStatus::Idle;
  QString message;
  bool readOnly = false;
  bool changingMode = false;
  bool syncingScroll = false;
  bool scrollLayoutChanging = false;
  QTimer cursorHighlightTimer;
  quint64 cursorHighlightRequest = 0;
  bool cursorHighlightPending = false;
  quint64 pdfDelivered = 0;
  quint64 pendingNavigationSnapshot = 0;
  int pendingNavigationPage = 0;
  QPointF pendingNavigationPoint;
};

VTypstEditor::VTypstEditor(const QSharedPointer<TypstEditorConfig> &p_config,
                           const QSharedPointer<TextEditorParameters> &p_parameters,
                           QWidget *p_parent)
    : QWidget(p_parent), d(new Private(this, p_config, p_parameters)) {}

VTypstEditor::~VTypstEditor() = default;
VTextEditor *VTypstEditor::getTextEditor() const { return d->source; }
QTextDocument *VTypstEditor::document() const { return d->source->document(); }
VTypstEditor::Mode VTypstEditor::mode() const { return d->mode; }

void VTypstEditor::setMode(Mode p_mode) {
  d->setPresentation(p_mode, d->inplacePreviewEnabled, d->livePreviewEnabled);
}

bool VTypstEditor::isInplacePreviewEnabled() const { return d->inplacePreviewEnabled; }
void VTypstEditor::setInplacePreviewEnabled(bool p_enabled) {
  d->setPresentation(d->mode, p_enabled, d->livePreviewEnabled);
}

bool VTypstEditor::isLivePreviewEnabled() const { return d->livePreviewEnabled; }
void VTypstEditor::setLivePreviewEnabled(bool p_enabled) {
  d->setPresentation(d->mode, d->inplacePreviewEnabled, p_enabled);
}

bool VTypstEditor::isReadOnly() const { return d->readOnly; }
void VTypstEditor::setReadOnly(bool p_readOnly) {
  d->readOnly = p_readOnly;
  d->source->setReadOnly(p_readOnly || d->mode == Mode::Read);
}

const TypstEditorConfig &VTypstEditor::getConfig() const { return *d->config; }

void VTypstEditor::setConfig(const QSharedPointer<TypstEditorConfig> &p_config) {
  if (!p_config) {
    return;
  }
  if (d->config->m_projectRoot != QDir(p_config->m_projectRoot).absolutePath() ||
      d->config->m_mainFile != normalizedFile(p_config->m_mainFile)) {
    d->host->clear();
    d->source->typstHighlighter()->invalidate();
  }
  d->config = QSharedPointer<TypstEditorConfig>::create(*p_config);
  d->normalizeConfig();
  d->source->applyConfig(d->config->m_textEditorConfig);
  d->pane->applyConfig(*d->config);
  d->host->applyConfig(*d->config);
  d->source->setReadOnly(d->readOnly || d->mode == Mode::Read);
  d->updateInput();
}

void VTypstEditor::setProject(const QString &p_root, const QString &p_mainFile) {
  const auto root = QDir(p_root).absolutePath();
  const auto mainFile = normalizedFile(p_mainFile);
  if (d->config->m_projectRoot != root || d->config->m_mainFile != mainFile) {
    d->host->clear();
    d->source->typstHighlighter()->invalidate();
  }
  d->config->m_projectRoot = root;
  d->config->m_mainFile = mainFile;
  d->updateInput();
}

QString VTypstEditor::activeFile() const { return d->activeFile; }

void VTypstEditor::setActiveFile(const QString &p_file) {
  const auto file = normalizedFile(p_file);
  if (file == d->activeFile) {
    return;
  }
  d->overlays.insert(d->activeFile, document()->toPlainText());
  d->activeFile = file;
  d->host->setActiveFile(file);
  d->source->typstHighlighter()->invalidate();
  d->updateInput();
}

void VTypstEditor::setFileOverlay(const QString &p_file, const QString &p_text) {
  const auto file = normalizedFile(p_file);
  const auto text = normalizedText(p_text);
  if (file == d->activeFile) {
    if (document()->toPlainText() != text) {
      d->source->setText(text);
    }
  } else if (d->overlays.value(file) != text || !d->overlays.contains(file)) {
    d->overlays.insert(file, text);
    d->updateInput();
  }
}

void VTypstEditor::removeFileOverlay(const QString &p_file) {
  const auto file = normalizedFile(p_file);
  if (file != d->activeFile && d->overlays.remove(file)) {
    d->updateInput();
  }
}

VTypstEditor::CompileStatus VTypstEditor::compileStatus() const { return d->status; }
QString VTypstEditor::compileMessage() const { return d->message; }
QStringList VTypstEditor::diagnostics() const { return d->diagnosticMessages; }
const QVector<TypstHeadingInfo> &VTypstEditor::getHeadings() const {
  static const QVector<TypstHeadingInfo> empty;
  return d->outlineSnapshot ? d->outlineSnapshot->m_headings : empty;
}
void VTypstEditor::retryCompilation() { d->bridge->retry(); }
void VTypstEditor::showOutputAtCursor() {
  if (d->mode == Mode::Edit && !d->livePreviewEnabled) {
    setLivePreviewEnabled(true);
  }
  d->invalidateCursorHighlight();
  d->cursorHighlightPending = false;
  d->bridge->sourceToOutput(d->activeFile, d->source->getTextEdit()->textCursor().position(), 0);
}

qreal VTypstEditor::pdfZoom() const { return d->pane->zoom(); }
void VTypstEditor::setPdfZoom(qreal p_zoom) { d->pane->setZoom(p_zoom); }
void VTypstEditor::fitPdfWidth() { d->pane->fitWidth(); }
void VTypstEditor::fitPdfPage() { d->pane->fitPage(); }
void VTypstEditor::setPdfSearchText(const QString &p_text) { d->pane->setSearchText(p_text); }
void VTypstEditor::findNextPdfMatch(bool p_backwards) { d->pane->findNext(p_backwards); }
int VTypstEditor::currentPdfPage() const { return d->pane->currentPage(); }
int VTypstEditor::pdfPageCount() const {
  const auto snapshot = d->pane->snapshot();
  return snapshot ? static_cast<int>(snapshot->m_pages.size()) : 0;
}
void VTypstEditor::goToPdfPage(int p_page) { d->pane->goToPage(p_page); }
bool VTypstEditor::hasPendingPdfUpdate() const { return d->pane->hasPendingUpdate(); }
void VTypstEditor::publishPendingPdfUpdate() { d->pane->publishPending(); }
