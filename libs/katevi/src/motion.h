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

#ifndef KATEVI_MOTION_H
#define KATEVI_MOTION_H

#include <command.h>
#include <katevi/katevi_export.h>
#include <modes/normalvimode.h>
#include <range.h>

namespace KateVi {

/**
 * combined class for motions and text objects. execute() returns a KateViRange.
 * For motions the returned range is only a position (start pos is (-1, -1) to
 * indicate this) for text objects a range (startx, starty), (endx, endy) is
 * returned
 */
class KATEVI_EXPORT Motion : public Command {
public:
  Motion(NormalViMode *parent, const QString &pattern, Range (NormalViMode::*commandMethod)(),
         unsigned int flags = 0);
  Range execute() const;

protected:
  Range (NormalViMode::*m_ptr2commandMethod)();
};

} // namespace KateVi

#endif /* KATEVI_MOTION_H */
