#ifndef SPELLCHECK_H
#define SPELLCHECK_H

#include <vtextedit/vtextedit_export.h>

#include <QScopedPointer>
#include <QStringList>
#include <QMap>

namespace Sonnet
{
    class Speller;
    class LanguageFilter;
    class WordTokenizer;
}

namespace vte
{
    // Wrapper of Sonnet spell check.
    class VTEXTEDIT_EXPORT SpellChecker
    {
    public:
        static SpellChecker &getInst();

        ~SpellChecker();

        // Returns a map of all available dictionaies with language descriptions and
        // their codes. The key is the description, the code the value.
        const QMap<QString, QString> &availableDictionaries() const;

        bool isValid() const;

        Sonnet::LanguageFilter *languageFilter();

        Sonnet::WordTokenizer *wordTokenizer();

        void setCurrentLanguage(const QString &p_lang);
        QString currentLanguage() const;

        bool isMisspelled(const QString &p_word) const;

        static void addDictionaryCustomSearchPaths(const QStringList &p_dirs);

    private:
        SpellChecker();

        QScopedPointer<Sonnet::Speller> m_speller;

        QScopedPointer<Sonnet::LanguageFilter> m_languageFilter;

        QScopedPointer<Sonnet::WordTokenizer> m_wordTokenizer;

        QMap<QString, QString> m_dictionaries;
    };
}

#endif // SPELLCHECK_H
