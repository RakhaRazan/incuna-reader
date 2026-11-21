import React, { useEffect, useMemo, useRef, useState, useCallback } from 'react';
import { useTranslation } from 'react-i18next';
import {
  FolderPlus,
  FilePlus2,
  LayoutGrid,
  FolderTree,
  Folder,
  ChevronRight,
  CheckCircle2,
  Circle,
  Star,
  CloudDownload,
  Bookmark,
  X,
  CheckSquare,
  History,
  User,
  Tag,
} from 'lucide-react';
import { api, isAbortError } from '../api/client';
import { formatDisplayTitle } from '../utils/format';
import ComicCard, { CoverImage } from './ComicCard';

const PAGE_SIZE = 150;
const GAP = 16;
const PADDING = 24;
const META_HEIGHT = 76;
const COVER_ASPECT = 1.42;

export default function LibraryView({
  search,
  smartview,
  shelfId,
  authorFilter = '',
  tagFilter = '',
  sort,
  cardMinWidth,
  refreshToken,
  coverVersion,
  shelves = [],
  settings = null,
  onTotalChange,
  onFilterAuthor,
  onClearAuthor,
  onClearTag,
  onOpenComic,
  onContextMenu,
  onComicMutated,
  onRefreshShelves,
  onPickFolder,
  onPickFiles,
}) {
  const { t } = useTranslation();
  const containerRef = useRef(null);

  const [viewport, setViewport] = useState({ width: 1024, height: 768 });
  const [scrollTop, setScrollTop] = useState(0);
  const [totalCount, setTotalCount] = useState(0);
  const [initialLoading, setInitialLoading] = useState(true);

  // Folder / Series navigation state
  const [viewMode, setViewMode] = useState('grid'); // 'grid' | 'folders'
  const [folderPrefix, setFolderPrefix] = useState('');
  const [folderTrail, setFolderTrail] = useState([]);
  const [subfolders, setSubfolders] = useState([]);
  const [foldersLoading, setFoldersLoading] = useState(false);
  const [continueItems, setContinueItems] = useState([]);

  // Scroll state (rAF-throttled position + 90ms idle debounce for deferred cover image mounting)
  const [isScrolling, setIsScrolling] = useState(false);
  const scrollIdleTimerRef = useRef(null);
  const isScrollingRef = useRef(false);

  // Stable callback refs so React.memo on ComicCard is never defeated by parent re-renders
  const onOpenComicRef = useRef(onOpenComic);
  onOpenComicRef.current = onOpenComic;
  const onContextMenuRef = useRef(onContextMenu);
  onContextMenuRef.current = onContextMenu;
  const stableOpenComic = useCallback((comic) => onOpenComicRef.current?.(comic), []);
  const stableContextMenu = useCallback((e, comic) => onContextMenuRef.current?.(e, comic), []);

  // Multi-select batch state
  const [selectedIds, setSelectedIds] = useState(() => new Set());
  const [batchBusy, setBatchBusy] = useState(false);
  const [shelfMenuOpen, setShelfMenuOpen] = useState(false);
  const lastSelectedIndexRef = useRef(null);

  // Sparse chunked cache: index -> comic object
  const itemsRef = useRef(new Map());
  const loadedPagesRef = useRef(new Set());
  const loadingPagesRef = useRef(new Set());
  const fetchGenRef = useRef(0);
  const scrollFetchControllerRef = useRef(null);
  const [, setCacheTick] = useState(0);

  // Scroll cancellation & active viewport tracking (75ms debounce)
  const prevVisibleIdsRef = useRef(new Set());
  const pendingCancelIdsRef = useRef(new Set());
  const cancelTimerRef = useRef(null);
  const viewportTimerRef = useRef(null);
  const rafRef = useRef(null);

  const circularProgressBar = settings?.circular_progress_bar ?? true;
  const foldersFirst = settings?.display_folders_before_files ?? true;
  const smartRename = Boolean(settings?.smart_rename);
  const includeHidden = Boolean(settings?.show_hidden_elements);
  const enableContinueReading = settings?.enable_continue_reading ?? true;
  const enableMetadataExtractor = settings?.enable_metadata_extractor ?? true;

  useEffect(() => {
    if (!enableContinueReading) {
      setContinueItems([]);
      return;
    }
    const controller = new AbortController();
    api
      .getContinueReading(10, includeHidden, controller.signal)
      .then((res) => {
        if (!controller.signal.aborted) {
          setContinueItems(res?.items || []);
        }
      })
      .catch(() => {
        if (!controller.signal.aborted) {
          setContinueItems([]);
        }
      });
    return () => controller.abort();
  }, [enableContinueReading, includeHidden, refreshToken]);

  const activeFolderPrefix = viewMode === 'folders' ? folderPrefix : '';
  const isFolderRoot = viewMode === 'folders' && folderPrefix === '';
  const filterKey = `${search}|${smartview}|${shelfId}|${authorFilter}|${tagFilter}|${viewMode}|${activeFolderPrefix}|${sort}|${includeHidden ? 1 : 0}`;
  const prevFilterKeyRef = useRef(filterKey);

  useEffect(() => {
    const el = containerRef.current;
    if (!el) return;

    const updateSize = () => {
      setViewport({ width: el.clientWidth, height: el.clientHeight });
    };
    updateSize();

    const ro = new ResizeObserver(updateSize);
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  // Fetch subfolders immediately when in Folder / Series View (non-blocking)
  useEffect(() => {
    if (viewMode !== 'folders') {
      setSubfolders([]);
      setFoldersLoading(false);
      return;
    }
    const controller = new AbortController();
    setFoldersLoading(true);
    api
      .getLibraryFolders(folderPrefix, controller.signal, includeHidden)
      .then((res) => {
        if (controller.signal.aborted) return;
        const nextFolders = res?.folders || [];
        setSubfolders(nextFolders);
        setFoldersLoading(false);
        if (folderPrefix === '') {
          onTotalChange?.(nextFolders.length, false);
        }
      })
      .catch((err) => {
        if (isAbortError(err) || controller.signal.aborted) return;
        setSubfolders([]);
        setFoldersLoading(false);
      });
    return () => controller.abort();
  }, [viewMode, folderPrefix, refreshToken, includeHidden]);

  useEffect(() => {
    if (scrollFetchControllerRef.current) {
      scrollFetchControllerRef.current.abort();
      scrollFetchControllerRef.current = null;
    }

    const gen = ++fetchGenRef.current;
    const isFilterChange = prevFilterKeyRef.current !== filterKey;
    prevFilterKeyRef.current = filterKey;

    if (isFolderRoot) {
      itemsRef.current = new Map();
      loadedPagesRef.current = new Set();
      loadingPagesRef.current = new Set();
      prevVisibleIdsRef.current = new Set();
      pendingCancelIdsRef.current = new Set();
      setSelectedIds(new Set());
      setShelfMenuOpen(false);
      lastSelectedIndexRef.current = null;
      setTotalCount(0);
      setInitialLoading(false);
      if (isFilterChange) {
        if (containerRef.current) {
          containerRef.current.scrollTop = 0;
        }
        setScrollTop(0);
      }
      setCacheTick((v) => v + 1);
      return;
    }

    const controller = new AbortController();

    if (isFilterChange) {
      itemsRef.current = new Map();
      loadedPagesRef.current = new Set();
      loadingPagesRef.current = new Set();
      prevVisibleIdsRef.current = new Set();
      pendingCancelIdsRef.current = new Set();
      setSelectedIds(new Set());
      setShelfMenuOpen(false);
      lastSelectedIndexRef.current = null;
      setTotalCount(0);
      setInitialLoading(true);
      setCacheTick((v) => v + 1);

      if (containerRef.current) {
        containerRef.current.scrollTop = 0;
      }
      setScrollTop(0);
    }

    const pagesToFetch = new Set([0]);
    const folderOffset = viewMode === 'folders' && foldersFirst ? subfolders.length : 0;
    if (!isFilterChange && visibleRange.endIndex >= folderOffset) {
      const comicStart = Math.max(0, visibleRange.startIndex - folderOffset);
      const comicEnd = visibleRange.endIndex - folderOffset;
      const firstPage = Math.floor(comicStart / PAGE_SIZE);
      const lastPage = Math.floor(comicEnd / PAGE_SIZE);
      for (let p = firstPage; p <= lastPage; p++) {
        pagesToFetch.add(p);
      }
    }

    const pageList = Array.from(pagesToFetch);
    const nextLoaded = new Set();
    loadingPagesRef.current = new Set(pageList);

    Promise.all(
      pageList.map((page) => {
        const offset = page * PAGE_SIZE;
        return api
          .queryLibrary(
            {
              search,
              smartview,
              shelfId,
              folderPrefix: activeFolderPrefix,
              author: authorFilter,
              tag: tagFilter,
              sort,
              includeHidden,
              offset,
              limit: PAGE_SIZE,
            },
            controller.signal
          )
          .then((res) => ({ page, offset, res }));
      })
    )
      .then((results) => {
        if (controller.signal.aborted || fetchGenRef.current !== gen) return;

        // Atomically replace itemsRef on refresh/filter so stale slices never overlap
        const nextItems = new Map();
        let nextTotal = totalCount;

        for (const { page, offset, res } of results) {
          if (page === 0) {
            nextTotal = res.total ?? 0;
          }
          (res.items || []).forEach((item, idx) => {
            nextItems.set(offset + idx, item);
          });
          nextLoaded.add(page);
        }

        itemsRef.current = nextItems;
        loadedPagesRef.current = nextLoaded;
        loadingPagesRef.current = new Set();
        setTotalCount(nextTotal);
        onTotalChange?.(
          viewMode === 'folders' ? subfolders.length + nextTotal : nextTotal,
          activeFolderPrefix !== ''
        );
        setInitialLoading(false);
        setCacheTick((v) => v + 1);
      })
      .catch((err) => {
        if (isAbortError(err) || controller.signal.aborted || fetchGenRef.current !== gen) return;
        loadingPagesRef.current = new Set();
        setInitialLoading(false);
      });

    return () => {
      controller.abort();
    };
  }, [filterKey, refreshToken]);

  // Clear selection on Escape
  useEffect(() => {
    if (selectedIds.size === 0) return;
    const onKeyDown = (e) => {
      if (e.key === 'Escape') {
        setSelectedIds(new Set());
        setShelfMenuOpen(false);
        lastSelectedIndexRef.current = null;
      }
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [selectedIds.size]);

  const folderCount = viewMode === 'folders' ? subfolders.length : 0;
  const comicCount = isFolderRoot ? 0 : totalCount;
  const unifiedTotal = folderCount + comicCount;

  const layout = useMemo(() => {
    const availWidth = Math.max(220, viewport.width - PADDING * 2);
    const columns = Math.max(1, Math.floor((availWidth + GAP) / (cardMinWidth + GAP)));
    const cardWidth = Math.floor((availWidth - GAP * (columns - 1)) / columns);
    const coverHeight = Math.round(cardWidth * COVER_ASPECT);
    const cardHeight = coverHeight + META_HEIGHT;
    const rowHeight = cardHeight + GAP;
    const totalRows = Math.ceil(unifiedTotal / columns);
    const totalHeight =
      totalRows > 0 ? PADDING * 2 + totalRows * rowHeight - GAP : 0;

    return { columns, cardWidth, coverHeight, cardHeight, rowHeight, totalRows, totalHeight };
  }, [viewport.width, cardMinWidth, unifiedTotal]);

  const visibleRange = useMemo(() => {
    if (unifiedTotal === 0 || layout.totalRows === 0) {
      return { startRow: 0, endRow: -1, startIndex: 0, endIndex: -1 };
    }
    const effectiveScroll = Math.max(0, scrollTop - PADDING);
    const startRow = Math.max(0, Math.floor(effectiveScroll / layout.rowHeight) - 1);
    const endRow = Math.min(
      layout.totalRows - 1,
      Math.ceil((scrollTop + viewport.height - PADDING) / layout.rowHeight)
    );
    const startIndex = startRow * layout.columns;
    const endIndex = Math.min(unifiedTotal - 1, (endRow + 1) * layout.columns - 1);
    return { startRow, endRow, startIndex, endIndex };
  }, [scrollTop, viewport.height, layout, unifiedTotal]);

  useEffect(() => {
    if (isFolderRoot) return;
    const comicStartOffset = foldersFirst ? folderCount : 0;
    const comicMaxEnd = foldersFirst ? visibleRange.endIndex : Math.min(visibleRange.endIndex, comicCount - 1);
    if (comicMaxEnd < comicStartOffset) return;

    const comicStart = Math.max(0, visibleRange.startIndex - comicStartOffset);
    const comicEnd = comicMaxEnd - comicStartOffset;
    if (comicEnd < comicStart) return;

    const gen = fetchGenRef.current;
    const controller = new AbortController();
    scrollFetchControllerRef.current = controller;

    const firstPage = Math.floor(comicStart / PAGE_SIZE);
    const lastPage = Math.floor(comicEnd / PAGE_SIZE);

    for (let page = firstPage; page <= lastPage; page++) {
      if (loadedPagesRef.current.has(page) || loadingPagesRef.current.has(page)) {
        continue;
      }
      loadingPagesRef.current.add(page);
      const offset = page * PAGE_SIZE;

      api
        .queryLibrary(
          {
            search,
            smartview,
            shelfId,
            folderPrefix: activeFolderPrefix,
            author: authorFilter,
            tag: tagFilter,
            sort,
            includeHidden,
            offset,
            limit: PAGE_SIZE,
          },
          controller.signal
        )
        .then((res) => {
          if (controller.signal.aborted || fetchGenRef.current !== gen) return;
          (res.items || []).forEach((item, idx) => {
            itemsRef.current.set(offset + idx, item);
          });
          loadedPagesRef.current.add(page);
          loadingPagesRef.current.delete(page);
          setCacheTick((v) => v + 1);
        })
        .catch(() => {
          loadingPagesRef.current.delete(page);
        });
    }

    return () => {
      controller.abort();
      if (scrollFetchControllerRef.current === controller) {
        scrollFetchControllerRef.current = null;
      }
    };
  }, [
    isFolderRoot,
    foldersFirst,
    folderCount,
    comicCount,
    visibleRange.startIndex,
    visibleRange.endIndex,
    search,
    smartview,
    shelfId,
    activeFolderPrefix,
    authorFilter,
    tagFilter,
    sort,
    includeHidden,
  ]);

  useEffect(() => {
    const currentVisibleIds = new Set();
    for (let i = visibleRange.startIndex; i <= visibleRange.endIndex; i++) {
      if (foldersFirst) {
        if (i < folderCount) {
          const previewId = subfolders[i]?.preview_comic_id;
          if (previewId > 0) {
            currentVisibleIds.add(previewId);
            pendingCancelIdsRef.current.delete(previewId);
          }
        } else if (!isFolderRoot) {
          const comicIndex = i - folderCount;
          const item = itemsRef.current.get(comicIndex);
          if (item?.id != null) {
            currentVisibleIds.add(item.id);
            pendingCancelIdsRef.current.delete(item.id);
          }
        }
      } else {
        if (!isFolderRoot && i < comicCount) {
          const item = itemsRef.current.get(i);
          if (item?.id != null) {
            currentVisibleIds.add(item.id);
            pendingCancelIdsRef.current.delete(item.id);
          }
        } else {
          const folderIdx = i - comicCount;
          const previewId = subfolders[folderIdx]?.preview_comic_id;
          if (previewId > 0) {
            currentVisibleIds.add(previewId);
            pendingCancelIdsRef.current.delete(previewId);
          }
        }
      }
    }

    for (const prevId of prevVisibleIdsRef.current) {
      if (!currentVisibleIds.has(prevId)) {
        pendingCancelIdsRef.current.add(prevId);
      }
    }
    prevVisibleIdsRef.current = currentVisibleIds;

    if (viewportTimerRef.current) clearTimeout(viewportTimerRef.current);
    const visibleList = Array.from(currentVisibleIds);
    viewportTimerRef.current = setTimeout(() => {
      api.setCoversViewport(visibleList);
    }, 75);

    if (pendingCancelIdsRef.current.size > 0) {
      if (cancelTimerRef.current) clearTimeout(cancelTimerRef.current);
      cancelTimerRef.current = setTimeout(() => {
        const idsToCancel = Array.from(pendingCancelIdsRef.current);
        pendingCancelIdsRef.current.clear();
        if (idsToCancel.length > 0) {
          api.cancelCovers(idsToCancel);
        }
      }, 75);
    }

    return () => {
      if (cancelTimerRef.current) clearTimeout(cancelTimerRef.current);
      if (viewportTimerRef.current) clearTimeout(viewportTimerRef.current);
    };
  }, [
    visibleRange.startIndex,
    visibleRange.endIndex,
    totalCount,
    subfolders,
    folderCount,
    comicCount,
    foldersFirst,
    isFolderRoot,
  ]);

  const handleScroll = useCallback(() => {
    if (!isScrollingRef.current) {
      isScrollingRef.current = true;
      setIsScrolling(true);
    }
    if (scrollIdleTimerRef.current) {
      clearTimeout(scrollIdleTimerRef.current);
    }
    scrollIdleTimerRef.current = setTimeout(() => {
      scrollIdleTimerRef.current = null;
      isScrollingRef.current = false;
      setIsScrolling(false);
    }, 90);

    if (rafRef.current !== null) return;
    rafRef.current = requestAnimationFrame(() => {
      rafRef.current = null;
      if (containerRef.current) {
        setScrollTop(containerRef.current.scrollTop);
      }
    });
  }, []);

  useEffect(() => {
    const el = containerRef.current;
    if (!el) return;
    el.addEventListener('scroll', handleScroll, { passive: true });
    return () => {
      el.removeEventListener('scroll', handleScroll);
      if (rafRef.current !== null) cancelAnimationFrame(rafRef.current);
      if (scrollIdleTimerRef.current) clearTimeout(scrollIdleTimerRef.current);
    };
  }, [handleScroll]);

  const handleToggleFavorite = useCallback(
    async (e, comic, index) => {
      e.stopPropagation();
      const nextFav = !comic.is_favorite;
      const optimistic = { ...comic, is_favorite: nextFav };
      itemsRef.current.set(index, optimistic);
      setCacheTick((v) => v + 1);
      try {
        const updated = await api.updateProgress(comic.id, { is_favorite: nextFav });
        if (updated?.id) {
          itemsRef.current.set(index, updated);
          setCacheTick((v) => v + 1);
        }
        onComicMutated?.();
      } catch {
        itemsRef.current.set(index, comic);
        setCacheTick((v) => v + 1);
      }
    },
    [onComicMutated]
  );

  const applyRangeSelect = useCallback((fromIdx, toIdx) => {
    const lo = Math.min(fromIdx, toIdx);
    const hi = Math.max(fromIdx, toIdx);
    setSelectedIds((prev) => {
      const next = new Set(prev);
      for (let i = lo; i <= hi; i++) {
        const item = itemsRef.current.get(i);
        if (item?.id != null) {
          next.add(item.id);
        }
      }
      return next;
    });
  }, []);

  const toggleSingleSelect = useCallback((comic, index) => {
    setSelectedIds((prev) => {
      const next = new Set(prev);
      if (next.has(comic.id)) next.delete(comic.id);
      else next.add(comic.id);
      return next;
    });
    lastSelectedIndexRef.current = index;
  }, []);

  const handleCardClick = useCallback(
    (e, comic, index) => {
      if (e.shiftKey) {
        const anchor = lastSelectedIndexRef.current ?? index;
        applyRangeSelect(anchor, index);
        lastSelectedIndexRef.current = index;
        return;
      }
      if (e.ctrlKey || e.metaKey) {
        toggleSingleSelect(comic, index);
        return;
      }
      onOpenComic?.(comic);
    },
    [applyRangeSelect, toggleSingleSelect, onOpenComic]
  );

  const handleToggleSelect = useCallback(
    (e, comic, index) => {
      if (e.shiftKey && lastSelectedIndexRef.current !== null) {
        applyRangeSelect(lastSelectedIndexRef.current, index);
        lastSelectedIndexRef.current = index;
        return;
      }
      toggleSingleSelect(comic, index);
    },
    [applyRangeSelect, toggleSingleSelect]
  );

  const handleSelectLoaded = useCallback(() => {
    const next = new Set();
    itemsRef.current.forEach((item) => {
      if (item?.id != null) next.add(item.id);
    });
    setSelectedIds(next);
  }, []);

  const runBatchAction = useCallback(
    async (payload) => {
      const comicIds = Array.from(selectedIds);
      if (comicIds.length === 0 || batchBusy) return;
      setBatchBusy(true);
      setShelfMenuOpen(false);
      try {
        await api.batchAction({ comic_ids: comicIds, ...payload });
        // Update cached items in-place for instant feedback
        itemsRef.current.forEach((item, idx) => {
          if (!item || !selectedIds.has(item.id)) return;
          if (payload.action === 'set_status') {
            itemsRef.current.set(idx, { ...item, status: payload.status });
          } else if (payload.action === 'set_favorite') {
            itemsRef.current.set(idx, { ...item, is_favorite: payload.is_favorite });
          } else if (payload.action === 'pin_offline') {
            itemsRef.current.set(idx, { ...item, is_offline_pinned: payload.pinned });
          }
        });
        setCacheTick((v) => v + 1);
        if (payload.action === 'assign_shelf' || payload.action === 'remove_shelf') {
          await onRefreshShelves?.();
        }
        onComicMutated?.();
        setSelectedIds(new Set());
        lastSelectedIndexRef.current = null;
      } finally {
        setBatchBusy(false);
      }
    },
    [selectedIds, batchBusy, onRefreshShelves, onComicMutated]
  );

  const handleEnterFolder = useCallback((folder) => {
    setFolderPrefix(folder.path);
    setFolderTrail((prev) => [...prev, { name: folder.name, path: folder.path }]);
  }, []);

  const handleResetToFolderRoot = useCallback(() => {
    setViewMode('folders');
    setFolderPrefix('');
    setFolderTrail([]);
  }, []);

  const visibleItems = [];
  for (let i = visibleRange.startIndex; i <= visibleRange.endIndex; i++) {
    if (foldersFirst) {
      if (i < folderCount) {
        visibleItems.push({ index: i, kind: 'folder', folder: subfolders[i] });
      } else {
        const comicIndex = i - folderCount;
        visibleItems.push({
          index: i,
          kind: 'comic',
          comicIndex,
          comic: itemsRef.current.get(comicIndex) || null,
        });
      }
    } else {
      if (i < comicCount) {
        visibleItems.push({
          index: i,
          kind: 'comic',
          comicIndex: i,
          comic: itemsRef.current.get(i) || null,
        });
      } else {
        const folderIdx = i - comicCount;
        visibleItems.push({ index: i, kind: 'folder', folder: subfolders[folderIdx] });
      }
    }
  }

  const isLoadingEmpty =
    unifiedTotal === 0 && (isFolderRoot ? foldersLoading : initialLoading);

  const selectionMode = selectedIds.size > 0;

  return (
    <div className="relative flex-1 flex flex-col min-h-0 overflow-hidden bg-[#F4F6F9] dark:bg-[#0B0D10]">
      {/* Compact Library Sub-Toolbar: View Toggle + Clean Root-Relative Breadcrumbs */}
      <div className="shrink-0 px-6 py-2.5 border-b border-[#DCE1E8] dark:border-[#222834] bg-white dark:bg-[#12151B] flex items-center justify-between gap-3 flex-wrap">
        {/* View Toggle */}
        <div className="inline-flex items-center rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] p-0.5 text-xs font-medium">
          <button
            type="button"
            onClick={() => setViewMode('grid')}
            className={`inline-flex items-center gap-1.5 px-2.5 py-1 rounded-md transition-colors ${
              viewMode === 'grid'
                ? 'bg-white dark:bg-[#171C24] text-cyan-700 dark:text-[#22D3EE] border border-[#DCE1E8] dark:border-[#222834]'
                : 'text-slate-600 dark:text-slate-400 hover:text-slate-900 dark:hover:text-[#F1F5F9] border border-transparent'
            }`}
          >
            <LayoutGrid className="w-3.5 h-3.5" />
            <span>{t('library.allGrid')}</span>
          </button>
          <button
            type="button"
            onClick={handleResetToFolderRoot}
            className={`inline-flex items-center gap-1.5 px-2.5 py-1 rounded-md transition-colors ${
              viewMode === 'folders'
                ? 'bg-white dark:bg-[#171C24] text-cyan-700 dark:text-[#22D3EE] border border-[#DCE1E8] dark:border-[#222834]'
                : 'text-slate-600 dark:text-slate-400 hover:text-slate-900 dark:hover:text-[#F1F5F9] border border-transparent'
            }`}
          >
            <FolderTree className="w-3.5 h-3.5" />
            <span>{t('library.folderView')}</span>
          </button>
        </div>

        {/* Clickable Root-Relative Breadcrumb Bar (Folder / Series View) */}
        {viewMode === 'folders' && (
          <nav
            aria-label="Folder breadcrumb"
            className="flex items-center gap-1 text-xs font-mono overflow-x-auto py-0.5"
          >
            <button
              type="button"
              onClick={handleResetToFolderRoot}
              className={`px-2 py-1 rounded-md transition-colors shrink-0 ${
                folderPrefix === ''
                  ? 'bg-cyan-500/10 dark:bg-[#22D3EE]/10 text-cyan-700 dark:text-[#22D3EE] font-semibold'
                  : 'text-slate-600 dark:text-slate-300 hover:bg-[#F4F6F9] dark:hover:bg-[#171C24]'
              }`}
            >
              {t('library.masterRoot', { defaultValue: 'Folders' })}
            </button>
            {folderTrail.map((seg, idx) => {
              const isLast = idx === folderTrail.length - 1;
              return (
                <React.Fragment key={seg.path}>
                  <ChevronRight className="w-3.5 h-3.5 text-slate-400 shrink-0" />
                  <button
                    type="button"
                    onClick={() => {
                      setFolderPrefix(seg.path);
                      setFolderTrail((prev) => prev.slice(0, idx + 1));
                    }}
                    className={`px-2 py-1 rounded-md transition-colors shrink-0 ${
                      isLast
                        ? 'bg-cyan-500/10 dark:bg-[#22D3EE]/10 text-cyan-700 dark:text-[#22D3EE] font-semibold'
                        : 'text-slate-600 dark:text-slate-300 hover:bg-[#F4F6F9] dark:hover:bg-[#171C24]'
                    }`}
                  >
                    {seg.name}
                  </button>
                </React.Fragment>
              );
            })}
          </nav>
        )}

        {/* Active Author / Tag Filter Pills */}
        {(authorFilter || tagFilter) && (
          <div className="flex items-center gap-1.5 flex-wrap">
            {authorFilter && (
              <span className="inline-flex items-center gap-1.5 px-2.5 py-1 rounded-full text-xs font-medium bg-cyan-500/10 dark:bg-[#22D3EE]/10 text-cyan-700 dark:text-[#22D3EE] border border-cyan-600/30 dark:border-[#22D3EE]/30">
                <User className="w-3.5 h-3.5 shrink-0" />
                <span className="truncate max-w-[180px]">Author: {authorFilter}</span>
                <button
                  type="button"
                  onClick={() => onClearAuthor?.()}
                  className="hover:opacity-75"
                  title="Clear author filter"
                >
                  <X className="w-3 h-3" />
                </button>
              </span>
            )}
            {tagFilter && (
              <span className="inline-flex items-center gap-1.5 px-2.5 py-1 rounded-full text-xs font-medium bg-slate-200/70 dark:bg-[#171C24] text-slate-700 dark:text-slate-200 border border-[#DCE1E8] dark:border-[#222834]">
                <Tag className="w-3.5 h-3.5 shrink-0 text-cyan-600 dark:text-[#22D3EE]" />
                <span className="truncate max-w-[180px]">Tag: {tagFilter}</span>
                <button
                  type="button"
                  onClick={() => onClearTag?.()}
                  className="hover:opacity-75"
                  title="Clear tag filter"
                >
                  <X className="w-3 h-3" />
                </button>
              </span>
            )}
          </div>
        )}
      </div>

      {/* Continue Reading Banner */}
      {enableContinueReading && continueItems.length > 0 && viewMode === 'grid' && (
        <div className="shrink-0 px-6 py-3 border-b border-[#DCE1E8] dark:border-[#222834] bg-white/70 dark:bg-[#12151B]/70">
          <div className="flex items-center gap-2 mb-2">
            <History className="w-3.5 h-3.5 text-cyan-600 dark:text-[#22D3EE]" />
            <h3 className="text-[11px] font-bold uppercase tracking-wider text-slate-600 dark:text-slate-300">
              Continue Reading
            </h3>
          </div>
          <div className="flex items-stretch gap-3 overflow-x-auto pb-1">
            {continueItems.map((item) => {
              const padNum = (n) => String(n).padStart(3, '0');
              const pct =
                item.page_count > 0
                  ? Math.min(100, Math.round((item.current_page / item.page_count) * 100))
                  : item.status === 'read'
                    ? 100
                    : 0;
              return (
                <button
                  key={`cr-${item.id}`}
                  type="button"
                  onClick={() => onOpenComic?.(item)}
                  className="group shrink-0 w-56 flex items-center gap-2.5 p-2 rounded-lg bg-white dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] hover:border-cyan-600/80 dark:hover:border-[#22D3EE]/80 transition-colors text-left"
                >
                  <div className="relative w-11 h-16 rounded overflow-hidden bg-slate-200 dark:bg-[#171C24] shrink-0">
                    <img
                      src={`/api/covers/${item.id}${coverVersion ? `?v=${coverVersion}` : ''}`}
                      alt={item.title}
                      loading="lazy"
                      decoding="async"
                      className="w-full h-full object-cover"
                    />
                  </div>
                  <div className="min-w-0 flex-1 flex flex-col justify-between h-14">
                    <div className="text-xs font-semibold text-slate-900 dark:text-[#F1F5F9] line-clamp-2 leading-tight group-hover:text-cyan-700 dark:group-hover:text-[#22D3EE]">
                      {formatDisplayTitle(item.title, smartRename)}
                    </div>
                    <div className="space-y-1">
                      <div className="flex items-center justify-between text-[10px] font-mono tabular-nums text-slate-500 dark:text-slate-400">
                        <span>
                          {item.page_count > 0
                            ? `${padNum(item.current_page)} / ${padNum(item.page_count)}`
                            : item.status}
                        </span>
                        <span>{pct}%</span>
                      </div>
                      <div className="h-1 w-full rounded-full bg-slate-200 dark:bg-[#171C24] overflow-hidden">
                        <div
                          className="h-full bg-cyan-600 dark:bg-[#22D3EE]"
                          style={{ width: `${pct}%` }}
                        />
                      </div>
                    </div>
                  </div>
                </button>
              );
            })}
          </div>
        </div>
      )}

      {/* Unified 60-120fps Virtualized Grid (Folders + Comics) */}
      <div
        ref={containerRef}
        className="relative flex-1 overflow-y-auto"
      >
        {isLoadingEmpty ? (
          <div className="h-full flex items-center justify-center font-mono text-xs text-slate-500 dark:text-slate-400">
            {t('app.loading')}
          </div>
        ) : unifiedTotal === 0 ? (
          <div className="h-full flex flex-col items-center justify-center text-center px-6">
            <img src="/logo.svg" alt="Incuna Reader" className="w-16 h-16 mb-5" />
            <h3 className="text-base font-bold tracking-tight text-slate-900 dark:text-[#F1F5F9]">
              {t('app.emptyTitle')}
            </h3>
            <p className="mt-1.5 text-xs text-slate-500 dark:text-slate-400 max-w-md leading-relaxed">
              {t('app.emptySubtitle')}
            </p>
            <div className="mt-5 flex items-center gap-2.5">
              <button
                type="button"
                onClick={onPickFolder}
                className="inline-flex items-center gap-2 px-4 py-2 rounded-lg bg-cyan-600 dark:bg-[#22D3EE] hover:opacity-90 text-white dark:text-[#0B0D10] text-xs font-semibold transition-opacity"
              >
                <FolderPlus className="w-4 h-4" />
                <span>{t('sidebar.importFolder')}</span>
              </button>
              <button
                type="button"
                onClick={onPickFiles}
                className="inline-flex items-center gap-2 px-4 py-2 rounded-lg bg-white dark:bg-[#12151B] border border-[#DCE1E8] dark:border-[#222834] hover:border-cyan-600 dark:hover:border-[#22D3EE] text-slate-700 dark:text-[#F1F5F9] text-xs font-semibold transition-colors"
              >
                <FilePlus2 className="w-4 h-4 text-cyan-600 dark:text-[#22D3EE]" />
                <span>{t('sidebar.openFile')}</span>
              </button>
            </div>
          </div>
        ) : (
          <div style={{ height: `${layout.totalHeight}px` }} className="relative w-full">
            {visibleItems.map((entry) => {
              const row = Math.floor(entry.index / layout.columns);
              const col = entry.index % layout.columns;
              const top = PADDING + row * layout.rowHeight;
              const left = PADDING + col * (layout.cardWidth + GAP);

              if (entry.kind === 'folder') {
                const folder = entry.folder;
                return (
                  <div
                    key={`folder-${folder.path}`}
                    style={{
                      position: 'absolute',
                      top: 0,
                      left: 0,
                      transform: `translate3d(${left}px, ${top}px, 0)`,
                      willChange: 'transform',
                      width: `${layout.cardWidth}px`,
                      height: `${layout.cardHeight}px`,
                      contain: 'layout style paint',
                    }}
                  >
                    <button
                      type="button"
                      onClick={() => handleEnterFolder(folder)}
                      className="group relative w-full h-full rounded-lg bg-white dark:bg-[#12151B] border border-[#DCE1E8] dark:border-[#222834] hover:border-cyan-600/80 dark:hover:border-[#22D3EE]/80 overflow-hidden text-left flex flex-col cursor-pointer"
                    >
                      <div
                        style={{ height: `${layout.coverHeight}px` }}
                        className="relative w-full bg-slate-200 dark:bg-[#0B0D10] overflow-hidden flex items-center justify-center shrink-0"
                      >
                        {folder.preview_comic_id > 0 ? (
                          <CoverImage
                            comicId={folder.preview_comic_id}
                            coverVersion={coverVersion}
                            title={folder.name}
                            deferCover={isScrolling}
                            isFolder
                          />
                        ) : (
                          <Folder className="w-12 h-12 text-cyan-600/70 dark:text-[#22D3EE]/70" />
                        )}
                        <div className="absolute top-2 left-2 z-20 p-1.5 rounded-md bg-[#0B0D10]/80 border border-[#222834] text-[#22D3EE]">
                          <Folder className="w-4 h-4" />
                        </div>
                        <span className="absolute bottom-2 right-2 z-20 px-2 py-0.5 rounded text-[11px] font-mono font-semibold tabular-nums bg-[#0B0D10]/85 border border-[#222834] text-[#F1F5F9]">
                          {folder.comic_count.toLocaleString()}
                        </span>
                      </div>
                      <div className="px-2.5 py-2 flex-1 flex items-center justify-between gap-2 min-w-0">
                        <span
                          title={folder.name}
                          className="text-xs font-semibold text-slate-900 dark:text-[#F1F5F9] line-clamp-2 leading-snug"
                        >
                          {formatDisplayTitle(folder.name, smartRename)}
                        </span>
                        <ChevronRight className="w-3.5 h-3.5 text-slate-400 group-hover:text-cyan-600 dark:group-hover:text-[#22D3EE] shrink-0" />
                      </div>
                    </button>
                  </div>
                );
              }

              const { comic, comicIndex } = entry;
              return (
                <div
                  key={comic ? comic.id : `skeleton-${comicIndex}`}
                  style={{
                    position: 'absolute',
                    top: 0,
                    left: 0,
                    transform: `translate3d(${left}px, ${top}px, 0)`,
                    willChange: 'transform',
                    width: `${layout.cardWidth}px`,
                    height: `${layout.cardHeight}px`,
                    contain: 'layout style paint',
                  }}
                >
                  <ComicCard
                    comic={comic}
                    index={comicIndex}
                    coverHeight={layout.coverHeight}
                    coverVersion={coverVersion}
                    deferCover={isScrolling}
                    isSelected={comic ? selectedIds.has(comic.id) : false}
                    selectionMode={selectionMode}
                    circularProgressBar={circularProgressBar}
                    smartRename={smartRename}
                    enableMetadataExtractor={enableMetadataExtractor}
                    onOpenComic={stableOpenComic}
                    onCardClick={handleCardClick}
                    onToggleSelect={handleToggleSelect}
                    onContextMenu={stableContextMenu}
                    onToggleFavorite={handleToggleFavorite}
                    onFilterAuthor={onFilterAuthor}
                  />
                </div>
              );
            })}
          </div>
        )}
      </div>

      {/* Floating Multi-Select Batch Action Toolbar */}
      {selectionMode && (
        <div className="absolute bottom-5 left-1/2 -translate-x-1/2 z-40 flex items-center gap-2 px-3.5 py-2 rounded-xl bg-white/95 dark:bg-[#12151B]/95 backdrop-blur-md border border-cyan-600/40 dark:border-[#22D3EE]/40 shadow-2xl text-xs">
          <span className="font-mono font-semibold text-cyan-700 dark:text-[#22D3EE] px-2 py-1 rounded-md bg-cyan-500/10 dark:bg-[#22D3EE]/10 tabular-nums">
            {t('library.selectedCount', { count: selectedIds.size })}
          </span>

          <button
            type="button"
            disabled={batchBusy}
            onClick={handleSelectLoaded}
            className="inline-flex items-center gap-1 px-2 py-1.5 rounded-lg text-slate-600 dark:text-slate-300 hover:bg-[#F4F6F9] dark:hover:bg-[#171C24] transition-colors"
          >
            <CheckSquare className="w-3.5 h-3.5" />
            <span className="hidden sm:inline">{t('library.selectLoaded')}</span>
          </button>

          <div className="h-4 w-px bg-[#DCE1E8] dark:bg-[#222834]" />

          {/* Assign to Shelf */}
          <div className="relative">
            <button
              type="button"
              disabled={batchBusy}
              onClick={() => setShelfMenuOpen((o) => !o)}
              className="inline-flex items-center gap-1.5 px-2.5 py-1.5 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] hover:border-cyan-600 dark:hover:border-[#22D3EE] text-slate-700 dark:text-[#F1F5F9] font-medium transition-colors"
            >
              <Bookmark className="w-3.5 h-3.5 text-cyan-600 dark:text-[#22D3EE]" />
              <span>{t('library.assignShelf')}</span>
            </button>
            {shelfMenuOpen && (
              <div className="absolute bottom-full mb-2 left-0 w-48 rounded-xl bg-white dark:bg-[#1a1f2e] border border-[#DCE1E8] dark:border-[#222834] shadow-2xl py-1 max-h-48 overflow-y-auto">
                {shelves.length === 0 ? (
                  <div className="px-3 py-1.5 text-[11px] text-slate-400 italic">
                    {t('contextMenu.noShelves')}
                  </div>
                ) : (
                  shelves.map((shelf) => (
                    <button
                      key={shelf.id}
                      type="button"
                      onClick={() =>
                        runBatchAction({ action: 'assign_shelf', shelf_id: shelf.id })
                      }
                      className="w-full px-3 py-1.5 text-left flex items-center gap-2 hover:bg-slate-100 dark:hover:bg-slate-800 text-slate-700 dark:text-slate-200 truncate"
                    >
                      <Bookmark className="w-3.5 h-3.5 text-slate-400 shrink-0" />
                      <span className="truncate">{shelf.name}</span>
                    </button>
                  ))
                )}
              </div>
            )}
          </div>

          <button
            type="button"
            disabled={batchBusy}
            onClick={() => runBatchAction({ action: 'set_status', status: 'read' })}
            className="inline-flex items-center gap-1.5 px-2.5 py-1.5 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] hover:border-emerald-500 text-slate-700 dark:text-[#F1F5F9] font-medium transition-colors"
          >
            <CheckCircle2 className="w-3.5 h-3.5 text-emerald-500" />
            <span>{t('library.markRead')}</span>
          </button>

          <button
            type="button"
            disabled={batchBusy}
            onClick={() => runBatchAction({ action: 'set_status', status: 'unread' })}
            className="inline-flex items-center gap-1.5 px-2.5 py-1.5 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] hover:border-slate-400 text-slate-700 dark:text-[#F1F5F9] font-medium transition-colors"
          >
            <Circle className="w-3.5 h-3.5 text-slate-400" />
            <span>{t('library.markUnread')}</span>
          </button>

          <button
            type="button"
            disabled={batchBusy}
            onClick={() => runBatchAction({ action: 'set_favorite', is_favorite: true })}
            className="inline-flex items-center gap-1.5 px-2.5 py-1.5 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] hover:border-amber-500 text-slate-700 dark:text-[#F1F5F9] font-medium transition-colors"
          >
            <Star className="w-3.5 h-3.5 text-amber-500" />
            <span>{t('library.favorite')}</span>
          </button>

          <button
            type="button"
            disabled={batchBusy}
            onClick={() => runBatchAction({ action: 'pin_offline', pinned: true })}
            className="inline-flex items-center gap-1.5 px-2.5 py-1.5 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] hover:border-sky-500 text-slate-700 dark:text-[#F1F5F9] font-medium transition-colors"
          >
            <CloudDownload className="w-3.5 h-3.5 text-sky-500" />
            <span>{t('library.pinOffline')}</span>
          </button>

          <button
            type="button"
            title={t('library.clearSelection')}
            onClick={() => {
              setSelectedIds(new Set());
              setShelfMenuOpen(false);
              lastSelectedIndexRef.current = null;
            }}
            className="p-1.5 rounded-lg text-slate-400 hover:text-slate-700 dark:hover:text-[#F1F5F9] hover:bg-[#F4F6F9] dark:hover:bg-[#171C24] transition-colors"
          >
            <X className="w-4 h-4" />
          </button>
        </div>
      )}
    </div>
  );
}
