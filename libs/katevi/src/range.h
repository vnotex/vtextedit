/*  This file is part of the KDE libraries and the Kate part.
 *
 *  Copyright (C) 2008 Erlend Hamberg <ehamberg@gmail.com>
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

#ifndef KATEVI_RANGE_H
#define KATEVI_RANGE_H

#include <QDebug>
#include <katevi/definitions.h>
#include <katevi/katevi_export.h>

namespace KateViI {
class Cursor;
class Range;
} // namespace KateViI

namespace KateVi {

enum MotionType { ExclusiveMotion = 0, InclusiveMotion };

class KATEVI_EXPORT Range {
public:
  Range();

  /**
   * For motions which only return a position, in contrast to
   * "text objects" which returns a full blown range.
   */
  explicit Range(int elin, int ecol, MotionType inc);

  explicit Range(int slin, int scol, int elin, int ecol, MotionType mt);
  explicit Range(const KateViI::Cursor &c, MotionType mt);
  explicit Range(const KateViI::Cursor &c1, const KateViI::Cursor c2, MotionType mt);

  /**
   * Modifies this range so the start attributes are lesser than
   * the end attributes.
   */
  void normalize();

  /**
   * @returns an equivalent KateViI::Range for this Range.
   */
  KateViI::Range toEditorRange() const;

  /**
   * Writes this KateViRange to the debug output in a nicely formatted way.
   */
  friend QDebug operator<<(QDebug s, const Range &range) {
    s << "[" << " (" << range.startLine << ", " << range.startColumn << ")"
      << " -> " << " (" << range.endLine << ", " << range.endColumn << ")"
      << "]" << " (" << (range.motionType == InclusiveMotion ? "Inclusive" : "Exclusive")
      << ") (jump: " << (range.jump ? "true" : "false") << ")";
    return s;
  }

  /**
   * @returns an invalid KateViRange allocated on stack.
   */
  static Range invalid();

public:
  int startLine, startColumn;
  int endLine, endColumn;
  MotionType motionType;
  bool valid, jump;
};

} // namespace KateVi

#endif /* KATEVI_RANGE_H */
