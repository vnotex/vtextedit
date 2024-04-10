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
 *
 */

#ifndef KATEVI_HISTORY_H
#define KATEVI_HISTORY_H

#include <katevi/katevi_export.h>
#include <QStringList>

namespace KateVi
{

class KATEVI_EXPORT History
{

public:
    explicit History() = default;
    ~History() = default;

    void append(const QString &historyItem);
    inline const QStringList &items() const { return m_items; }
    void clear();
    inline bool isEmpty() { return m_items.isEmpty(); }

private:
    QStringList m_items;
};
}

#endif // KATEVI_HISTORY_H
