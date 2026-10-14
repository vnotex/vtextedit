#include <vtextedit/vtypsteditor.h>

#include <vtextedit/typsteditorconfig.h>
#include <vtextedit/vtextedit.h>
#include <vtextedit/vtexteditor.h>

#include "typstcompilerbridge.h"
#include "typsthighlighter.h"
#include "typstpreviewhost.h"
#include "typstpreviewpane.h"
#include "typstsourceeditor.h"

#include <QComboBox>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
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
    auto *modeBar = new QHBoxLayout;
    modeCombo = new QComboBox(q);
    modeCombo->setObjectName(QStringLiteral("typstMode"));
    modeCombo->addItem(VTypstEditor::tr("In-place preview"));
    modeCombo->addItem(VTypstEditor::tr("Side-by-side preview"));
    modeCombo->addItem(VTypstEditor::tr("Read"));
    modeBar->addWidget(modeCombo);
    statusLabel = new QLabel(q);
    statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    modeBar->addWidget(statusLabel, 1);
    auto *retryButton = button(VTypstEditor::tr("Retry"), q);
    modeBar->addWidget(retryButton);
    layout->addLayout(modeBar);

    pdfBar = new QWidget(q);
    auto *pdfLayout = new QHBoxLayout(pdfBar);
    pdfLayout->setContentsMargins(0, 0, 0, 0);
    search = new QLineEdit(pdfBar);
    search->setPlaceholderText(VTypstEditor::tr("Find in PDF"));
    search->setClearButtonEnabled(true);
    pdfLayout->addWidget(search, 1);
    auto *previous = button(VTypstEditor::tr("Previous match"), pdfBar);
    auto *next = button(VTypstEditor::tr("Next match"), pdfBar);
    auto *zoomOut = button(VTypstEditor::tr("Zoom out"), pdfBar);
    auto *zoomIn = button(VTypstEditor::tr("Zoom in"), pdfBar);
    auto *width = button(VTypstEditor::tr("Fit width"), pdfBar);
    auto *page = button(VTypstEditor::tr("Fit page"), pdfBar);
    auto *cursor = button(VTypstEditor::tr("Show cursor in output"), pdfBar);
    updateButton = button(VTypstEditor::tr("Update available"), pdfBar);
    for (auto *item : {previous, next, zoomOut, zoomIn, width, page, cursor, updateButton}) {
      pdfLayout->addWidget(item);
    }
    pageNumber = new QSpinBox(pdfBar);
    pageNumber->setPrefix(VTypstEditor::tr("Page "));
    pageNumber->setMinimum(1);
    pageNumber->setMaximum(1);
    pdfLayout->addWidget(pageNumber);
    layout->addWidget(pdfBar);

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
    updateButton->hide();
    pdfBar->hide();
    pane->hide();

    bridge = new TypstCompilerBridge(q);
    host = new TypstPreviewHost(source, *config, q);
    host->setActiveFile(activeFile);
    host->setEnabled(true);

    QObject::connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), q,
                     [this](int p_index) { q->setMode(static_cast<Mode>(p_index)); });
    QObject::connect(retryButton, &QToolButton::clicked, q, &VTypstEditor::retryCompilation);
    QObject::connect(search, &QLineEdit::textChanged, pane, &TypstPreviewPane::setSearchText);
    QObject::connect(search, &QLineEdit::returnPressed, pane, [this]() { pane->findNext(); });
    QObject::connect(previous, &QToolButton::clicked, pane, [this]() { pane->findNext(true); });
    QObject::connect(next, &QToolButton::clicked, pane, [this]() { pane->findNext(); });
    QObject::connect(zoomOut, &QToolButton::clicked, pane,
                     [this]() { pane->setZoom(pane->zoom() / 1.2); });
    QObject::connect(zoomIn, &QToolButton::clicked, pane,
                     [this]() { pane->setZoom(pane->zoom() * 1.2); });
    QObject::connect(width, &QToolButton::clicked, pane, &TypstPreviewPane::fitWidth);
    QObject::connect(page, &QToolButton::clicked, pane, &TypstPreviewPane::fitPage);
    QObject::connect(cursor, &QToolButton::clicked, q, &VTypstEditor::showOutputAtCursor);
    QObject::connect(updateButton, &QToolButton::clicked, pane, &TypstPreviewPane::publishPending);
    QObject::connect(pageNumber, QOverload<int>::of(&QSpinBox::valueChanged), pane,
                     [this](int p_page) { pane->goToPage(p_page - 1); });
    QObject::connect(pane, &TypstPreviewPane::verticalScrollChanged, q, [this]() {
      updatePageNumber();
      syncScroll(false);
    });
    auto *sourceScroll = source->getTextEdit()->verticalScrollBar();
    QObject::connect(sourceScroll, &QScrollBar::valueChanged, q,
                     [this]() { syncScroll(true); });
    QObject::connect(sourceScroll, &QScrollBar::rangeChanged, q, [this]() {
      // Qt may clamp the value during deferred layout, including after a mode
      // switch. Let the source's extra-tail scrollbar settle without moving PDF.
      if (!sourceRangeChanging) {
        sourceRangeChanging = true;
        QTimer::singleShot(0, q, [this]() { sourceRangeChanging = false; });
      }
    });
    QObject::connect(pane, &TypstPreviewPane::updateAvailableChanged, updateButton,
                     &QWidget::setVisible);
    QObject::connect(source, &VTextEditor::focusIn, q, [this]() {
      if (mode != Mode::Read) {
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
                       host->setCurrentGeneration(p_epoch, p_generation);
                       source->typstHighlighter()->setGeneration(p_generation);
                       pane->setCurrentGeneration(p_epoch, p_generation);
                       pdfDelivered = 0;
                       pendingNavigationSnapshot = 0;
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
        bridge, &TypstCompilerBridge::snapshotReady, q, [this](const TypstSnapshot &p_snapshot) {
          if (mode == Mode::InPlacePreview) {
            host->setSnapshot(p_snapshot);
          } else {
            ensurePdf();
          }
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
        });
    QObject::connect(bridge, &TypstCompilerBridge::pdfReady, q,
                     [this](const TypstSnapshot &p_snapshot, const QByteArray &p_pdf) {
                       pdfDelivered = p_snapshot->m_id;
                       pane->setSnapshot(p_snapshot, p_pdf);
                     });
    QObject::connect(pane, &TypstPreviewPane::snapshotPublished, q, [this](quint64 p_id) {
      bridge->setDisplayedSnapshot(pane->snapshot());
      updatePageNumber();
      if (pendingNavigationSnapshot == p_id) {
        const QScopedValueRollback<bool> guard(syncingScroll, true);
        pane->goToPage(pendingNavigationPage, pendingNavigationPoint);
        pendingNavigationSnapshot = 0;
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
    QObject::disconnect(bridge, nullptr, q, nullptr);
    QObject::disconnect(pane, nullptr, q, nullptr);
    QObject::disconnect(source, nullptr, q, nullptr);
    QObject::disconnect(source->getTextEdit()->verticalScrollBar(), nullptr, q, nullptr);
    // Stop the producer, then source reservations, before either underlying document dies.
    delete bridge;
    delete host;
    delete pane;
    delete source;
  }

  static QToolButton *button(const QString &p_text, QWidget *p_parent) {
    auto *result = new QToolButton(p_parent);
    result->setText(p_text);
    result->setToolTip(p_text);
    return result;
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
    statusLabel->setText(message);
    statusLabel->setToolTip(message);
    emit q->compileStatusChanged(status, message);
  }

  void ensurePdf() {
    const auto snapshot = bridge->latestSnapshot();
    if (snapshot && pdfDelivered != snapshot->m_id) {
      bridge->requestPdf(snapshot->m_id);
    }
  }

  void updatePageNumber() {
    const auto snapshot = pane->snapshot();
    const QSignalBlocker blocker(pageNumber);
    pageNumber->setMaximum(snapshot ? qMax(1, static_cast<int>(snapshot->m_pages.size())) : 1);
    pageNumber->setValue(qMax(1, pane->currentPage() + 1));
  }

  void syncScroll(bool p_fromSource) {
    if (syncingScroll || sourceRangeChanging || mode != Mode::SideBySidePreview ||
        !pane->snapshot()) {
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
    if (mode == Mode::Read) {
      q->setMode(Mode::SideBySidePreview);
    }
    QTextCursor cursor(source->document());
    cursor.setPosition(p_start);
    cursor.setPosition(p_end, QTextCursor::KeepAnchor);
    source->getTextEdit()->setTextCursor(cursor);
    source->getTextEdit()->ensureCursorVisible();
    source->getTextEdit()->setFocus();
  }

  void navigateOutput(quint64 p_snapshot, int p_page, const QPointF &p_point) {
    const auto latest = bridge->latestSnapshot();
    if (!latest || latest->m_id != p_snapshot) {
      return;
    }
    const QScopedValueRollback<bool> guard(syncingScroll, true);
    if (mode == Mode::InPlacePreview) {
      q->setMode(Mode::SideBySidePreview);
    }
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
  QSplitter *splitter = nullptr;
  QComboBox *modeCombo = nullptr;
  QLabel *statusLabel = nullptr;
  QWidget *pdfBar = nullptr;
  QLineEdit *search = nullptr;
  QSpinBox *pageNumber = nullptr;
  QToolButton *updateButton = nullptr;
  QListWidget *diagnosticList = nullptr;
  QVector<TypstDiagnostic> diagnosticRecords;
  QStringList diagnosticMessages;
  QMap<QString, QString> overlays;
  QString activeFile;
  QList<int> splitSizes;
  Mode mode = Mode::InPlacePreview;
  CompileStatus status = CompileStatus::Idle;
  QString message;
  bool readOnly = false;
  bool syncingScroll = false;
  bool sourceRangeChanging = false;
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
  if (p_mode == d->mode) {
    return;
  }
  const QScopedValueRollback<bool> guard(d->syncingScroll, true);
  if (d->mode == Mode::SideBySidePreview) {
    d->splitSizes = d->splitter->sizes();
  }
  d->mode = p_mode;
  const QSignalBlocker blocker(d->modeCombo);
  d->modeCombo->setCurrentIndex(static_cast<int>(p_mode));
  d->host->setEnabled(p_mode == Mode::InPlacePreview);
  d->source->setReadOnly(d->readOnly || p_mode == Mode::Read);
  d->source->setVisible(p_mode != Mode::Read);
  d->pane->setVisible(p_mode != Mode::InPlacePreview);
  d->pdfBar->setVisible(p_mode != Mode::InPlacePreview);
  if (p_mode == Mode::SideBySidePreview) {
    d->splitter->setSizes(d->splitSizes.isEmpty() ? QList<int>{width() / 2, width() / 2}
                                                  : d->splitSizes);
  }
  if (p_mode == Mode::InPlacePreview) {
    if (d->bridge->latestSnapshot()) {
      d->host->setSnapshot(d->bridge->latestSnapshot());
    }
  } else {
    d->ensurePdf();
  }
  emit modeChanged(p_mode);
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
void VTypstEditor::retryCompilation() { d->bridge->retry(); }
void VTypstEditor::showOutputAtCursor() {
  d->bridge->sourceToOutput(d->activeFile, d->source->getTextEdit()->textCursor().position());
}
