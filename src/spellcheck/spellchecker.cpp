#include <vtextedit/spellchecker.h>

#include <QDebug>
#include <QMap>

#include <languagefilter_p.h>
#include <speller.h>

using namespace vte;

SpellChecker::SpellChecker()
    : m_speller(new Sonnet::Speller()),
      m_languageFilter(new Sonnet::LanguageFilter(new Sonnet::SentenceTokenizer())),
      m_wordTokenizer(new Sonnet::WordTokenizer()) {
  m_dictionaries = m_speller->availableDictionaries();

  m_speller->setAttribute(Sonnet::Speller::AutoDetectLanguage, false);
}

SpellChecker::~SpellChecker() {}

SpellChecker &SpellChecker::getInst() {
  static SpellChecker inst;
  return inst;
}

void SpellChecker::addDictionaryCustomSearchPaths(const QStringList &p_dirs) {
  Sonnet::Speller::addDictionaryCustomSearchPaths(p_dirs);
}

const QMap<QString, QString> &SpellChecker::availableDictionaries() const { return m_dictionaries; }

bool SpellChecker::isValid() const { return m_speller->isValid(); }

Sonnet::LanguageFilter *SpellChecker::languageFilter() { return m_languageFilter.data(); }

Sonnet::WordTokenizer *SpellChecker::wordTokenizer() { return m_wordTokenizer.data(); }

void SpellChecker::setCurrentLanguage(const QString &p_lang) {
  if (p_lang == currentLanguage()) {
    return;
  }

  m_speller->setLanguage(p_lang);
}

QString SpellChecker::currentLanguage() const { return m_speller->language(); }

bool SpellChecker::isMisspelled(const QString &p_word) const {
  return m_speller->isMisspelled(p_word);
}

void SpellChecker::ignoreWord(const QString &p_word) { m_speller->addToSession(p_word); }

void SpellChecker::addToDictionary(const QString &p_word) { m_speller->addToPersonal(p_word); }

QStringList SpellChecker::suggest(const QString &p_word, bool p_autoDetectEnabled) {
  if (p_autoDetectEnabled) {
    auto lang = detectLanguage(p_word);
    if (!lang.isEmpty()) {
      m_speller->setLanguage(lang);
    }
  }

  return m_speller->suggest(p_word);
}

QString SpellChecker::detectLanguage(const QString &p_word) {
  m_languageFilter->setBuffer(p_word);
  if (m_languageFilter->hasNext() && m_languageFilter->isSpellcheckable()) {
    m_languageFilter->next();
    return m_languageFilter->language();
  }

  return QString();
}
