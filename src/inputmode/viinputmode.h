#ifndef VIINPUTMODE_H
#define VIINPUTMODE_H

#include "abstractinputmode.h"
#include <katevi/definitions.h>
#include <katevi/interface/kateviinputmode.h>
#include <texteditor/inputmodestatuswidget.h>

#include <QScopedPointer>
#include <QSharedPointer>
#include <QWidget>

class QLabel;

namespace vte {
class ViStatusBar : public QWidget {
  Q_OBJECT
public:
  ViStatusBar(KateVi::EmulatedCommandBar *p_commandBar);

  void setKeyStroke(const QString &p_keys);

signals:
  void commandBarHidden();

  void commandBarShown();

private:
  // Managed by QObject.
  KateVi::EmulatedCommandBar *m_commandBar = nullptr;

  // Managed by QObject.
  QLabel *m_keyStrokeIndicator = nullptr;
};

class ViStatusWidget : public InputModeStatusWidget {
  Q_OBJECT
public:
  ViStatusWidget(const QSharedPointer<ViStatusBar> &p_widget);

  QSharedPointer<QWidget> widget() Q_DECL_OVERRIDE;

private:
  QSharedPointer<ViStatusBar> m_statusBar;
};

class ViInputMode : public AbstractInputMode, public KateViI::KateViInputMode {
public:
  explicit ViInputMode(InputModeEditorInterface *p_interface,
                       const QSharedPointer<KateVi::GlobalState> &p_viGlobal,
                       const QSharedPointer<KateViI::KateViConfig> &p_viConfig);

  ~ViInputMode();

public:
  void changeViInputMode(KateVi::ViMode p_mode);

public:
  // AbstractInputMode interface.
  QString name() const Q_DECL_OVERRIDE;

  InputMode mode() const Q_DECL_OVERRIDE;

  EditorMode editorMode() const Q_DECL_OVERRIDE;

  QSharedPointer<InputModeStatusWidget> statusWidget() Q_DECL_OVERRIDE;

  void activate() Q_DECL_OVERRIDE;

  void deactivate() Q_DECL_OVERRIDE;

  void focusIn() Q_DECL_OVERRIDE;

  void focusOut() Q_DECL_OVERRIDE;

  bool handleKeyPress(QKeyEvent *p_event) Q_DECL_OVERRIDE;

  bool stealShortcut(QKeyEvent *p_event) Q_DECL_OVERRIDE;

  void preKeyPressDefaultHandle(QKeyEvent *p_event) Q_DECL_OVERRIDE;

  void postKeyPressDefaultHandle(QKeyEvent *p_event) Q_DECL_OVERRIDE;

public:
  // KateVi::KateViInputMode interface.
  KateVi::EmulatedCommandBar *viModeEmulatedCommandBar() Q_DECL_OVERRIDE;

  void showViModeEmulatedCommandBar() Q_DECL_OVERRIDE;

  KateVi::InputModeManager *viInputModeManager() const Q_DECL_OVERRIDE;

  bool isActive() const Q_DECL_OVERRIDE;

  KateVi::GlobalState *globalState() const Q_DECL_OVERRIDE;

  KateViI::KateViConfig *kateViConfig() const Q_DECL_OVERRIDE;

  void updateCursor(const KateViI::Cursor &p_cursor) Q_DECL_OVERRIDE;

  void setCaretStyle(KateViI::CaretStyle p_style) Q_DECL_OVERRIDE;

  int linesDisplayed() Q_DECL_OVERRIDE;

  void setCursorBlinkingEnabled(bool p_enabled) Q_DECL_OVERRIDE;

  bool keyPress(QKeyEvent *p_event) Q_DECL_OVERRIDE;

  void updateKeyStroke() Q_DECL_OVERRIDE;

private:
  void updateCursorBlinking() const;

  void restoreCursorBlinking() const;

  bool needToStartEditSession(const QKeyEvent *p_event) const;

  static CaretStyle kateViCaretStyleToEditorCaretStyle(KateViI::CaretStyle p_style);

  QSharedPointer<KateVi::GlobalState> m_viGlobal;

  QSharedPointer<KateViI::KateViConfig> m_viConfig;

  QScopedPointer<KateVi::InputModeManager> m_viModeManager;

  // Managed by QObject.
  KateVi::EmulatedCommandBar *m_viEmulatedCommandBar = nullptr;

  QSharedPointer<ViStatusBar> m_viStatusBar;

  QSharedPointer<InputModeStatusWidget> m_statusWidget;

  bool m_active = false;

  // If we just stole a shortcut, then the next key press will be the
  // same as the shortcut.
  bool m_nextKeyPressIsOverriddenShortcut = false;

  KateViI::CaretStyle m_caret = KateViI::CaretStyle::Block;

  bool m_cursorBlinkingEnabled = false;

  // Cursor Flash time set by system.
  const int c_cursorFlashTime = 0;
};
} // namespace vte

#endif // VIINPUTMODE_H
