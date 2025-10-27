/*  This file is part of the KDE libraries
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

#include "jumps.h"

#include <katevi/global.h>

#include <QDebug>

using namespace KateVi;

void Jumps::add(const KateViI::Cursor &cursor) {
  for (auto iterator = m_jumps.begin(); iterator != m_jumps.end(); iterator++) {
    if ((*iterator).line() == cursor.line()) {
      m_jumps.erase(iterator);
      break;
    }
  }

  m_jumps.push_back(cursor);
  m_current = m_jumps.end();
}

KateViI::Cursor Jumps::next(const KateViI::Cursor &cursor) {
  if (m_current == m_jumps.end()) {
    return cursor;
  }

  KateViI::Cursor jump;
  if (m_current + 1 != m_jumps.end()) {
    jump = *(++m_current);
  } else {
    jump = *(m_current);
  }

  return jump;
}

KateViI::Cursor Jumps::prev(const KateViI::Cursor &cursor) {
  if (m_current == m_jumps.end()) {
    add(cursor);
    m_current--;
  }

  if (m_current != m_jumps.begin()) {
    m_current--;
    return *m_current;
  }

  return cursor;
}

void Jumps::readSessionConfig() { KATEVI_NIY; }

void Jumps::writeSessionConfig() const { KATEVI_NIY; }
