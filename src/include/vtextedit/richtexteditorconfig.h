#ifndef VTEXTEDIT_RICHTEXTEDITORCONFIG_H
#define VTEXTEDIT_RICHTEXTEDITORCONFIG_H

#include <QSharedPointer>

#include "global.h"
#include "vtextedit_export.h"

namespace vte {
class ViConfig;

// Configuration of VRichTextEditor.
// Deliberately minimal: no theme, syntax highlight, folding or spell check.
class VTEXTEDIT_EXPORT RichTextEditorConfig {
public:
  RichTextEditorConfig() = default;

  InputMode m_inputMode = InputMode::NormalMode;

  QSharedPointer<ViConfig> m_viConfig;

  // How many spaces with a Tab be translated into.
  int m_tabStopWidth = 4;

  // Whether center the cursor.
  CenterCursor m_centerCursor = CenterCursor::NeverCenter;

  // Word wrap mode.
  WrapMode m_wrapMode = WrapMode::WordWrap;
};
} // namespace vte

#endif // VTEXTEDIT_RICHTEXTEDITORCONFIG_H
