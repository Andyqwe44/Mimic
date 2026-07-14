// ═══ i18n — i18next initialization ═══
// Import in main.tsx before App render so translations are ready.
import i18n from 'i18next'
import { initReactI18next } from 'react-i18next'
import LanguageDetector from 'i18next-browser-languagedetector'
import en from '../locales/en.json'
import zhCN from '../locales/zh-CN.json'
import zhTW from '../locales/zh-TW.json'

// Persisted locale override (set by App after loading settings.json).
// LanguageDetector runs first; App overwrites with saved preference.
let _savedLocale: string | null = null

export function setSavedLocale(locale: string) {
  _savedLocale = locale
  if (i18n.isInitialized && i18n.language !== locale) {
    i18n.changeLanguage(locale)
  }
}

export function getSavedLocale(): string | null {
  return _savedLocale
}

i18n
  .use(LanguageDetector)
  .use(initReactI18next)
  .init({
    resources: {
      en: { translation: en },
      'zh-CN': { translation: zhCN },
      'zh-TW': { translation: zhTW },
    },
    fallbackLng: 'zh-CN',
    debug: false,
    interpolation: {
      escapeValue: false, // React already escapes
    },
    detection: {
      order: ['querystring', 'cookie', 'localStorage', 'navigator'],
      caches: [], // We persist manually via settings.json
      lookupQuerystring: 'lang',
    },
  })

export default i18n
