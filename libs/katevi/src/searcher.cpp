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
  const Range r = motionFindPrev();
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
  return Range(match.start(), match.end(), ExclusiveMotion);
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
  KateViI::Cursor searchBegin = startFrom;
  KateViI::SearchOptions flags = KateViI::Regex;

  const QString &pattern = searchParams.pattern;

  if (searchParams.isBackwards) {
    flags |= KateViI::Backwards;
  }
  if (!searchParams.isCaseSensitive) {
    flags |= KateViI::CaseInsensitive;
  }
  KateViI::Range finalMatch;
  for (int i = 0; i < count; i++) {
    if (!searchParams.isBackwards) {
      const KateViI::Range matchRange =
          m_interface
              ->searchText(
                  KateViI::Range(KateViI::Cursor(searchBegin.line(), searchBegin.column() + 1),
                                 m_interface->documentEnd()),
                  pattern, flags)
              .first();

      if (matchRange.isValid()) {
        finalMatch = matchRange;
      } else {
        // Wrap around.
        const KateViI::Range wrappedMatchRange =
            m_interface
                ->searchText(KateViI::Range(m_interface->documentRange().start(),
                                            m_interface->documentEnd()),
                             pattern, flags)
                .first();
        if (wrappedMatchRange.isValid()) {
          finalMatch = wrappedMatchRange;
        } else {
          return KateViI::Range::invalid();
        }
      }
    } else {
      // Ok - this is trickier: we can't search in the range from doc start to
      // searchBegin, because the match might extend *beyond* searchBegin. We
      // could search through the entire document and then filter out only those
      // matches that are after searchBegin, but it's more efficient to instead
      // search from the start of the document until the beginning of the line
      // after searchBegin, and then filter. Unfortunately, searchText doesn't
      // necessarily turn up all matches (just the first one, sometimes) so we
      // must repeatedly search in such a way that the previous match isn't
      // found, until we either find no matches at all, or the first match that
      // is before searchBegin.
      KateViI::Cursor newSearchBegin =
          KateViI::Cursor(searchBegin.line(), m_interface->lineLength(searchBegin.line()));
      KateViI::Range bestMatch = KateViI::Range::invalid();
      while (true) {
        QVector<KateViI::Range> matchesUnfiltered = m_interface->searchText(
            KateViI::Range(newSearchBegin, m_interface->documentRange().start()), pattern, flags);

        if (matchesUnfiltered.size() == 1 && !matchesUnfiltered.first().isValid()) {
          break;
        }

        // After sorting, the last element in matchesUnfiltered is the last
        // match position.
        std::sort(matchesUnfiltered.begin(), matchesUnfiltered.end());

        QVector<KateViI::Range> filteredMatches;
        for (KateViI::Range unfilteredMatch : qAsConst(matchesUnfiltered)) {
          if (unfilteredMatch.start() < searchBegin) {
            filteredMatches.append(unfilteredMatch);
          }
        }
        if (!filteredMatches.isEmpty()) {
          // Want the latest matching range that is before searchBegin.
          bestMatch = filteredMatches.last();
          break;
        }

        // We found some unfiltered matches, but none were suitable. In case
        // matchesUnfiltered wasn't all matching elements, search again,
        // starting from before the earliest matching range.
        if (filteredMatches.isEmpty()) {
          newSearchBegin = matchesUnfiltered.first().start();
        }
      }

      KateViI::Range matchRange = bestMatch;

      if (matchRange.isValid()) {
        finalMatch = matchRange;
      } else {
        const KateViI::Range wrappedMatchRange =
            m_interface
                ->searchText(KateViI::Range(m_interface->documentEnd(),
                                            m_interface->documentRange().start()),
                             pattern, flags)
                .first();

        if (wrappedMatchRange.isValid()) {
          finalMatch = wrappedMatchRange;
        } else {
          return KateViI::Range::invalid();
        }
      }
    }
    searchBegin = finalMatch.start();
  }
  return finalMatch;
}
