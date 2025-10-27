#include <vtextedit/theme.h>

#include <QDebug>

#include <QFile>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMetaEnum>

using namespace vte;

int Format::s_nextId = 0;

Format::Format()
    : m_bold(false), m_italic(false), m_underline(false), m_strikeThrough(false), m_hasBold(false),
      m_hasItalic(false), m_hasUnderline(false), m_hasStrikeThrough(false), m_id(s_nextId++) {}

QTextCharFormat Format::toTextCharFormat() const {
  QTextCharFormat tcf;

  if (!m_fontFamily.isEmpty()) {
    tcf.setFontFamily(m_fontFamily);
  }

  if (m_fontPointSize > 0) {
    tcf.setFontPointSize(m_fontPointSize);
  }

  auto color = textColor();
  if (color.isValid()) {
    tcf.setForeground(color);
  }

  color = backgroundColor();
  if (color.isValid()) {
    tcf.setBackground(color);
  }

  if (m_hasBold) {
    tcf.setFontWeight(QFont::Bold);
  }

  if (m_hasItalic) {
    tcf.setFontItalic(true);
  }

  if (m_hasUnderline) {
    tcf.setFontUnderline(true);
  }

  if (m_hasStrikeThrough) {
    tcf.setFontStrikeOut(true);
  }

  return tcf;
}

QSharedPointer<Theme> Theme::createThemeFromFile(const QString &p_filePath) {
  QSharedPointer<Theme> theme;
  QFile themeFile(p_filePath);
  if (!themeFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
    qWarning() << "failed to open theme file" << p_filePath;
    return theme;
  }
  const auto jsonData = themeFile.readAll();

  QJsonParseError parseError;
  auto jsonDoc = QJsonDocument::fromJson(jsonData, &parseError);
  if (parseError.error != QJsonParseError::NoError) {
    qWarning() << "failed to parse theme file" << p_filePath << ":" << parseError.errorString();
    return theme;
  }

  theme.reset(new Theme());
  theme->m_filePath = p_filePath;
  theme->load(jsonDoc.object());
  return theme;
}

void Theme::load(const QJsonObject &p_obj) {
  loadMetadata(p_obj);

  if (m_type != QStringLiteral("vtextedit")) {
    qWarning() << "incorrect type of theme file" << m_filePath << m_type;
    return;
  }

  loadEditorStyles(p_obj);

  loadMarkdownEditorStyles(p_obj);

  loadMarkdownSyntaxStyles(p_obj);
}

void Theme::loadMetadata(const QJsonObject &p_obj) {
  const auto obj = p_obj.value(QStringLiteral("metadata")).toObject();
  m_name = obj.value(QStringLiteral("name")).toString();
  m_revision = obj.value(QStringLiteral("revision")).toInt();
  m_type = obj.value(QStringLiteral("type")).toString();
}

void Theme::loadEditorStyles(const QJsonObject &p_obj) {
  static const auto indexOfEditorStyleEnum =
      Theme::staticMetaObject.indexOfEnumerator("EditorStyle");
  Q_ASSERT(indexOfEditorStyleEnum >= 0);
  const auto metaEnum = Theme::staticMetaObject.enumerator(indexOfEditorStyleEnum);
  const auto obj = p_obj.value(QStringLiteral("editor-styles")).toObject();
  // Skip the Max flag.
  for (int i = 0; i < metaEnum.keyCount() - 1; ++i) {
    Q_ASSERT(i == metaEnum.value(i));
    m_editorStyles[i] = loadStyleFormat(obj.value(metaEnum.key(i)).toObject());
  }
}

void Theme::loadMarkdownEditorStyles(const QJsonObject &p_obj) {
  static const auto indexOfEditorStyleEnum =
      Theme::staticMetaObject.indexOfEnumerator("EditorStyle");
  Q_ASSERT(indexOfEditorStyleEnum >= 0);
  const auto metaEnum = Theme::staticMetaObject.enumerator(indexOfEditorStyleEnum);
  const auto obj = p_obj.value(QStringLiteral("markdown-editor-styles")).toObject();
  // Skip the Max flag.
  for (int i = 0; i < metaEnum.keyCount() - 1; ++i) {
    Q_ASSERT(i == metaEnum.value(i));
    if (obj.contains(metaEnum.key(i))) {
      m_markdownEditorStyles.insert(static_cast<EditorStyle>(metaEnum.value(i)),
                                    loadStyleFormat(obj.value(metaEnum.key(i)).toObject()));
    }
  }
}

// Convert QJsonValue @p val into a color, if possible. Valid colors are only
// in hex format: #rrggbb. On error, returns 0x00000000.
static inline QRgb readColor(const QJsonValue &val) {
  const QRgb unsetColor = 0;
  if (!val.isString()) {
    return unsetColor;
  }
  const QString str = val.toString();
  if (str.isEmpty() || str[0] != QLatin1Char('#')) {
    return unsetColor;
  }
  const QColor color(str);
  return color.isValid() ? color.rgb() : unsetColor;
}

static QString chooseAvailableFont(const QStringList &p_families) {
  if (p_families.isEmpty()) {
    return QString();
  }

  const auto dbFamilies = QFontDatabase().families();
  for (int i = 0; i < p_families.size(); ++i) {
    const QString family = p_families.at(i).trimmed();
    if (family.isEmpty()) {
      continue;
    }

    for (int j = 0; j < dbFamilies.size(); ++j) {
      QString dbFamily = dbFamilies.at(j);
      dbFamily.remove(QRegularExpression("\\[.*\\]"));
      dbFamily = dbFamily.trimmed();
      if (family == dbFamily || family.toLower() == dbFamily.toLower()) {
        return dbFamilies.at(j);
      }
    }
  }

  return p_families.at(0);
}

Format Theme::loadStyleFormat(const QJsonObject &p_obj) {
  Format fmt;

  auto val = p_obj.value(QStringLiteral("font-family"));
  if (val.isString()) {
    fmt.m_fontFamilies = val.toString().split(QLatin1Char(','));
    fmt.m_fontFamily = chooseAvailableFont(fmt.m_fontFamilies);
  }

  val = p_obj.value(QStringLiteral("font-size"));
  int fontSize = val.toInt(-1);
  if (fontSize > 0) {
    fmt.m_fontPointSize = fontSize;
  }

  val = p_obj.value(QStringLiteral("bold"));
  if (val.isBool()) {
    fmt.m_bold = val.toBool();
    fmt.m_hasBold = true;
  }

  val = p_obj.value(QStringLiteral("italic"));
  if (val.isBool()) {
    fmt.m_italic = val.toBool();
    fmt.m_hasItalic = true;
  }

  val = p_obj.value(QStringLiteral("underline"));
  if (val.isBool()) {
    fmt.m_underline = val.toBool();
    fmt.m_hasUnderline = true;
  }

  val = p_obj.value(QStringLiteral("strike-through"));
  if (val.isBool()) {
    fmt.m_strikeThrough = val.toBool();
    fmt.m_hasStrikeThrough = true;
  }

  fmt.m_textColor = readColor(p_obj.value(QStringLiteral("text-color")));
  fmt.m_backgroundColor = readColor(p_obj.value(QStringLiteral("background-color")));
  fmt.m_selectedTextColor = readColor(p_obj.value(QStringLiteral("selected-text-color")));
  fmt.m_selectedBackgroundColor =
      readColor(p_obj.value(QStringLiteral("selected-background-color")));
  return fmt;
}

void Theme::loadMarkdownSyntaxStyles(const QJsonObject &p_obj) {
  static const auto indexOfMarkdownStyleEnum =
      Theme::staticMetaObject.indexOfEnumerator("MarkdownSyntaxStyle");
  Q_ASSERT(indexOfMarkdownStyleEnum >= 0);
  const auto metaEnum = Theme::staticMetaObject.enumerator(indexOfMarkdownStyleEnum);
  const auto obj = p_obj.value(QStringLiteral("markdown-syntax-styles")).toObject();
  if (obj.isEmpty()) {
    return;
  }

  // Skip the Max flag.
  m_markdownSyntaxStyles.reset(new QVector<Format>(metaEnum.keyCount() - 1));
  for (int i = 0; i < metaEnum.keyCount() - 1; ++i) {
    Q_ASSERT(i == metaEnum.value(i));
    (*m_markdownSyntaxStyles)[i] = loadStyleFormat(obj.value(metaEnum.key(i)).toObject());
  }
}

const QString &Theme::name() const { return m_name; }

Format &Theme::editorStyle(EditorStyle p_style) {
  Q_ASSERT(p_style < EditorStyle::MaxEditorStyle);
  return m_editorStyles[p_style];
}

const Format &Theme::editorStyle(EditorStyle p_style) const {
  Q_ASSERT(p_style < EditorStyle::MaxEditorStyle);
  return m_editorStyles[p_style];
}

const QMap<Theme::EditorStyle, Format> &Theme::markdownEditorStyles() const {
  return m_markdownEditorStyles;
}

const QSharedPointer<QVector<Format>> &Theme::markdownSyntaxStyles() const {
  return m_markdownSyntaxStyles;
}
