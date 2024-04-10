/*  This file is part of the KDE libraries and the Kate part.
 *
 *  Copyright (C) 2003-2005 Anders Lund <anders@alweb.dk>
 *  Copyright (C) 2001-2010 Christoph Cullmann <cullmann@kde.org>
 *  Copyright (C) 2001 Charles Samuels <charles@kde.org>
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

#ifndef KATEVI_CMDS_H
#define KATEVI_CMDS_H

#include <katevi/interface/command.h>
#include "mappings.h"

#include <QStringList>

namespace KateViI
{
    class KateViEditorInterface;
}

namespace KateVi
{
    class Commands : public KateViI::Command
    {
        Commands();

        static Commands *m_instance;

    public:
        ~Commands() override
        {
            delete m_instance;
            m_instance = nullptr;
        }

        /**
         * execute command on given range
         * @param view view to use for execution
         * @param cmd cmd string
         * @param msg message returned from running the command
         * @param range range to execute command on
         * @return success
         */
        bool exec(KateViI::KateViEditorInterface *editorInterface,
                  const QString &cmd,
                  QString &msg,
                  const KateViI::Range &range = KateViI::Range(-1, -0, -1, 0)) override;

        bool supportsRange(const QString &range) override;

        /** This command does not have help. @see KTextEditor::Command::help */
        bool help(KateViI::KateViEditorInterface *,
                  const QString &,
                  QString &) override
        {
            return false;
        }

        KateViI::KCompletion *completionObject(KateViI::KateViEditorInterface *, const QString &) override;

        static Commands *self()
        {
            if (m_instance == nullptr) {
                m_instance = new Commands();
            }
            return m_instance;
        }

    private:
        const QStringList &mappingCommands();

        Mappings::MappingMode modeForMapCommand(const QString &mapCommand);

        bool isMapCommandRecursive(const QString &mapCommand);
    };

#if 0
    /**
     * Support vim/sed style search and replace
     * @author Charles Samuels <charles@kde.org>
     **/
    class SedReplace : public KateCommands::SedReplace, public KateViCommandInterface
    {
        SedReplace() { }
        static SedReplace *m_instance;

    public:
        ~SedReplace() override
        {
            m_instance = nullptr;
        }

        static SedReplace *self()
        {
            if (m_instance == nullptr) {
                m_instance = new SedReplace();
            }
            return m_instance;
        }

    protected:
        bool interactiveSedReplace(KTextEditor::ViewPrivate *kateView, QSharedPointer<InteractiveSedReplacer> interactiveSedReplace) override;
    };
#endif
}

#endif /* KATEVI_CMDS_H */
