/*  This file is part of the KDE libraries and the Kate part.
 *
 *  Copyright (C) 2008 Erlend Hamberg <ehamberg@gmail.com>
 *  Copyright (C) 2011 Svyatoslav Kuzmich <svatoslav1@gmail.com>
 *  Copyright (C) 2012 - 2013 Simon St James <kdedevel@etotheipiplusone.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 */

#ifndef KATEVI_INPUT_MODE_MANAGER_H
#define KATEVI_INPUT_MODE_MANAGER_H

#include <QKeyEvent>
#include <QScopedPointer>
#include <QStack>
#include <katevi/katevi_export.h>

#include <katevi/completion.h>
#include <katevi/definitions.h>
#include <katevi/interface/katevieditorinterface.h>

class QString;

namespace KateViI {
class KateViInputMode;
class KateViConfig;
} // namespace KateViI

namespace KateVi {
class GlobalState;
class Searcher;
class CompletionRecorder;
class CompletionReplayer;
class Marks;
class Jumps;
class MacroRecorder;
class LastChangeRecorder;
class ModeBase;
class NormalViMode;
class InsertViMode;
class VisualViMode;
class ReplaceViMode;
class KeyParser;
class KeyMapper;

class KATEVI_EXPORT InputModeManager {
public:
  InputModeManager(KateViI::KateViInputMode *inputAdapter,
                   KateViI::KateViEditorInterface *editorInterface);
  ~InputModeManager();
  InputModeManager(const InputModeManager &) = delete;
  InputModeManager &operator=(const InputModeManager &) = delete;

  /**
   * feed key the given key press to the command parser
   * @return true if keypress was is [part of a] command, false otherwise
   */
  bool handleKeyPress(const QKeyEvent *e);

  /**
   * feed key the given list of key presses to the key handling code, one by one
   */
  void feedKeyPresses(const QString &keyPresses) const;

  /**
   * Determines whether we are currently processing a Vi keypress
   * @return true if we are still in a call to handleKeyPress, false otherwise
   */
  bool isHandlingKeyPress() const;

  /**
   * @return The current vi mode
   */
  ViMode getCurrentViMode() const;

  /**
   * @return The current vi mode string representation
   */
  KateViI::ViewMode getCurrentViewMode() const;

  /**
   * @return the previous vi mode
   */
  ViMode getPreviousViMode() const;

  /**
   * @return true if and only if the current mode is one of VisualMode,
   * VisualBlockMode or VisualLineMode.
   */
  bool isAnyVisualMode() const;

  /**
   * @return one of getViNormalMode(), getViVisualMode(), etc, depending on
   * getCurrentViMode().
   */
  ModeBase *getCurrentViModeHandler() const;

  /**
   * set normal mode to be the active vi mode and perform the needed setup work
   */
  void viEnterNormalMode();

  const QString getVerbatimKeys() const;

  /**
   * changes the current vi mode to the given mode
   */
  void changeViMode(ViMode newMode);

  // Call mode base's method to start insert mode.
  void startInsertMode();

  /**
   * set insert mode to be the active vi mode and perform the needed setup work
   */
  void viEnterInsertMode();

  /**
   * set visual mode to be the active vi mode and make the needed setup work
   */
  void viEnterVisualMode(ViMode visualMode = ViMode::VisualMode);

  /**
   * set replace mode to be the active vi mode and make the needed setup work
   */
  void viEnterReplaceMode();

  /**
   * @return the NormalMode instance
   */
  NormalViMode *getViNormalMode();

  /**
   * @return the InsertMode instance
   */
  InsertViMode *getViInsertMode();

  /**
   * @return the VisualMode instance
   */
  VisualViMode *getViVisualMode();

  /**
   * @return the ReplaceMode instance
   */
  ReplaceViMode *getViReplaceMode();

#if 0
    /**
     * append a QKeyEvent to the key event log
     */
    void appendKeyEventToLog(const QKeyEvent &e);
#endif

  /**
   * clear the key event log
   */
  void clearCurrentChangeLog();

  /**
   * copy the contents of the key events log to m_lastChange so that it can be
   * repeated
   */
  void storeLastChangeCommand();

  /**
   * repeat last change by feeding the contents of m_lastChange to feedKeys()
   */
  void repeatLastChange();

  void doNotLogCurrentKeypress();

  bool getTemporaryNormalMode() { return m_temporaryNormalMode; }

  void reset();

  inline Marks *marks() { return m_marks.data(); }

  // Whether the MacroRecorder is recording.
  bool isRecording() const;

#if 0
    inline Searcher *searcher() { return m_searcher; }
#endif

  CompletionRecorder *completionRecorder() { return m_completionRecorder.data(); }
  CompletionReplayer *completionReplayer() { return m_completionReplayer.data(); }

  MacroRecorder *macroRecorder() { return m_macroRecorder.data(); }

  LastChangeRecorder *lastChangeRecorder() { return m_lastChangeRecorder.data(); }

#if 0
    // session stuff
    void readSessionConfig();
    void writeSessionConfig();
#endif

  KeyMapper *keyMapper();

  void setTemporaryNormalMode(bool b) { m_temporaryNormalMode = b; }

  inline Jumps *jumps() { return m_jumps.data(); }

  GlobalState *globalState() const;

  KateViI::KateViConfig *kateViConfig() const;

  KateViI::KateViEditorInterface *editorInterface() const;

  KateViI::KateViInputMode *inputAdapter() { return m_inputAdapter; }

  void updateCursor(const KateViI::Cursor &c);

  void pushKeyMapper(QSharedPointer<KeyMapper> mapper);
  void popKeyMapper();

  // Make sure the cursor does not end up after the end of the line.
  void amendCursorPosition();

private:
  QScopedPointer<NormalViMode> m_viNormalMode;

  QScopedPointer<InsertViMode> m_viInsertMode;

  QSharedPointer<VisualViMode> m_viVisualMode;

  QSharedPointer<ReplaceViMode> m_viReplaceMode;

  ViMode m_currentViMode;
  ViMode m_previousViMode;

  KateViI::KateViInputMode *m_inputAdapter;
  KateViI::KateViEditorInterface *m_interface;
  KeyParser *m_keyParser;

  // Create a new keymapper for each macro event, to simplify expansion of
  // mappings in macros where the macro itself was triggered by expanding a
  // mapping!
  QStack<QSharedPointer<KeyMapper>> m_keyMapperStack;

  int m_insideHandlingKeyPressCount = 0;

  /**
   * a list of the (encoded) key events that was part of the last change.
   */
  QString m_lastChange;

  CompletionList m_lastChangeCompletionsLog;

  /**
   * true when normal mode was started by Ctrl-O command in insert mode.
   */
  bool m_temporaryNormalMode = false;

  QScopedPointer<Marks> m_marks;

  QScopedPointer<Jumps> m_jumps;

  Searcher *m_searcher;

  QSharedPointer<CompletionRecorder> m_completionRecorder;
  QSharedPointer<CompletionReplayer> m_completionReplayer;

  QSharedPointer<MacroRecorder> m_macroRecorder;

  QSharedPointer<LastChangeRecorder> m_lastChangeRecorder;
};

} // namespace KateVi

#endif
