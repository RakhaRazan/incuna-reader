import i18n from 'i18next';
import { initReactI18next } from 'react-i18next';
import en from './locales/en.json';
import id from './locales/id.json';
import ja from './locales/ja.json';

const savedLang = typeof window !== 'undefined' ? localStorage.getItem('koma_lang') : null;

i18n.use(initReactI18next).init({
  resources: {
    en: { translation: en },
    id: { translation: id },
    ja: { translation: ja },
  },
  lng: savedLang && ['en', 'id', 'ja'].includes(savedLang) ? savedLang : 'en',
  fallbackLng: 'en',
  interpolation: {
    escapeValue: false,
  },
});

export default i18n;
