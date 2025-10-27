/* This file is part of the KDE libraries
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

#ifndef KATE_VIMODE_MARKS_H
#define KATE_VIMODE_MARKS_H

#include <katevi/interface/markinterface.h>

#include <QMap>
#include <QObject>

namespace KateViI {
class KateViEditorInterface;
class Cursor;
class MovingCursor;
} // namespace KateViI

namespace KateVi {
class InputModeManager;

class Marks : public QObject {
  Q_OBJECT

public:
  explicit Marks(InputModeManager *imm);
  ~Marks();

  /** JBOS == Just a Bunch Of Shortcuts **/
  void setStartEditYanked(const KateViI::Cursor &pos);
  void setFinishEditYanked(const KateViI::Cursor &pos);
  void setLastChange(const KateViI::Cursor &pos);
  void setInsertStopped(const KateViI::Cursor &pos);
  void setSelectionStart(const KateViI::Cursor &pos);
  void setSelectionFinish(const KateViI::Cursor &pos);
  void setUserMark(const QChar &mark, const KateViI::Cursor &pos);

  KateViI::Cursor getStartEditYanked() const;
  KateViI::Cursor getFinishEditYanked() const;
  KateViI::Cursor getLastChange() const;
  KateViI::Cursor getInsertStopped() const;
  KateViI::Cursor getSelectionStart() const;
  KateViI::Cursor getSelectionFinish() const;
  KateViI::Cursor getMarkPosition(const QChar &mark) const;

  void writeSessionConfig() const;
  void readSessionConfig();

  QString getMarksOnTheLine(int line) const;

private:
  void syncViMarksAndBookmarks();
  bool isShowable(const QChar &mark);

  void setMark(const QChar &mark, const KateViI::Cursor &pos);

private Q_SLOTS:
  void markChanged(KateViI::KateViEditorInterface *editorInterface, KateViI::Mark mark,
                   KateViI::MarkInterface::MarkChangeAction action);

private:
  InputModeManager *m_inputModeManager = nullptr;

  KateViI::KateViEditorInterface *m_interface = nullptr;

  QMap<QChar, KateViI::MovingCursor *> m_marks;
  bool m_settingMark = false;
};
} // namespace KateVi

#endif // KATE_VIMODE_MARKS_H
