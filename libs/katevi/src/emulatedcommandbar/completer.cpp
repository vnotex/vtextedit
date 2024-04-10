/*  This file is part of the KDE libraries and the Kate part.
 *
 *  Copyright (C) 2013-2016 Simon St James <kdedevel@etotheipiplusone.com>
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

#include "completer.h"

#include "activemode.h"
#include <katevi/emulatedcommandbar.h>

#include <katevi/interface/katevieditorinterface.h>

#include <QCompleter>
#include <QLineEdit>
#include <QStringListModel>
#include <QAbstractItemView>
#include <QRegularExpression>
#include <QKeyEvent>

using namespace KateVi;

namespace
{
    bool caseInsensitiveLessThan(const QString &s1, const QString &s2)
    {
        return s1.toLower() < s2.toLower();
    }
}

Completer::Completer(EmulatedCommandBar *emulatedCommandBar,
                     KateViI::KateViEditorInterface *editorInterface,
                     QLineEdit *edit )
    : m_edit(edit),
      m_interface(editorInterface)
{
    m_completer = new QCompleter(QStringList(), edit);
    // Can't find a way to stop the QCompleter from auto-completing when attached to a QLineEdit,
    // so don't actually set it as the QLineEdit's completer.
    m_completer->setWidget(edit);
    m_completer->setObjectName(QStringLiteral("Completer.EmulatedCommandBar.KateVi"));
    m_completionModel = new QStringListModel(emulatedCommandBar);
    m_completer->setModel(m_completionModel);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->popup()->installEventFilter(emulatedCommandBar);
}

void Completer::startCompletion(const CompletionStartParams &completionStartParams )
{
    if (completionStartParams.completionType != CompletionStartParams::None)
    {
        m_completionModel->setStringList(completionStartParams.completions);
        const QString completionPrefix = m_edit->text().mid(completionStartParams.wordStartPos,
                                                            m_edit->cursorPosition() - completionStartParams.wordStartPos);
        m_completer->setCompletionPrefix(completionPrefix);
        m_completer->complete();
        m_currentCompletionStartParams = completionStartParams;
        m_currentCompletionType = completionStartParams.completionType;
    }
}

void Completer::deactivateCompletion()
{
    m_completer->popup()->hide();
    m_currentCompletionType = CompletionStartParams::None;
}

bool Completer::isCompletionActive() const
{
    return m_currentCompletionType != CompletionStartParams::None;
}

bool Completer::isNextTextChangeDueToCompletionChange() const
{
    return m_isNextTextChangeDueToCompletionChange;
}

bool Completer::completerHandledKeyPress(const QKeyEvent *keyEvent)
{
    if (!m_edit->isVisible()) {
        return false;
    }

    const auto modifiers = keyEvent->modifiers();
    const auto key = keyEvent->key();
    if (modifiers == Qt::ControlModifier
        && (key == Qt::Key_C || key == Qt::Key_BracketLeft))
    {
        if (m_currentCompletionType != CompletionStartParams::None
            && m_completer->popup()->isVisible())
        {
            abortCompletionAndResetToPreCompletion();
            return true;
        }
    }

    if (modifiers == Qt::ControlModifier && key == Qt::Key_Space) {
        CompletionStartParams completionStartParams = activateWordFromDocumentCompletion();
        startCompletion(completionStartParams);
        return true;
    }

    if ((modifiers == Qt::ControlModifier && key == Qt::Key_P)
        || key == Qt::Key_Down) {
        if (!m_completer->popup()->isVisible()) {
            const auto completionStartParams = m_currentMode->completionInvoked(CompletionInvocation::ExtraContext);
            startCompletion(completionStartParams);
            if (m_currentCompletionType != CompletionStartParams::None) {
                setCompletionIndex(0);
            }
        } else {
            // Descend to next row, wrapping around if necessary.
            if (m_completer->currentRow() + 1 == m_completer->completionCount()) {
                setCompletionIndex(0);
            } else {
                setCompletionIndex(m_completer->currentRow() + 1);
            }
        }
        return true;
    }

    if ((modifiers == Qt::ControlModifier && key == Qt::Key_N)
        || key == Qt::Key_Up) {
        if (!m_completer->popup()->isVisible()) {
            const auto completionStartParams = m_currentMode->completionInvoked(CompletionInvocation::NormalContext);
            startCompletion(completionStartParams);
            setCompletionIndex(m_completer->completionCount() - 1);
        } else {
            // Ascend to previous row, wrapping around if necessary.
            if (m_completer->currentRow() == 0) {
                setCompletionIndex(m_completer->completionCount() - 1);
            } else {
                setCompletionIndex(m_completer->currentRow() - 1);
            }
        }
        return true;
    }

    if (key == Qt::Key_Enter || key == Qt::Key_Return) {
        if (!m_completer->popup()->isVisible()
            || m_currentCompletionType != CompletionStartParams::WordFromDocument) {
            m_currentMode->completionChosen();
        }

        deactivateCompletion();
        return true;
    }

    return false;
}

void Completer::editTextChanged(const QString &newText)
{
    if (!m_isNextTextChangeDueToCompletionChange) {
        m_textToRevertToIfCompletionAborted = newText;
        m_cursorPosToRevertToIfCompletionAborted = m_edit->cursorPosition();
    }

    // If we edit the text after having selected a completion, this means we implicitly accept it,
    // and so we should dismiss it.
    if (!m_isNextTextChangeDueToCompletionChange
        && m_completer->popup()->currentIndex().row() != -1) {
        deactivateCompletion();
    }

    if (m_currentCompletionType != CompletionStartParams::None
        && !m_isNextTextChangeDueToCompletionChange) {
        updateCompletionPrefix();
    }
}

void Completer::setCurrentMode(ActiveMode* currentMode)
{
    m_currentMode = currentMode;
}

void Completer::setCompletionIndex(int index)
{
    const QModelIndex modelIndex = m_completer->popup()->model()->index(index, 0);
    // Need to set both of these, for some reason.
    m_completer->popup()->setCurrentIndex(modelIndex);
    m_completer->setCurrentRow(index);

    m_completer->popup()->scrollTo(modelIndex);

    currentCompletionChanged();
}

void Completer::currentCompletionChanged()
{
    const QString newCompletion = m_completer->currentCompletion();
    if (newCompletion.isEmpty()) {
        return;
    }

    QString transformedCompletion = newCompletion;
    if (m_currentCompletionStartParams.completionTransform)
    {
        transformedCompletion = m_currentCompletionStartParams.completionTransform(newCompletion);
    }

    m_isNextTextChangeDueToCompletionChange = true;
    m_edit->setSelection(m_currentCompletionStartParams.wordStartPos, m_edit->cursorPosition() - m_currentCompletionStartParams.wordStartPos);
    m_edit->insert(transformedCompletion);
    m_isNextTextChangeDueToCompletionChange = false;
}

void Completer::updateCompletionPrefix()
{
    const QString completionPrefix = m_edit->text().mid(m_currentCompletionStartParams.wordStartPos,
                                                        m_edit->cursorPosition() - m_currentCompletionStartParams.wordStartPos);
    m_completer->setCompletionPrefix(completionPrefix);
    // Seem to need a call to complete() else the size of the popup box is not altered appropriately.
    m_completer->complete();
}

CompletionStartParams Completer::activateWordFromDocumentCompletion()
{
    static const QRegularExpression wordRegEx(QStringLiteral("\\w{1,}"));
    QRegularExpressionMatch match;

    QSet<QString> set;
    // Narrow the range of lines we search around the cursor so that we don't die on huge files.
    const int startLine = qMax(0, m_interface->cursorPosition().line() - 4096);
    const int endLine = qMin(m_interface->lines(), m_interface->cursorPosition().line() + 4096);
    for (int lineNum = startLine; lineNum < endLine; lineNum++) {
        const QString line = m_interface->line(lineNum);
        int wordSearchBeginPos = 0;
        while ((match = wordRegEx.match(line, wordSearchBeginPos)).hasMatch()) {
            const QString foundWord = match.captured();
            set << foundWord;
            wordSearchBeginPos = match.capturedEnd();
        }
    }

    QStringList foundWords(set.values());
    std::sort(foundWords.begin(), foundWords.end(), caseInsensitiveLessThan);

    CompletionStartParams completionStartParams;
    completionStartParams.completionType = CompletionStartParams::WordFromDocument;
    completionStartParams.completions = foundWords;
    completionStartParams.wordStartPos = wordBeforeCursorBegin();
    return completionStartParams;
}

QString Completer::wordBeforeCursor()
{
    const int wordBeforeCursorBegin = this->wordBeforeCursorBegin();
    return m_edit->text().mid(wordBeforeCursorBegin,
                              m_edit->cursorPosition() - wordBeforeCursorBegin);
}

int Completer::wordBeforeCursorBegin()
{
    int wordBeforeCursorBegin = m_edit->cursorPosition() - 1;
    const auto text = m_edit->text();
    while (wordBeforeCursorBegin >= 0
           && (text[wordBeforeCursorBegin].isLetterOrNumber()
               || text[wordBeforeCursorBegin] == QLatin1Char('_'))) {
        --wordBeforeCursorBegin;
    }

    return ++wordBeforeCursorBegin;
}

void Completer::abortCompletionAndResetToPreCompletion()
{
    deactivateCompletion();
    m_isNextTextChangeDueToCompletionChange = true;
    m_edit->setText(m_textToRevertToIfCompletionAborted);
    m_edit->setCursorPosition(m_cursorPosToRevertToIfCompletionAborted);
    m_isNextTextChangeDueToCompletionChange = false;
}
