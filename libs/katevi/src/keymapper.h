/*  This file is part of the KDE libraries and the Kate part.
 *
 *  Copyright (C) 2008-2009 Erlend Hamberg <ehamberg@gmail.com>
 *  Copyright (C) 2013 Simon St James <kdedevel@etotheipiplusone.com>
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

#ifndef KATEVI_KEY_MAPPER_H
#define KATEVI_KEY_MAPPER_H

#include <QObject>
#include <katevi/katevi_export.h>

class QTimer;

namespace KateViI {
class KateViEditorInterface;
}

namespace KateVi {

class InputModeManager;

class KATEVI_EXPORT KeyMapper : public QObject {
  Q_OBJECT

public:
  KeyMapper(InputModeManager *kateViInputModeManager,
            KateViI::KateViEditorInterface *editorInterface);

  bool handleKeyPress(QChar key);

  void setMappingTimeout(int timeoutMS);

  void setDoNotMapNextKeyPress();

  bool isExecutingMapping();

  bool isPlayingBackRejectedKeys();

public Q_SLOTS:
  void mappingTimerTimeOut();

private:
  // Will be the mapping used if we decide that no extra mapping characters will
  // be typed, either because we have a mapping that cannot be extended to
  // another mapping by adding additional characters, or we have a mapping and
  // timed out waiting for it to be extended to a longer mapping. (Essentially,
  // this allows us to have mappings that extend each other e.g. "'12" and
  // "'123", and to choose between them.)
  QString m_fullMappingMatch;

  // Pending mapping keys.
  QString m_mappingKeys;

  // For recrusive mapping.
  bool m_doNotExpandFurtherMappings = false;

  QTimer *m_mappingTimer = nullptr;

  InputModeManager *m_viInputModeManager = nullptr;

  KateViI::KateViEditorInterface *m_interface = nullptr;

  // Time to wait for the next keyPress of a multi-key mapping (default: 1000
  // ms)
  int m_timeoutlen = 1000;

  // If true, do nothing in next handleKeyPress().
  bool m_doNotMapNextKeyPress = false;
  ;

  // Num of matched mappings begin executed.
  int m_numMappingsBeingExecuted = 0;

  bool m_isPlayingBackRejectedKeys = false;

private:
  void executeMapping();

  // If @m_mappingKeys does not end up in a full mapping match, we need to
  // play back these keys without any mapping.
  void playBackRejectedKeys();

  void appendMappingKey(const QChar &key);
};

} // namespace KateVi

#endif /* KATEVI_KEY_MAPPER_H */
