#include <vtextedit/vrichtexteditor.h>

#include <QEvent>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QTextDocument>

#include <katevi/interface/kateviconfig.h>

#include <inputmode/abstractinputmode.h>
#include <inputmode/abstractinputmodefactory.h>
#include <inputmode/inputmodemgr.h>
#include <inputmode/texteditinputmode.h>
#include <inputmode/viinputmodefactory.h>
#include <texteditor/inputmodestatuswidget.h>
#include <vtextedit/viconfig.h>
#include <vtextedit/vtextedit.h>

using namespace vte;

VRichTextEditor::VRichTextEditor(const QSharedPointer<RichTextEditorConfig> &p_config,
                                 QWidget *p_parent)
    : QWidget(p_parent), m_config(p_config) {
  if (!m_config) {
    m_config = QSharedPointer<RichTextEditorConfig>::create();
  }

  setupUI();

  // Install all permanent connections before the input mode is created, so that
  // a widget constructed directly in Vi mode is fully consistent.
  connect(this, &VRichTextEditor::modeChanged, this, &VRichTextEditor::updateInputMethodEnabled);

  updateFromConfig();
}

VRichTextEditor::~VRichTextEditor() {
  // AbstractInputMode holds a raw pointer to the interface, so the mode must be
  // torn down while the interface is still alive.
  detachInputModeStatusWidget();

  if (m_textEdit) {
    m_textEdit->setInputMode(QSharedPointer<AbstractInputMode>());
  }
}

void VRichTextEditor::setupUI() {
  auto mainLayout = new QHBoxLayout(this);
  mainLayout->setContentsMargins(0, 0, 0, 0);
  mainLayout->setSpacing(0);

  m_textEdit = new VTextEdit(this);
  // Keep rich text enabled: this is exactly what distinguishes this widget.
  m_textEdit->setFrameStyle(QFrame::NoFrame);
  m_textEdit->setExpandTab(false);
  m_textEdit->setAutoBracketsEnabled(false);

  mainLayout->addWidget(m_textEdit);

  setFocusProxy(m_textEdit);
}

void VRichTextEditor::updateFromConfig() {
  setInputMode(m_config->m_inputMode);

  m_textEdit->setCenterCursor(m_config->m_centerCursor);

  QTextOption::WrapMode wrapMode = QTextOption::WordWrap;
  switch (m_config->m_wrapMode) {
  case WrapMode::NoWrap:
    wrapMode = QTextOption::NoWrap;
    break;

  case WrapMode::WordWrap:
    wrapMode = QTextOption::WordWrap;
    break;

  case WrapMode::WrapAnywhere:
    wrapMode = QTextOption::WrapAnywhere;
    break;

  case WrapMode::WordWrapOrAnywhere:
    wrapMode = QTextOption::WrapAtWordBoundaryOrAnywhere;
    break;
  }
  m_textEdit->setWordWrapMode(wrapMode);

  // Keep setTabStopDistance() self-consistent with the font in use.
  m_textEdit->setSpaceWidth(QFontMetricsF(m_textEdit->font()).horizontalAdvance(QLatin1Char(' ')));
  m_textEdit->setTabStopWidthInSpaces(m_config->m_tabStopWidth);
}

VTextEdit *VRichTextEditor::getTextEdit() const { return m_textEdit; }

QTextDocument *VRichTextEditor::document() const { return m_textEdit->document(); }

void VRichTextEditor::setInputMode(InputMode p_mode) {
  {
    // Scoped: the owning pointer must be released before the mode is replaced,
    // so that VTextEdit destroys the outgoing mode while the outgoing
    // interface (which it holds by raw pointer) is still alive.
    auto currentMode = m_textEdit->getInputMode();
    if (currentMode && currentMode->mode() == p_mode) {
      return;
    }
  }

  // The outgoing mode object will be destructed by setInputMode() below, and it
  // asserts that its status widget is no longer parented.
  detachInputModeStatusWidget();

  QScopedPointer<TextEditInputMode> newInterface(new TextEditInputMode(m_textEdit));

  // Connect before the mode is installed so no notification is lost during
  // activation.
  connect(newInterface.data(), &TextEditInputMode::editorModeChanged, this,
          &VRichTextEditor::modeChanged);
  connect(newInterface.data(), &TextEditInputMode::textEditFocusIn, this, [this]() {
    auto mode = getInputMode();
    if (mode) {
      mode->focusIn();
    }
    emit focusIn();
  });
  connect(newInterface.data(), &TextEditInputMode::textEditFocusOut, this, [this]() {
    auto mode = getInputMode();
    if (mode) {
      mode->focusOut();
    }
    emit focusOut();
  });

  if (p_mode == InputMode::ViMode && m_config->m_viConfig) {
    auto kateViConfig = m_config->m_viConfig->toKateViConfig();
    kateViConfig->setTabWidth(m_config->m_tabStopWidth);
    auto viFactory = InputModeMgr::getInst().getFactory(InputMode::ViMode);
    static_cast<ViInputModeFactory *>(viFactory.data())->updateViConfig(kateViConfig);
  }

  auto modeFactory = InputModeMgr::getInst().getFactory(p_mode);
  Q_ASSERT(modeFactory);
  auto mode = modeFactory->createInputMode(newInterface.data());
  m_textEdit->setInputMode(mode);

  m_inputModeInterface.swap(newInterface);

  updateInputModeStatusWidget();

  emit modeChanged();
}

QSharedPointer<AbstractInputMode> VRichTextEditor::getInputMode() const {
  return m_textEdit->getInputMode();
}

QSharedPointer<QWidget> VRichTextEditor::inputModeStatusWidget() const {
  return m_inputModeStatusWidget ? m_inputModeStatusWidget->widget() : QSharedPointer<QWidget>();
}

void VRichTextEditor::updateInputModeStatusWidget() {
  auto mode = getInputMode();
  auto widget = mode ? mode->statusWidget() : QSharedPointer<InputModeStatusWidget>();
  if (widget == m_inputModeStatusWidget) {
    return;
  }

  m_inputModeStatusWidget = widget;

  disconnect(m_inputModeFocusConnection);
  if (widget) {
    m_inputModeFocusConnection = connect(widget.data(), &InputModeStatusWidget::focusOut, this,
                                         [this]() { m_textEdit->setFocus(); });
  }

  emit inputModeStatusWidgetChanged(widget ? widget->widget() : QSharedPointer<QWidget>());
}

void VRichTextEditor::detachInputModeStatusWidget() {
  if (!m_inputModeStatusWidget) {
    return;
  }

  auto widget = m_inputModeStatusWidget->widget();
  m_inputModeStatusWidget.clear();

  disconnect(m_inputModeFocusConnection);

  // Let the host unmount it first, then make sure it is unparented.
  emit inputModeStatusWidgetChanged(QSharedPointer<QWidget>());

  if (widget) {
    widget->hide();
    widget->setParent(nullptr);
  }
}

EditorMode VRichTextEditor::getEditorMode() const {
  auto inputMode = getInputMode();
  Q_ASSERT(inputMode);
  return inputMode->editorMode();
}

bool VRichTextEditor::isReadOnly() const { return m_textEdit->isReadOnly(); }

void VRichTextEditor::setReadOnly(bool p_enabled) { m_textEdit->setReadOnly(p_enabled); }

void VRichTextEditor::setHtml(const QString &p_html) { m_textEdit->setHtml(p_html); }

QString VRichTextEditor::toHtml() const { return m_textEdit->toHtml(); }

bool VRichTextEditor::isModified() const { return m_textEdit->document()->isModified(); }

void VRichTextEditor::setModified(bool p_modified) {
  m_textEdit->document()->setModified(p_modified);
}

void VRichTextEditor::updateInputMethodEnabled() {
  m_textEdit->setInputMethodEnabled(isTextInsertingEditorMode(getEditorMode()));
}
