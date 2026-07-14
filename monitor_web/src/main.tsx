import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import App from './App'
import './index.css'
import './lib/i18n' // i18next init — must import before App render
import i18n, { setSavedLocale } from './lib/i18n'
import { applyBootTheme, readBootSettings } from './lib/bootSettings'

// Theme/accent before React mount (C++ also injects this; belt-and-suspenders for HMR).
const boot = readBootSettings()
applyBootTheme(boot)
if (typeof boot.locale === 'string' && boot.locale) {
  setSavedLocale(boot.locale)
  void i18n.changeLanguage(boot.locale)
}

createRoot(document.getElementById('root')!).render(
  <StrictMode><App /></StrictMode>,
)
