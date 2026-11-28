// One-time startup migration of legacy koma_* localStorage keys to incuna_*
export function migrateLegacyStorage() {
  if (typeof window === 'undefined' || !window.localStorage) return;
  try {
    const keys = Object.keys(window.localStorage);
    for (const key of keys) {
      if (key.startsWith('koma_')) {
        const nextKey = `incuna_${key.slice(5)}`;
        if (window.localStorage.getItem(nextKey) === null) {
          const val = window.localStorage.getItem(key);
          if (val !== null) {
            window.localStorage.setItem(nextKey, val);
          }
        }
        window.localStorage.removeItem(key);
      }
    }
  } catch {
    // Ignore storage access errors
  }
}

migrateLegacyStorage();
