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

#include "macrorecorder.h"
#include "mappings.h"
#include <katevi/globalstate.h>
#include <katevi/inputmodemanager.h>
#include <keymapper.h>
#include <keyparser.h>

#include <QDebug>
#include <QTimer>

using namespace KateVi;

KeyMapper::KeyMapper(InputModeManager *kateViInputModeManager,
                     KateViI::KateViEditorInterface *editorInterface)
    : m_viInputModeManager(kateViInputModeManager), m_interface(editorInterface) {
  m_mappingTimer = new QTimer(this);
  connect(m_mappingTimer, SIGNAL(timeout()), this, SLOT(mappingTimerTimeOut()));
}

void KeyMapper::executeMapping() {
  m_mappingKeys.clear();
  m_mappingTimer->stop();
  m_numMappingsBeingExecuted++;
  const auto mappings = m_viInputModeManager->globalState()->mappings();
  const auto mappingMode =
      Mappings::mappingModeForCurrentViMode(m_viInputModeManager->inputAdapter());
  const QString mappedKeyPresses = mappings->get(mappingMode, m_fullMappingMatch, false, true);
  if (!mappings->isRecursive(mappingMode, m_fullMappingMatch)) {
    m_doNotExpandFurtherMappings = true;
  }

  m_interface->editBegin();
  m_viInputModeManager->feedKeyPresses(mappedKeyPresses);
  m_doNotExpandFurtherMappings = false;
  m_interface->editEnd();

  m_numMappingsBeingExecuted--;
}

void KeyMapper::playBackRejectedKeys() {
  m_isPlayingBackRejectedKeys = true;

  const QString mappingKeys = m_mappingKeys;
  m_mappingKeys.clear();
  m_viInputModeManager->feedKeyPresses(mappingKeys);

  m_isPlayingBackRejectedKeys = false;
}

void KeyMapper::setMappingTimeout(int timeoutMS) { m_timeoutlen = timeoutMS; }

void KeyMapper::mappingTimerTimeOut() {
  if (!m_fullMappingMatch.isNull()) {
    executeMapping();
  } else {
    playBackRejectedKeys();
  }

  m_mappingKeys.clear();
}

bool KeyMapper::handleKeyPress(QChar key) {
  if (!m_doNotExpandFurtherMappings && !m_doNotMapNextKeyPress && !m_isPlayingBackRejectedKeys) {
    m_mappingKeys.append(key);

    // Try to match through all the defined mappings.
    bool isPartialMapping = false;
    bool isFullMapping = false;
    m_fullMappingMatch.clear();
    const auto mappings = m_viInputModeManager->globalState()->mappings();
    const auto mappingMode =
        Mappings::mappingModeForCurrentViMode(m_viInputModeManager->inputAdapter());
    const auto allMappings = mappings->getAll(mappingMode, false, true);
    for (const QString &mapping : allMappings) {
      if (mapping.startsWith(m_mappingKeys)) {
        if (mapping == m_mappingKeys) {
          isFullMapping = true;
          m_fullMappingMatch = mapping;
        } else {
          isPartialMapping = true;
        }
      }
    }

    if (isFullMapping && !isPartialMapping) {
      // Great - m_mappingKeys is a mapping, and one that can't be extended to
      // a longer one - execute it immediately.
      executeMapping();
      return true;
    }

    if (isPartialMapping) {
      // Need to wait for more characters (or a timeout) before we decide what
      // to do with this.
      m_mappingTimer->start(m_timeoutlen);
      m_mappingTimer->setSingleShot(true);
      return true;
    }

    // We've been swallowing all the keypresses meant for m_keys for our mapping
    // keys; now that we know this cannot be a mapping, restore them.
    Q_ASSERT(!isPartialMapping && !isFullMapping);

    const bool isUserKeyPress =
        !m_viInputModeManager->macroRecorder()->isReplaying() && !isExecutingMapping();
    if (isUserKeyPress && m_mappingKeys.size() == 1) {
      // Ugh - unpleasant complication starting with Qt 5.5-ish - it is no
      // longer possible to replay QKeyEvents in such a way that shortcuts
      // are triggered, so if we want to correctly handle a shortcut (e.g.
      // ctrl+s for e.g. Save), we can no longer pop it into m_mappingKeys
      // then immediately playBackRejectedKeys() (as this will not trigger
      // the e.g. Save shortcut) - the best we can do is, if e.g. ctrl+s is
      // not part of any mapping, immediately return false, *not* calling
      // playBackRejectedKeys() and clearing m_mappingKeys ourselves.
      // If e.g. ctrl+s *is* part of a mapping, then if the mapping is
      // rejected, the played back e.g. ctrl+s does not trigger the e.g.
      // Save shortcut. Likewise, we can no longer have e.g. ctrl+s inside
      // mappings or macros - the e.g. Save shortcut will not be triggered!
      // Altogether, a pretty disastrous change from Qt's old behaviour -
      // either they "fix" it (although it could be argued that being able
      // to trigger shortcuts from QKeyEvents was never the desired behaviour)
      // or we try to emulate Shortcut-handling ourselves :(
      m_mappingKeys.clear();
      return false;
    } else {
      playBackRejectedKeys();
      return true;
    }
  }

  m_doNotMapNextKeyPress = false;
  return false;
}

void KeyMapper::setDoNotMapNextKeyPress() { m_doNotMapNextKeyPress = true; }

bool KeyMapper::isExecutingMapping() { return m_numMappingsBeingExecuted > 0; }

bool KeyMapper::isPlayingBackRejectedKeys() { return m_isPlayingBackRejectedKeys; }
