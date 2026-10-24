#include "typstsourceeditor.h"

#include <QEvent>
#include <QFontMetricsF>
#include <QTextDocument>

#include "typsthighlighter.h"
#include <markdowneditor/documentresourcemgr.h>
#include <markdowneditor/textdocumentlayout.h>
#include <vtextedit/texteditorconfig.h>
#include <vtextedit/vtextedit.h>

using namespace vte;

namespace {
QSharedPointer<TextEditorConfig> sourceConfig(const QSharedPointer<TextEditorConfig> &p_config) {
  const auto config = p_config ? p_config : QSharedPointer<TextEditorConfig>::create();
  const auto theme = config->m_theme ? config->m_theme : TextEditorConfig::defaultTheme();
  const auto &override = theme->typstTextStyle();
  if (override.m_fontFamily.isEmpty() && override.m_fontPointSize <= 0 && !override.m_textColor &&
      !override.m_backgroundColor && !override.m_selectedTextColor &&
      !override.m_selectedBackgroundColor && !override.m_hasBold && !override.m_hasItalic &&
      !override.m_hasUnderline && !override.m_hasStrikeThrough)
    return config;

  // VTypstEditor keeps the canonical config. Specialize only this source editor,
  // so refreshes and removing the override never inherit an earlier merge.
  auto specialized = QSharedPointer<TextEditorConfig>::create(*config);
  specialized->m_theme = QSharedPointer<Theme>::create(*theme);
  auto &text = specialized->m_theme->editorStyle(Theme::Text);
  if (!override.m_fontFamily.isEmpty()) {
    text.m_fontFamilies = override.m_fontFamilies;
    text.m_fontFamily = override.m_fontFamily;
  }
  if (override.m_fontPointSize > 0)
    text.m_fontPointSize = override.m_fontPointSize;
  if (override.m_textColor)
    text.m_textColor = override.m_textColor;
  if (override.m_backgroundColor)
    text.m_backgroundColor = override.m_backgroundColor;
  if (override.m_selectedTextColor)
    text.m_selectedTextColor = override.m_selectedTextColor;
  if (override.m_selectedBackgroundColor)
    text.m_selectedBackgroundColor = override.m_selectedBackgroundColor;
  if (override.m_hasBold) {
    text.m_bold = override.m_bold;
    text.m_hasBold = true;
  }
  if (override.m_hasItalic) {
    text.m_italic = override.m_italic;
    text.m_hasItalic = true;
  }
  if (override.m_hasUnderline) {
    text.m_underline = override.m_underline;
    text.m_hasUnderline = true;
  }
  if (override.m_hasStrikeThrough) {
    text.m_strikeThrough = override.m_strikeThrough;
    text.m_hasStrikeThrough = true;
  }
  return specialized;
}
} // namespace

TypstSourceEditor::TypstSourceEditor(const QSharedPointer<TextEditorConfig> &p_config,
                                     const QSharedPointer<TextEditorParameters> &p_parameters,
                                     QWidget *p_parent)
    : VTextEditor(sourceConfig(p_config), p_parameters, p_parent) {
  // The document owns the layout and can outlive this subclass's members. Tie
  // the empty resource manager to layout destruction, not editor destruction.
  const auto resources = QSharedPointer<DocumentResourceMgr>::create();
  auto layout = new TextDocumentLayout(document(), resources.data());
  connect(layout, &QObject::destroyed, [resources]() {});
  layout->setPreviewEnabled(true);
  document()->setDocumentLayout(layout);
  layout->setCursorWidth(m_textEdit->cursorWidth());
  connect(m_textEdit, &VTextEdit::cursorWidthChanged, this,
          [this]() { typstLayout()->setCursorWidth(m_textEdit->cursorWidth()); });

  delete m_highlighter;
  m_highlighter = new TypstHighlighter(document(), getConfig());
  connect(typstHighlighter(), &TypstHighlighter::highlightingUpdated, m_textEdit,
          &VTextEdit::updateCursorWidth);
  updateSpellCheck();

  m_geometryTimer.setSingleShot(true);
  connect(&m_geometryTimer, &QTimer::timeout, this, &TypstSourceEditor::refreshGeometry);
  connect(layout, &TextDocumentLayout::becameIdle, this, [this]() {
    if (m_geometryPending)
      m_geometryTimer.start(0);
  });
  m_textEdit->installEventFilter(this);
  refreshGeometry();
  emit syntaxChanged();
}

TypstSourceEditor::~TypstSourceEditor() = default;

void TypstSourceEditor::setSyntax(const QString &p_syntax) {
  Q_UNUSED(p_syntax);
  // The protected seam installs the sole highlighter after the base ctor.
}

QString TypstSourceEditor::getSyntax() const { return QStringLiteral("Typst"); }

TextDocumentLayout *TypstSourceEditor::typstLayout() const {
  return static_cast<TextDocumentLayout *>(document()->documentLayout());
}

TypstHighlighter *TypstSourceEditor::typstHighlighter() const {
  return static_cast<TypstHighlighter *>(m_highlighter);
}

void TypstSourceEditor::applyConfig(const QSharedPointer<TextEditorConfig> &p_config) {
  const int delta = zoomDelta();
  VTextEditor::zoom(0);
  const auto &previous = getConfig().m_theme->typstTextStyle();
  if (!previous.m_fontFamily.isEmpty() || previous.m_textColor || previous.m_backgroundColor ||
      previous.m_selectedTextColor || previous.m_selectedBackgroundColor) {
    // The base editor reads unspecified font/palette properties from the widget.
    // Remove our old stylesheet first so omitted generic properties inherit the
    // unstyled widget, rather than retaining the previous Typst override.
    setStyleSheet(QString());
    m_textEdit->viewport()->setStyleSheet(QString());
  }
  VTextEditor::setConfig(sourceConfig(p_config));
  VTextEditor::zoom(delta);
  typstHighlighter()->refreshTheme(getConfig(), editorFontPointSize() - baseEditorFontPointSize());
  refreshGeometry();
}

void TypstSourceEditor::zoom(int p_delta) {
  VTextEditor::zoom(p_delta);
  refreshGeometry();
}

void TypstSourceEditor::refreshGeometry() {
  auto layout = typstLayout();
  if (layout->isBusy()) {
    m_geometryPending = true;
    layout->requestIdleNotification();
    return;
  }
  m_geometryPending = false;
  // TextDocumentLayout consumes the document's default font/text option and
  // margin; unlike QTextDocumentLayout it uses explicit leading for spacing.
  const QFontMetricsF metrics(m_textEdit->font(), m_textEdit);
  m_textEdit->setSpaceWidth(metrics.horizontalAdvance(QLatin1Char(' ')));
  m_textEdit->setTabStopWidthInSpaces(qMax(1, getConfig().m_tabStopWidth));
  const qreal leading = metrics.lineSpacing() * (qMax(1.0, getConfig().m_lineSpacing) - 1.0);
  if (!qFuzzyCompare(layout->getLeadingSpaceOfLine() + 1, leading + 1)) {
    layout->setLeadingSpaceOfLine(leading);
    layout->relayout();
  }
  m_textEdit->updateCursorWidth();
}

bool TypstSourceEditor::eventFilter(QObject *p_object, QEvent *p_event) {
  if (p_object == m_textEdit && m_highlighter &&
      (p_event->type() == QEvent::FontChange || p_event->type() == QEvent::PaletteChange ||
       p_event->type() == QEvent::StyleChange)) {
    typstHighlighter()->refreshTheme(getConfig(),
                                      editorFontPointSize() - baseEditorFontPointSize());
    m_geometryTimer.start(0);
  }
  return VTextEditor::eventFilter(p_object, p_event);
}
