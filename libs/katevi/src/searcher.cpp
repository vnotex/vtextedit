/*
 *  This file is part of the KDE libraries
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
 *
 */

#include "searcher.h"
#include "history.h"
#include <katevi/globalstate.h>
#include <katevi/inputmodemanager.h>
#include <katevi/interface/katevieditorinterface.h>
#include <katevi/interface/range.h>
#include <modes/modebase.h>

using namespace KateVi;

Searcher::Searcher(InputModeManager *manager)
    : m_viInputModeManager(manager), m_interface(manager->editorInterface()) {}

Searcher::~Searcher() {}

const QString Searcher::getLastSearchPattern() const { return m_lastSearchConfig.pattern; }

void Searcher::setLastSearchParams(const SearchParams &searchParams) {
  m_lastSearchConfig = searchParams;
}

void Searcher::findNext() {
  const Range r = motionFindNext();
  if (r.valid) {
    m_viInputModeManager->getCurrentViModeHandler()->goToPos(r);
  }
}

void Searcher::findPrevious() {
  const Range r = motionFindPrev();
  if (r.valid) {
    m_viInputModeManager->getCurrentViModeHandler()->goToPos(r);
  }
}

Range Searcher::motionFindNext(int count) {
  Range match = findPatternForMotion(m_lastSearchConfig, m_interface->cursorPosition(), count);

  if (!match.valid) {
    return match;
  }
  if (!m_lastSearchConfig.shouldPlaceCursorAtEndOfMatch) {
    return Range(match.startLine, match.startColumn, ExclusiveMotion);
  }
  return Range(match.endLine, match.endColumn - 1, ExclusiveMotion);
}

Range Searcher::motionFindPrev(int count) {
  SearchParams lastSearchReversed = m_lastSearchConfig;
  lastSearchReversed.isBackwards = !lastSearchReversed.isBackwards;
  Range match = findPatternForMotion(lastSearchReversed, m_interface->cursorPosition(), count);

  if (!match.valid) {
    return match;
  }
  if (!m_lastSearchConfig.shouldPlaceCursorAtEndOfMatch) {
    return Range(match.startLine, match.startColumn, ExclusiveMotion);
  }
  return Range(match.endLine, match.endColumn - 1, ExclusiveMotion);
}

Range Searcher::findPatternForMotion(const SearchParams &searchParams,
                                     const KateViI::Cursor &startFrom, int count) const {
  if (searchParams.pattern.isEmpty()) {
    return Range::invalid();
  }

  KateViI::Range match = findPatternWorker(searchParams, startFrom, count);
  return match.isValid() ? Range(match.start(), match.end(), ExclusiveMotion) : Range::invalid();
}

Range Searcher::findWordForMotion(const QString &word, bool backwards,
                                  const KateViI::Cursor &startFrom, int count) {
  m_lastSearchConfig.isBackwards = backwards;
  m_lastSearchConfig.isCaseSensitive = false;
  m_lastSearchConfig.shouldPlaceCursorAtEndOfMatch = false;

  m_viInputModeManager->globalState()->searchHistory()->append(
      QStringLiteral("\\<%1\\>").arg(word));
  QString pattern = QStringLiteral("\\b%1\\b").arg(word);
  m_lastSearchConfig.pattern = pattern;

  return findPatternForMotion(m_lastSearchConfig, startFrom, count);
}

KateViI::Range Searcher::findPattern(const SearchParams &searchParams,
                                     const KateViI::Cursor &startFrom, int count,
                                     bool addToSearchHistory) {
  if (addToSearchHistory) {

    m_viInputModeManager->globalState()->searchHistory()->append(searchParams.pattern);
    m_lastSearchConfig = searchParams;
  }

  return findPatternWorker(searchParams, startFrom, count);
}

KateViI::Range Searcher::findPatternWorker(const SearchParams &searchParams,
                                           const KateViI::Cursor &startFrom, int count) const {
  const auto validCursor = [this](const KateViI::Cursor &p_cursor) {
    return p_cursor.isValid() && p_cursor.line() < m_interface->lines() &&
           p_cursor.column() <= m_interface->lineLength(p_cursor.line());
  };
  if (searchParams.pattern.isEmpty() || count < 1 || !validCursor(startFrom)) {
    return KateViI::Range::invalid();
  }
  KateViI::SearchOptions flags = KateViI::Regex;
  if (searchParams.isBackwards) {
    flags |= KateViI::Backwards;
  }
  if (!searchParams.isCaseSensitive) {
    flags |= KateViI::CaseInsensitive;
  }
  const auto matches =
      m_interface->searchText(m_interface->documentRange(), searchParams.pattern, flags);
  if (matches.isEmpty()) {
    return KateViI::Range::invalid();
  }
  int first = -1;
  for (int i = 0; i < matches.size(); ++i) {
    const auto &match = matches[i];
    if (!match.isValid() || !validCursor(match.start()) || !validCursor(match.end())) {
      return KateViI::Range::invalid();
    }
    if (first < 0 &&
        (searchParams.isBackwards ? match.start() < startFrom : match.start() > startFrom)) {
      first = i;
    }
  }
  if (first < 0) {
    first = 0;
  }
  const auto index = (qint64(first) + (count - 1) % matches.size()) % matches.size();
  return matches[index];
}
