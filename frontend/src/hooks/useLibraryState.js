import { useCallback, useEffect, useRef, useState } from 'react';
import { api } from '../api/client';

export function useLibraryState(i18n) {
  const [health, setHealth] = useState(null);
  const [settings, setSettings] = useState(null);
  const [smartviews, setSmartviews] = useState([]);
  const [shelves, setShelves] = useState([]);
  const [folders, setFolders] = useState([]);
  const [scanStatus, setScanStatus] = useState(null);
  const [coverPackStatus, setCoverPackStatus] = useState(null);
  const [activeComic, setActiveComic] = useState(null);

  const [activeFilter, setActiveFilter] = useState({ type: 'all' });
  const [authorFilter, setAuthorFilter] = useState('');
  const [tagFilter, setTagFilter] = useState('');
  const [searchInput, setSearchInput] = useState('');
  const [debouncedSearch, setDebouncedSearch] = useState('');
  const [sort, setSort] = useState('title_asc');
  const [cardMinWidth, setCardMinWidth] = useState(170);
  const [visibleCount, setVisibleCount] = useState(0);
  const [allComicsCount, setAllComicsCount] = useState(0);
  const [refreshToken, setRefreshToken] = useState(0);
  const [coverVersion, setCoverVersion] = useState(0);
  const [theme, setTheme] = useState(() => localStorage.getItem('incuna_theme') || 'dark');
  const lastTotalComicsRef = useRef(null);
  const refreshDebounceRef = useRef(null);
  const metadataControllerRef = useRef(null);
  const autoRefreshRef = useRef(true);
  autoRefreshRef.current = settings?.auto_refresh_library ?? true;

  useEffect(() => {
    const root = document.documentElement;
    if (theme === 'dark') {
      root.classList.add('dark');
    } else {
      root.classList.remove('dark');
    }
    localStorage.setItem('incuna_theme', theme);
  }, [theme]);

  useEffect(() => {
    const timer = setTimeout(() => setDebouncedSearch(searchInput), 150);
    return () => clearTimeout(timer);
  }, [searchInput]);

  const loadMetadata = useCallback(async () => {
    if (metadataControllerRef.current) {
      metadataControllerRef.current.abort();
    }
    const controller = new AbortController();
    metadataControllerRef.current = controller;
    try {
      const [h, s, sv, sh, fl] = await Promise.all([
        api.getHealth(controller.signal),
        api.getSettings(controller.signal),
        api.getSmartviews(controller.signal),
        api.getShelves(controller.signal),
        api.getWatchedFolders(controller.signal),
      ]);
      if (controller.signal.aborted) return;
      setHealth(h);
      const nextCount = h?.comics_count ?? 0;
      lastTotalComicsRef.current = nextCount;
      setAllComicsCount(nextCount);
      setSettings(s);
      setSmartviews(sv || []);
      setShelves(sh || []);
      setFolders(fl || []);

      if (s?.theme && !localStorage.getItem('incuna_theme')) {
        setTheme(s.theme);
      }
      if (s?.language && !localStorage.getItem('incuna_lang') && ['en', 'id', 'ja'].includes(s.language)) {
        i18n.changeLanguage(s.language);
      }
    } catch {
      // ignore initial offline/startup or aborted errors
    } finally {
      if (metadataControllerRef.current === controller) {
        metadataControllerRef.current = null;
      }
    }
  }, [i18n]);

  useEffect(() => {
    loadMetadata();
    return () => {
      if (metadataControllerRef.current) {
        metadataControllerRef.current.abort();
      }
    };
  }, [loadMetadata]);

  useEffect(() => {
    api
      .getStartupFile()
      .then((sf) => {
        if (sf?.has_file && sf.path) {
          const filename = sf.path.split(/[\\/]/).pop() || sf.path;
          setActiveComic({
            id: sf.comic_id || 0,
            path: sf.path,
            title: sf.title || filename,
            current_page: sf.page || 0,
          });
          api.clearStartupFile().catch(() => {});
        }
      })
      .catch(() => {});
  }, []);

  const triggerRefresh = useCallback(() => setRefreshToken((v) => v + 1), []);
  const triggerCoverRefresh = useCallback(() => setCoverVersion((v) => v + 1), []);

  // Poll scan/cover-pack status ONLY while actively scanning or building covers AND not reading.
  // Pauses completely at idle (0 wasteful 1.5s HTTP requests) and while ReaderView is open.
  const isScanning = Boolean(scanStatus?.scanning);
  const isBuildingCovers = Boolean(coverPackStatus?.building);
  const isReading = Boolean(activeComic);
  const wasScanningRef = useRef(false);

  useEffect(() => {
    let active = true;
    let pollController = null;

    const scheduleLibraryRefresh = () => {
      if (refreshDebounceRef.current) {
        clearTimeout(refreshDebounceRef.current);
      }
      refreshDebounceRef.current = setTimeout(() => {
        refreshDebounceRef.current = null;
        if (active) {
          setRefreshToken((v) => v + 1);
        }
      }, 1000);
    };

    const pollStatus = async () => {
      if (pollController) {
        pollController.abort();
      }
      const controller = new AbortController();
      pollController = controller;
      try {
        const [st, packSt, h] = await Promise.all([
          api.getScanStatus(controller.signal),
          api.getCoverPackStatus(controller.signal),
          api.getHealth(controller.signal),
        ]);
        if (!active || controller.signal.aborted) return;

        setScanStatus(st);
        setCoverPackStatus(packSt);
        if (h) {
          setHealth(h);
          const nextTotal = h.comics_count ?? 0;
          setAllComicsCount(nextTotal);
          if (
            lastTotalComicsRef.current !== null &&
            nextTotal !== lastTotalComicsRef.current
          ) {
            lastTotalComicsRef.current = nextTotal;
            if (autoRefreshRef.current) {
              scheduleLibraryRefresh();
            }
          } else if (lastTotalComicsRef.current === null) {
            lastTotalComicsRef.current = nextTotal;
          }
        }

        const scanningNow = Boolean(st?.scanning);
        if (wasScanningRef.current && !scanningNow) {
          loadMetadata();
          if (autoRefreshRef.current) {
            scheduleLibraryRefresh();
          }
        }
        wasScanningRef.current = scanningNow;
      } catch {
        // ignore transient or aborted poll errors
      } finally {
        if (pollController === controller) {
          pollController = null;
        }
      }
    };

    if (!isReading) {
      pollStatus();
    }

    const shouldPoll = !isReading && (isScanning || isBuildingCovers);
    const interval = shouldPoll ? setInterval(pollStatus, 1500) : null;

    return () => {
      active = false;
      if (interval) clearInterval(interval);
      if (pollController) {
        pollController.abort();
      }
      if (refreshDebounceRef.current) {
        clearTimeout(refreshDebounceRef.current);
        refreshDebounceRef.current = null;
      }
    };
  }, [loadMetadata, isReading, isScanning, isBuildingCovers, refreshToken, coverVersion]);

  const handleToggleTheme = async () => {
    const next = theme === 'dark' ? 'light' : 'dark';
    setTheme(next);
    try {
      setSettings(await api.updateSettings({ theme: next }));
    } catch {
      // ignore
    }
  };

  const handleLanguageChange = async (lng) => {
    i18n.changeLanguage(lng);
    localStorage.setItem('incuna_lang', lng);
    try {
      setSettings(await api.updateSettings({ language: lng }));
    } catch {
      // ignore
    }
  };

  const handleCreateShelf = async (name) => {
    await api.createShelf(name);
    setShelves(await api.getShelves());
  };

  const handleRenameShelf = async (id, name) => {
    await api.renameShelf(id, name);
    setShelves(await api.getShelves());
  };

  const handleDeleteShelf = async (id) => {
    await api.deleteShelf(id);
    if (activeFilter.type === 'shelf' && activeFilter.shelfId === id) {
      setActiveFilter({ type: 'all' });
    }
    setShelves(await api.getShelves());
  };

  const handleDeleteSmartview = async (id) => {
    await api.deleteSmartview(id);
    setSmartviews(await api.getSmartviews());
    setActiveFilter({ type: 'all' });
  };

  const handlePickFolder = async () => {
    try {
      const res = await api.pickFolder();
      if (!res?.cancelled && res?.path) {
        loadMetadata();
        setScanStatus((s) => ({ ...s, scanning: true }));
      }
    } catch {
      // ignore if user cancelled or scan already active
    }
  };

  const handleImportFolderPath = async (folderPath) => {
    if (!folderPath) return;
    await api.importFolder(folderPath);
    loadMetadata();
    setScanStatus((s) => ({ ...s, scanning: true }));
  };

  const handlePickFiles = async () => {
    try {
      const res = await api.pickFiles();
      if (!res?.cancelled && res?.paths?.length > 0) {
        const filePath = res.paths[0];
        const filename = filePath.split(/[\\/]/).pop() || filePath;
        setActiveComic({ id: 0, path: filePath, title: filename, current_page: 0 });
      }
    } catch {
      // ignore
    }
  };

  const handleDeleteFolder = async (id) => {
    await api.deleteFolder(id);
    setFolders(await api.getWatchedFolders());
    triggerRefresh();
    loadMetadata();
  };

  const handleToggleFavorite = async (comic) => {
    await api.updateProgress(comic.id, { is_favorite: !comic.is_favorite });
    triggerRefresh();
  };

  const handleSetStatus = async (comic, status) => {
    await api.updateProgress(comic.id, { status });
    triggerRefresh();
  };

  const handleToggleOfflinePin = async (comic) => {
    await api.pinOffline(comic.id, !comic.is_offline_pinned);
    triggerRefresh();
  };

  const handleToggleHidden = async (comic) => {
    await api.setComicHidden(comic.id, !comic.is_hidden);
    triggerRefresh();
  };

  const handleAssignShelf = async (shelfId, comicId) => {
    await api.addComicToShelf(shelfId, comicId);
    setShelves(await api.getShelves());
  };

  const handleRemoveFromShelf = async (shelfId, comicId) => {
    await api.removeComicFromShelf(shelfId, comicId);
    setShelves(await api.getShelves());
    triggerRefresh();
  };

  const handleDeleteComic = async (comic) => {
    await api.deleteComic(comic.id, true);
    triggerRefresh();
    loadMetadata();
  };

  return {
    health,
    settings,
    setSettings,
    smartviews,
    setSmartviews,
    shelves,
    setShelves,
    folders,
    scanStatus,
    coverPackStatus,
    activeComic,
    setActiveComic,
    activeFilter,
    setActiveFilter,
    authorFilter,
    setAuthorFilter,
    tagFilter,
    setTagFilter,
    searchInput,
    setSearchInput,
    debouncedSearch,
    sort,
    setSort,
    cardMinWidth,
    setCardMinWidth,
    visibleCount,
    setVisibleCount,
    allComicsCount,
    setAllComicsCount,
    refreshToken,
    coverVersion,
    theme,
    triggerRefresh,
    triggerCoverRefresh,
    handleToggleTheme,
    handleLanguageChange,
    handleCreateShelf,
    handleRenameShelf,
    handleDeleteShelf,
    handleDeleteSmartview,
    handlePickFolder,
    handleImportFolderPath,
    handlePickFiles,
    handleDeleteFolder,
    handleToggleFavorite,
    handleSetStatus,
    handleToggleOfflinePin,
    handleToggleHidden,
    handleAssignShelf,
    handleRemoveFromShelf,
    handleDeleteComic,
  };
}
