#ifndef KATEVI_KATEVIINPUTMODE_H
#define KATEVI_KATEVIINPUTMODE_H

#include <katevi/katevi_export.h>

class QKeyEvent;

namespace KateVi {
class EmulatedCommandBar;
class GlobalState;
class InputModeManager;
} // namespace KateVi

namespace KateViI {
class Cursor;
class KateViConfig;

enum CaretStyle { Line, Block, Underline, Half, MaxCaretStyle };

class KATEVI_EXPORT KateViInputMode {
public:
  virtual ~KateViInputMode() {}

  virtual KateVi::EmulatedCommandBar *viModeEmulatedCommandBar() = 0;

  virtual void showViModeEmulatedCommandBar() = 0;

  virtual KateVi::InputModeManager *viInputModeManager() const = 0;

  virtual bool isActive() const = 0;

  virtual KateVi::GlobalState *globalState() const = 0;

  virtual KateViI::KateViConfig *kateViConfig() const = 0;

  virtual void updateCursor(const KateViI::Cursor &p_cursor) = 0;

  virtual void setCaretStyle(KateViI::CaretStyle p_style) = 0;

  // Count of lines visible in the view port.
  virtual int linesDisplayed() = 0;

  virtual void setCursorBlinkingEnabled(bool p_enabled) = 0;

  virtual bool keyPress(QKeyEvent *p_event) = 0;

  // Request to update key stroke shown to user.
  virtual void updateKeyStroke() = 0;
};
} // namespace KateViI

#endif
