#include <vtextedit/spellchecker.h>

#include <QDebug>
#include <QMap>

#include <speller.h>
#include <languagefilter_p.h>

using namespace vte;

SpellChecker::SpellChecker()
    : m_speller(new Sonnet::Speller()),
      m_languageFilter(new Sonnet::LanguageFilter(new Sonnet::SentenceTokenizer())),
      m_wordTokenizer(new Sonnet::WordTokenizer())
{
    m_dictionaries = m_speller->availableDictionaries();

    m_speller->setAttribute(Sonnet::Speller::AutoDetectLanguage, false);
}

SpellChecker::~SpellChecker()
{

}

SpellChecker &SpellChecker::getInst()
{
    static SpellChecker inst;
    return inst;
}

void SpellChecker::addDictionaryCustomSearchPaths(const QStringList &p_dirs)
{
    Sonnet::Speller::addDictionaryCustomSearchPaths(p_dirs);
}

bool SpellChecker::isAutoDetectLanguageEnabled() const
{
    return m_autoDetectLanguageEnabled;
}

void SpellChecker::setAutoDetectLanguageEnabled(bool p_enabled)
{
    m_autoDetectLanguageEnabled = p_enabled;
    m_speller->setAttribute(Sonnet::Speller::AutoDetectLanguage, p_enabled);
}

const QMap<QString, QString> &SpellChecker::availableDictionaries() const
{
    return m_dictionaries;
}

bool SpellChecker::isValid() const
{
    return m_speller->isValid();
}

Sonnet::LanguageFilter *SpellChecker::languageFilter()
{
    return m_languageFilter.data();
}

Sonnet::WordTokenizer *SpellChecker::wordTokenizer()
{
    return m_wordTokenizer.data();
}

void SpellChecker::setCurrentLanguage(const QString &p_lang)
{
    m_speller->setLanguage(p_lang);
}

QString SpellChecker::currentLanguage() const
{
    return m_speller->language();
}

bool SpellChecker::isMisspelled(const QString &p_word) const
{
    return m_speller->isMisspelled(p_word);
}
