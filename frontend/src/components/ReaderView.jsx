import React, { memo, useEffect, useState, useRef, useCallback } from 'react';
import { useTranslation } from 'react-i18next';
import { ChevronLeft, ChevronRight, ArrowRight } from 'lucide-react';
import { api, isAbortError } from '../api/client';
import { formatDisplayTitle } from '../utils/format';
import { useReaderShortcuts } from '../hooks/useKeyboardShortcuts';
import ReaderToolbar from './ReaderToolbar';
import BookmarksDrawer from './BookmarksDrawer';
import MangaPage from './MangaPage';

const LOUPE_SIZE = 220;
const LOUPE_MIN_ZOOM = 2.5;
const LOUPE_MAX_ZOOM = 3.5;
const LOUPE_DEFAULT_ZOOM = 3.0;
const AUTO_SCROLL_SPEEDS = [0, 45, 90, 150, 230, 340]; // px/sec for 1x..5x

const NIGHT_MODE_FILTER =
  'invert(0.92) hue-rotate(180deg) sepia(0.18) brightness(0.95) contrast(1.05)';

function getRenderedMediaRect(el, srcW, srcH) {
  const rect = el.getBoundingClientRect();
  if (!rect.width || !rect.height || !srcW || !srcH) return rect;
  const boxRatio = rect.width / rect.height;
  const mediaRatio = srcW / srcH;
  if (Math.abs(boxRatio - mediaRatio) < 0.01) return rect;

  if (mediaRatio > boxRatio) {
    const renderedH = rect.width / mediaRatio;
    const offsetY = (rect.height - renderedH) / 2;
    return {
      left: rect.left,
      top: rect.top + offsetY,
      right: rect.right,
      bottom: rect.top + offsetY + renderedH,
      width: rect.width,
      height: renderedH,
    };
  } else {
    const renderedW = rect.height * mediaRatio;
    const offsetX = (rect.width - renderedW) / 2;
    return {
      left: rect.left + offsetX,
      top: rect.top,
      right: rect.left + offsetX + renderedW,
      bottom: rect.bottom,
      width: renderedW,
      height: rect.height,
    };
  }
}

const VerticalPageSlot = memo(function VerticalPageSlot({
  pageIndex,
  pageUrl,
  isMounted,
  mangaEnhancer,
  nightMode,
  fit,
  zoom,
  onVisible,
}) {
  const slotRef = useRef(null);
  const [aspectRatio, setAspectRatio] = useState(0.707);

  useEffect(() => {
    const el = slotRef.current;
    if (!el) return;

    const activeObserver = new IntersectionObserver(
      (entries) => {
        if (entries[0]?.isIntersecting) {
          onVisible(pageIndex);
        }
      },
      { threshold: 0.35 }
    );
    activeObserver.observe(el);

    return () => {
      activeObserver.disconnect();
    };
  }, [pageIndex, onVisible]);

  const handleLoad = useCallback((e) => {
    const img = e?.currentTarget;
    if (img?.naturalWidth && img?.naturalHeight) {
      setAspectRatio(img.naturalWidth / img.naturalHeight);
    }
  }, []);

  const widthStyle =
    fit === 'original'
      ? { width: 'auto', maxWidth: 'none' }
      : { width: `${Math.round(100 * zoom)}%`, maxWidth: zoom === 1 ? '960px' : 'none' };

  const placeholderStyle =
    fit === 'original'
      ? { width: 'min(720px, 90vw)', aspectRatio: String(aspectRatio) }
      : { ...widthStyle, aspectRatio: String(aspectRatio) };

  return (
    <div
      ref={slotRef}
      data-page-index={pageIndex}
      className="w-full flex justify-center min-h-[360px]"
    >
      {isMounted ? (
        <MangaPage
          src={pageUrl}
          alt={`Page ${pageIndex + 1}`}
          enhanced={mangaEnhancer}
          onLoad={handleLoad}
          style={{
            ...widthStyle,
            filter: nightMode ? NIGHT_MODE_FILTER : 'none',
          }}
          className="h-auto object-contain select-none shadow-lg"
        />
      ) : (
        <div style={placeholderStyle} className="bg-slate-800/25 rounded" />
      )}
    </div>
  );
});

export default function ReaderView({ comic, activeShelfId = 0, settings = null, onOpenComic, onClose, onProgressSaved }) {
  const { t } = useTranslation();

  const [session, setSession] = useState(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');
  const [currentPage, setCurrentPage] = useState(0);

  const [layout, setLayout] = useState(() => {
    if (settings?.stack_pages_vertically) return 'vertical';
    const saved = localStorage.getItem('koma_reader_layout');
    return saved === 'dual' ? 'dual' : 'single';
  });
  const [coverOffset, setCoverOffset] = useState(() => localStorage.getItem('koma_reader_cover_offset') !== '0');
  const [direction, setDirection] = useState(() => {
    if (settings && typeof settings.default_manga_mode === 'boolean') {
      return settings.default_manga_mode ? 'rtl' : 'ltr';
    }
    return localStorage.getItem('koma_reader_dir') || 'rtl';
  });
  const [fit, setFit] = useState(() => localStorage.getItem('koma_reader_fit') || 'fitPage');
  const [bgColor, setBgColor] = useState(() => localStorage.getItem('koma_reader_bg') || '#000000');
  const [autoCrop, setAutoCrop] = useState(() => {
    if (settings && typeof settings.auto_detect_borders === 'boolean') {
      return settings.auto_detect_borders;
    }
    return localStorage.getItem('koma_reader_crop') === '1';
  });
  const [mangaEnhancer, setMangaEnhancer] = useState(() => {
    if (settings && typeof settings.enable_manga_enhancer === 'boolean') {
      return settings.enable_manga_enhancer;
    }
    return localStorage.getItem('koma_reader_enhancer') === '1';
  });
  const [nightMode, setNightMode] = useState(() => localStorage.getItem('koma_reader_night') === '1');
  const [autoScrollPlaying, setAutoScrollPlaying] = useState(false);
  const [autoScrollSpeed, setAutoScrollSpeed] = useState(() => {
    const saved = Number(localStorage.getItem('koma_reader_autoscroll_speed'));
    return saved >= 1 && saved <= 5 ? saved : 2;
  });
  const [controlsVisible, setControlsVisible] = useState(true);
  const [widePages, setWidePages] = useState(() => new Set());

  const [zoom, setZoom] = useState(1);
  const [pan, setPan] = useState({ x: 0, y: 0 });
  const [isFullscreen, setIsFullscreen] = useState(false);
  const [bookmarksOpen, setBookmarksOpen] = useState(false);
  const [bookmarks, setBookmarks] = useState([]);
  const [bookmarkName, setBookmarkName] = useState('');
  const [adjacent, setAdjacent] = useState(null);

  const viewportRef = useRef(null);
  const verticalContainerRef = useRef(null);
  const bookmarkInputRef = useRef(null);
  const dragRef = useRef({ active: false, startX: 0, startY: 0, origX: 0, origY: 0 });
  const touchRef = useRef({ startX: 0, startY: 0, lastTap: 0, pinchDist: 0, startZoom: 1 });

  // Loupe Magnifier & Auto-Scroll refs (updated via rAF with zero per-frame React re-renders)
  const loupeOverlayRef = useRef(null);
  const loupeCanvasRef = useRef(null);
  const loupeBadgeRef = useRef(null);
  const pointerDownRef = useRef(false);
  const loupeStateRef = useRef({
    active: false,
    rightMouseDown: false,
    keyDown: false,
    x: 0,
    y: 0,
    hasPointer: false,
    zoom: LOUPE_DEFAULT_ZOOM,
    rafId: 0,
  });

  useEffect(() => { localStorage.setItem('koma_reader_layout', layout); }, [layout]);
  useEffect(() => { localStorage.setItem('koma_reader_cover_offset', coverOffset ? '1' : '0'); }, [coverOffset]);
  useEffect(() => { localStorage.setItem('koma_reader_dir', direction); }, [direction]);
  useEffect(() => { localStorage.setItem('koma_reader_fit', fit); }, [fit]);
  useEffect(() => { localStorage.setItem('koma_reader_bg', bgColor); }, [bgColor]);
  useEffect(() => { localStorage.setItem('koma_reader_crop', autoCrop ? '1' : '0'); }, [autoCrop]);
  useEffect(() => { localStorage.setItem('koma_reader_enhancer', mangaEnhancer ? '1' : '0'); }, [mangaEnhancer]);
  useEffect(() => { localStorage.setItem('koma_reader_night', nightMode ? '1' : '0'); }, [nightMode]);
  useEffect(() => { localStorage.setItem('koma_reader_autoscroll_speed', String(autoScrollSpeed)); }, [autoScrollSpeed]);

  const smartPageLayout = settings?.smart_page_layout ?? true;
  const keepScreenActive = settings?.keep_screen_active ?? true;
  const loupeEnabled = settings?.enable_loupe_magnifier ?? true;
  const autoScrollEnabled = settings?.enable_auto_scroll ?? true;

  useEffect(() => {
    if (settings && typeof settings.enable_manga_enhancer === 'boolean') {
      setMangaEnhancer(settings.enable_manga_enhancer);
    }
  }, [settings?.enable_manga_enhancer]);

  useEffect(() => {
    if (!keepScreenActive) return;
    api.setKeepAwake(true).catch(() => {});
    return () => {
      api.setKeepAwake(false).catch(() => {});
    };
  }, [keepScreenActive]);

  useEffect(() => {
    const controller = new AbortController();
    setLoading(true);
    setError('');
    setAdjacent(null);
    setZoom(1);
    setPan({ x: 0, y: 0 });
    setBookmarksOpen(false);
    setAutoScrollPlaying(false);
    setWidePages(new Set());

    const payload = comic.id ? { comic_id: comic.id } : { path: comic.path };
    api
      .openReader(payload, controller.signal)
      .then((res) => {
        if (controller.signal.aborted) return;
        setSession(res);
        const total = res.page_count || 0;
        const startIdx =
          comic.current_page > 0 && total > 0
            ? Math.min(total - 1, Math.max(0, comic.current_page - 1))
            : 0;
        setCurrentPage(startIdx);
        setLoading(false);
      })
      .catch((err) => {
        if (isAbortError(err) || controller.signal.aborted) return;
        setError(err.message || t('reader.error'));
        setLoading(false);
      });

    if (comic.id > 0) {
      api
        .getBookmarks(comic.id, controller.signal)
        .then((list) => {
          if (!controller.signal.aborted) setBookmarks(list || []);
        })
        .catch(() => {});
      api
        .getAdjacentComics(comic.id, activeShelfId, controller.signal)
        .then((adj) => {
          if (!controller.signal.aborted) setAdjacent(adj || null);
        })
        .catch(() => {});
    }

    return () => {
      controller.abort();
      api.closeReader().catch(() => {});
    };
  }, [comic.id, comic.path, activeShelfId, t]);

  const pageCount = session?.page_count || 0;
  const pageMaxWidth = zoom > 1.3 ? 0 : 1800;

  const getPageUrl = useCallback(
    (idx) => api.getReaderPageUrl(idx, autoCrop, pageMaxWidth),
    [autoCrop, pageMaxWidth]
  );

  const markWidePageIfSpread = useCallback((idx, img) => {
    if (!img || !img.naturalWidth || !img.naturalHeight) return;
    if (img.naturalWidth > img.naturalHeight * 1.15) {
      setWidePages((prev) => {
        if (prev.has(idx)) return prev;
        const next = new Set(prev);
        next.add(idx);
        return next;
      });
    }
  }, []);

  useEffect(() => {
    if (!session || pageCount <= 0 || layout === 'vertical') return;
    [currentPage + 1, currentPage + 2, currentPage + 3, currentPage - 1].forEach((idx) => {
      if (idx >= 0 && idx < pageCount) {
        const img = new Image();
        img.onload = () => markWidePageIfSpread(idx, img);
        img.src = getPageUrl(idx);
      }
    });
  }, [session, pageCount, currentPage, layout, getPageUrl, markWidePageIfSpread]);

  const getDualPairStart = useCallback(
    (page) => {
      if (coverOffset) {
        if (page <= 0) return 0;
        return page % 2 === 1 ? page : page - 1;
      }
      return page % 2 === 0 ? page : page - 1;
    },
    [coverOffset]
  );

  const spreadPages = [];
  if (pageCount > 0 && layout !== 'vertical') {
    if (layout === 'dual') {
      if (coverOffset && currentPage === 0) {
        spreadPages.push(0);
      } else if (smartPageLayout) {
        if (widePages.has(currentPage) || (currentPage + 1 < pageCount && widePages.has(currentPage + 1))) {
          spreadPages.push(currentPage);
        } else if (currentPage + 1 < pageCount) {
          spreadPages.push(
            ...(direction === 'rtl' ? [currentPage + 1, currentPage] : [currentPage, currentPage + 1])
          );
        } else {
          spreadPages.push(currentPage);
        }
      } else {
        const pairStart = getDualPairStart(currentPage);
        if (pairStart + 1 < pageCount) {
          spreadPages.push(
            ...(direction === 'rtl' ? [pairStart + 1, pairStart] : [pairStart, pairStart + 1])
          );
        } else {
          spreadPages.push(pairStart);
        }
      }
    } else {
      spreadPages.push(currentPage);
    }
  }

  const atFirstSpread =
    layout === 'dual'
      ? coverOffset
        ? currentPage <= 0
        : getDualPairStart(currentPage) <= 0
      : currentPage <= 0;

  const atLastSpread =
    pageCount <= 0
      ? true
      : layout === 'dual'
        ? spreadPages.includes(pageCount - 1)
        : currentPage >= pageCount - 1;

  const isAtLastPage = !loading && !error && pageCount > 0 && atLastSpread;

  useEffect(() => {
    if (!session || !comic.id || pageCount <= 0) return;
    const maxVisible =
      layout === 'dual' && spreadPages.length > 0 ? Math.max(...spreadPages) : currentPage;
    const oneBased = maxVisible + 1;
    const status = oneBased >= pageCount ? 'read' : 'reading';
    const timer = setTimeout(() => {
      api
        .updateProgress(comic.id, {
          current_page: oneBased,
          page_count: pageCount,
          status,
        })
        .then(() => onProgressSaved?.())
        .catch(() => {});
    }, 180);
    return () => clearTimeout(timer);
  }, [session, comic.id, currentPage, pageCount, layout, spreadPages.join(','), onProgressSaved]);

  const isFullscreenRef = useRef(false);
  isFullscreenRef.current = isFullscreen;

  const setNativeFullscreen = useCallback(async (enable) => {
    try {
      const res = await api.setFullscreen(enable);
      const active = Boolean(res?.active);
      setIsFullscreen(active);
      if (active) {
        setControlsVisible(false);
      }
      return;
    } catch {
      // Fallback to browser Fullscreen API if running outside native Win32 host
    }
    if (enable && !document.fullscreenElement) {
      document.documentElement.requestFullscreen?.().catch(() => {});
    } else if (!enable && document.fullscreenElement) {
      document.exitFullscreen?.().catch(() => {});
    }
  }, []);

  useEffect(() => {
    const onFsChange = () => {
      if (document.fullscreenElement) setIsFullscreen(true);
    };
    const onF11 = (e) => {
      if (e.key === 'F11') {
        e.preventDefault();
        setNativeFullscreen(!isFullscreenRef.current);
      }
    };
    document.addEventListener('fullscreenchange', onFsChange);
    window.addEventListener('keydown', onF11);
    return () => {
      document.removeEventListener('fullscreenchange', onFsChange);
      window.removeEventListener('keydown', onF11);
      if (isFullscreenRef.current) {
        api.setFullscreen(false).catch(() => {});
      }
    };
  }, [setNativeFullscreen]);

  const toggleFullscreen = useCallback(() => {
    setNativeFullscreen(!isFullscreenRef.current);
  }, [setNativeFullscreen]);

  const resetZoom = useCallback(() => {
    setZoom(1);
    setPan({ x: 0, y: 0 });
  }, []);

  const zoomIn = useCallback(() => {
    setZoom((z) => Math.min(4, Number((z + 0.25).toFixed(2))));
  }, []);

  const zoomOut = useCallback(() => {
    setZoom((z) => {
      const next = Math.max(0.5, Number((z - 0.25).toFixed(2)));
      if (next <= 1) setPan({ x: 0, y: 0 });
      return next;
    });
  }, []);

  const goToPage = useCallback(
    (target) => {
      if (pageCount <= 0) return;
      const clamped = Math.max(0, Math.min(pageCount - 1, target));
      setCurrentPage(clamped);
      resetZoom();

      if (layout === 'vertical' && verticalContainerRef.current) {
        const el = verticalContainerRef.current.querySelector(
          `[data-page-index="${clamped}"]`
        );
        el?.scrollIntoView({ behavior: 'smooth', block: 'start' });
      }
    },
    [pageCount, layout, resetZoom]
  );

  const goNext = useCallback(() => {
    if (pageCount <= 0) return;
    if (layout === 'dual') {
      if (coverOffset && currentPage === 0) {
        if (pageCount > 1) goToPage(1);
        return;
      }
      if (smartPageLayout) {
        const step = spreadPages.length === 2 ? 2 : 1;
        if (currentPage + step < pageCount) {
          goToPage(currentPage + step);
        }
        return;
      }
      const pairStart = getDualPairStart(currentPage);
      if (pairStart + 2 < pageCount) {
        goToPage(pairStart + 2);
      }
      return;
    }
    goToPage(currentPage + 1);
  }, [pageCount, layout, coverOffset, smartPageLayout, spreadPages.length, currentPage, getDualPairStart, goToPage]);

  const goPrev = useCallback(() => {
    if (pageCount <= 0) return;
    if (layout === 'dual') {
      if (coverOffset && currentPage === 0) return;
      if (smartPageLayout) {
        if (currentPage <= 0) return;
        if ((coverOffset && currentPage === 1) || widePages.has(currentPage - 1) || (currentPage >= 2 && widePages.has(currentPage - 2))) {
          goToPage(currentPage - 1);
        } else {
          goToPage(Math.max(0, currentPage - 2));
        }
        return;
      }
      const pairStart = getDualPairStart(currentPage);
      if (coverOffset && pairStart <= 1) {
        goToPage(0);
      } else {
        goToPage(Math.max(0, pairStart - 2));
      }
      return;
    }
    goToPage(currentPage - 1);
  }, [pageCount, layout, coverOffset, smartPageLayout, widePages, currentPage, getDualPairStart, goToPage]);

  const handleOpenNextVolume = useCallback(() => {
    if (isAtLastPage && adjacent?.next && onOpenComic) {
      onOpenComic(adjacent.next);
    }
  }, [isAtLastPage, adjacent, onOpenComic]);

  useReaderShortcuts({
    shortcuts: settings?.shortcuts,
    direction,
    layout,
    bookmarksOpen,
    onGoNext: goNext,
    onGoPrev: goPrev,
    onFirstPage: () => goToPage(0),
    onLastPage: () => goToPage(pageCount - 1),
    onNextBook: () => {
      if (adjacent?.next && onOpenComic) onOpenComic(adjacent.next);
    },
    onPrevBook: () => {
      if (adjacent?.prev && onOpenComic) onOpenComic(adjacent.prev);
    },
    onToggleFullscreen: toggleFullscreen,
    onToggleDirection: () => setDirection((d) => (d === 'rtl' ? 'ltr' : 'rtl')),
    onCycleLayout: () =>
      setLayout((l) => (l === 'single' ? 'dual' : l === 'dual' ? 'vertical' : 'single')),
    onCycleFit: () => {
      const order = ['fitPage', 'fitWidth', 'fitHeight', 'original'];
      setFit((f) => order[(order.indexOf(f) + 1) % order.length]);
      resetZoom();
    },
    onToggleNightMode: () => setNightMode((v) => !v),
    onToggleControls: () => setControlsVisible((v) => !v),
    onOpenBookmarks: () => {
      setBookmarksOpen(true);
      setTimeout(() => bookmarkInputRef.current?.focus(), 60);
    },
    onCloseBookmarks: () => setBookmarksOpen(false),
    onZoomIn: zoomIn,
    onZoomOut: zoomOut,
    onResetZoom: resetZoom,
    onNextVolume: isAtLastPage && adjacent?.next ? handleOpenNextVolume : undefined,
    onCloseReader: () => {
      if (isFullscreenRef.current) {
        setNativeFullscreen(false);
        return;
      }
      onClose?.();
    },
  });

  // Stop auto-scroll if leaving vertical mode or feature disabled
  useEffect(() => {
    if (layout !== 'vertical' || !autoScrollEnabled) {
      setAutoScrollPlaying(false);
    }
  }, [layout, autoScrollEnabled]);

  // Hands-Free Auto-Scroll loop (requestAnimationFrame with sub-pixel accumulator)
  useEffect(() => {
    if (!autoScrollPlaying || !autoScrollEnabled || layout !== 'vertical') return;

    let rafId = 0;
    let lastTime = performance.now();
    let accum = 0;

    const tick = (now) => {
      const dt = Math.min(0.1, Math.max(0, (now - lastTime) / 1000));
      lastTime = now;

      const container = verticalContainerRef.current;
      const pausedByInteraction =
        pointerDownRef.current ||
        loupeStateRef.current.active ||
        bookmarksOpen ||
        loading;

      if (container && !pausedByInteraction) {
        const pxPerSec = AUTO_SCROLL_SPEEDS[autoScrollSpeed] || AUTO_SCROLL_SPEEDS[2];
        accum += pxPerSec * dt;
        if (accum >= 1) {
          const step = Math.floor(accum);
          accum -= step;
          container.scrollTop += step;

          if (container.scrollTop + container.clientHeight >= container.scrollHeight - 1) {
            setAutoScrollPlaying(false);
            return;
          }
        }
      }

      rafId = requestAnimationFrame(tick);
    };

    rafId = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(rafId);
  }, [autoScrollPlaying, autoScrollEnabled, autoScrollSpeed, layout, bookmarksOpen, loading]);

  // Loupe Magnifier renderer (runs inside rAF, zero React re-renders per frame)
  const renderLoupeFrame = useCallback(() => {
    const state = loupeStateRef.current;
    state.rafId = 0;

    const overlay = loupeOverlayRef.current;
    const canvas = loupeCanvasRef.current;
    const viewport = viewportRef.current;
    if (!overlay || !canvas) return;

    if (!state.active || !loupeEnabled || !viewport) {
      overlay.style.opacity = '0';
      overlay.style.visibility = 'hidden';
      return;
    }

    const { x, y, zoom: loupeZoom } = state;
    const half = LOUPE_SIZE / 2;

    overlay.style.visibility = 'visible';
    overlay.style.opacity = '1';
    overlay.style.transform = `translate3d(${Math.round(x - half)}px, ${Math.round(y - half)}px, 0)`;

    if (loupeBadgeRef.current) {
      loupeBadgeRef.current.textContent = `${loupeZoom.toFixed(1)}x`;
    }

    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const backingSize = Math.round(LOUPE_SIZE * dpr);
    if (canvas.width !== backingSize || canvas.height !== backingSize) {
      canvas.width = backingSize;
      canvas.height = backingSize;
    }

    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    ctx.save();
    ctx.fillStyle = bgColor || '#000000';
    ctx.fillRect(0, 0, backingSize, backingSize);

    // Find the page media element (<img> or WebGL-enhanced <canvas>) under cursor
    const pages = viewport.querySelectorAll('[data-reader-page="true"]');
    let targetEl = null;
    let targetRect = null;
    let srcW = 0;
    let srcH = 0;

    for (let i = 0; i < pages.length; i += 1) {
      const el = pages[i];
      const w = el.tagName === 'IMG' ? el.naturalWidth : el.width;
      const h = el.tagName === 'IMG' ? el.naturalHeight : el.height;
      if (!w || !h) continue;
      const r = getRenderedMediaRect(el, w, h);
      if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) {
        targetEl = el;
        targetRect = r;
        srcW = w;
        srcH = h;
        break;
      }
    }

    if (targetEl && targetRect && targetRect.width > 0 && targetRect.height > 0) {
      const scaleX = srcW / targetRect.width;
      const scaleY = srcH / targetRect.height;

      const centerSrcX = (x - targetRect.left) * scaleX;
      const centerSrcY = (y - targetRect.top) * scaleY;

      const sw = (LOUPE_SIZE / loupeZoom) * scaleX;
      const sh = (LOUPE_SIZE / loupeZoom) * scaleY;
      const sx = centerSrcX - sw / 2;
      const sy = centerSrcY - sh / 2;

      // Clamp source rectangle safely so page edges show background without stretching
      const clampedSx = Math.max(0, sx);
      const clampedSy = Math.max(0, sy);
      const clampedEx = Math.min(srcW, sx + sw);
      const clampedEy = Math.min(srcH, sy + sh);
      const clampedSw = clampedEx - clampedSx;
      const clampedSh = clampedEy - clampedSy;

      if (clampedSw > 0 && clampedSh > 0) {
        const dx = ((clampedSx - sx) / sw) * backingSize;
        const dy = ((clampedSy - sy) / sh) * backingSize;
        const dw = (clampedSw / sw) * backingSize;
        const dh = (clampedSh / sh) * backingSize;

        ctx.imageSmoothingEnabled = true;
        ctx.imageSmoothingQuality = 'high';
        ctx.drawImage(targetEl, clampedSx, clampedSy, clampedSw, clampedSh, dx, dy, dw, dh);
      }
    }

    ctx.restore();
  }, [loupeEnabled, bgColor]);

  const scheduleLoupeFrame = useCallback(() => {
    const state = loupeStateRef.current;
    if (!state.rafId) {
      state.rafId = requestAnimationFrame(renderLoupeFrame);
    }
  }, [renderLoupeFrame]);

  const hideLoupeImmediately = useCallback(() => {
    const state = loupeStateRef.current;
    state.active = false;
    state.rightMouseDown = false;
    state.keyDown = false;
    if (state.rafId) {
      cancelAnimationFrame(state.rafId);
      state.rafId = 0;
    }
    const overlay = loupeOverlayRef.current;
    if (overlay) {
      overlay.style.opacity = '0';
      overlay.style.visibility = 'hidden';
    }
  }, []);

  // Hide loupe if setting is turned off
  useEffect(() => {
    if (!loupeEnabled) {
      hideLoupeImmediately();
    }
  }, [loupeEnabled, hideLoupeImmediately]);

  // Global key & mouse release listeners for Loupe Magnifier (hold L / right-click) & Auto-Scroll pause
  useEffect(() => {
    const onKeyDown = (e) => {
      if (!loupeEnabled) return;
      if (e.repeat || e.ctrlKey || e.metaKey || e.altKey) return;
      if (e.key !== 'l' && e.key !== 'L') return;
      const tag = e.target?.tagName?.toLowerCase();
      if (tag === 'input' || tag === 'textarea' || tag === 'select') return;

      e.preventDefault();
      const state = loupeStateRef.current;
      state.keyDown = true;
      state.active = true;
      if (!state.hasPointer && viewportRef.current) {
        const r = viewportRef.current.getBoundingClientRect();
        state.x = r.left + r.width / 2;
        state.y = r.top + r.height / 2;
      }
      scheduleLoupeFrame();
    };

    const onKeyUp = (e) => {
      if (e.key !== 'l' && e.key !== 'L') return;
      const state = loupeStateRef.current;
      if (!state.keyDown) return;
      state.keyDown = false;
      if (!state.rightMouseDown) {
        hideLoupeImmediately();
      }
    };

    const onWindowMouseUp = (e) => {
      pointerDownRef.current = false;
      dragRef.current.active = false;
      const state = loupeStateRef.current;
      if (state.rightMouseDown && (e.button === 2 || (e.buttons & 2) === 0)) {
        state.rightMouseDown = false;
        if (!state.keyDown) {
          hideLoupeImmediately();
        }
      }
    };

    const onWindowBlur = () => {
      pointerDownRef.current = false;
      dragRef.current.active = false;
      hideLoupeImmediately();
    };

    window.addEventListener('keydown', onKeyDown);
    window.addEventListener('keyup', onKeyUp);
    window.addEventListener('mouseup', onWindowMouseUp);
    window.addEventListener('blur', onWindowBlur);
    return () => {
      window.removeEventListener('keydown', onKeyDown);
      window.removeEventListener('keyup', onKeyUp);
      window.removeEventListener('mouseup', onWindowMouseUp);
      window.removeEventListener('blur', onWindowBlur);
      hideLoupeImmediately();
    };
  }, [loupeEnabled, scheduleLoupeFrame, hideLoupeImmediately]);

  const handleWheel = (e) => {
    if (loupeStateRef.current.active) {
      e.preventDefault();
      const delta = e.deltaY < 0 ? 0.25 : -0.25;
      loupeStateRef.current.zoom = Math.max(
        LOUPE_MIN_ZOOM,
        Math.min(LOUPE_MAX_ZOOM, Number((loupeStateRef.current.zoom + delta).toFixed(2)))
      );
      scheduleLoupeFrame();
      return;
    }

    if (e.ctrlKey || e.metaKey || zoom > 1) {
      if (layout === 'vertical' && !e.ctrlKey && !e.metaKey) return;
      e.preventDefault();
      const delta = e.deltaY < 0 ? 0.15 : -0.15;
      setZoom((z) => {
        const next = Math.max(0.5, Math.min(4, Number((z + delta).toFixed(2))));
        if (next <= 1) setPan({ x: 0, y: 0 });
        return next;
      });
    }
  };

  const handleMouseDown = (e) => {
    pointerDownRef.current = true;

    if (loupeEnabled && e.button === 2) {
      e.preventDefault();
      const state = loupeStateRef.current;
      state.rightMouseDown = true;
      state.active = true;
      state.x = e.clientX;
      state.y = e.clientY;
      state.hasPointer = true;
      scheduleLoupeFrame();
      return;
    }

    if (e.button !== 0 || zoom <= 1 || layout === 'vertical') return;
    dragRef.current = {
      active: true,
      startX: e.clientX,
      startY: e.clientY,
      origX: pan.x,
      origY: pan.y,
    };
  };

  const handleMouseMove = (e) => {
    const state = loupeStateRef.current;
    state.x = e.clientX;
    state.y = e.clientY;
    state.hasPointer = true;

    if (state.active) {
      scheduleLoupeFrame();
    }

    if (!dragRef.current.active) return;
    setPan({
      x: dragRef.current.origX + (e.clientX - dragRef.current.startX),
      y: dragRef.current.origY + (e.clientY - dragRef.current.startY),
    });
  };

  const handleMouseUp = (e) => {
    pointerDownRef.current = false;
    dragRef.current.active = false;
    const state = loupeStateRef.current;
    if (state.rightMouseDown && (!e || e.button === 2 || (e.buttons & 2) === 0)) {
      state.rightMouseDown = false;
      if (!state.keyDown) {
        hideLoupeImmediately();
      }
    }
  };

  const handleMouseLeave = () => {
    dragRef.current.active = false;
    if (loupeStateRef.current.active && !loupeStateRef.current.keyDown) {
      hideLoupeImmediately();
    }
  };

  const handleContextMenu = (e) => {
    if (loupeEnabled || loupeStateRef.current.active) {
      e.preventDefault();
    }
  };

  const handleTouchStart = (e) => {
    if (e.touches.length === 2) {
      const dx = e.touches[0].clientX - e.touches[1].clientX;
      const dy = e.touches[0].clientY - e.touches[1].clientY;
      touchRef.current.pinchDist = Math.hypot(dx, dy);
      touchRef.current.startZoom = zoom;
    } else if (e.touches.length === 1) {
      const now = Date.now();
      if (now - touchRef.current.lastTap < 280) {
        zoom > 1 ? resetZoom() : setZoom(2);
      }
      touchRef.current.lastTap = now;
      touchRef.current.startX = e.touches[0].clientX;
      touchRef.current.startY = e.touches[0].clientY;
    }
  };

  const handleTouchMove = (e) => {
    if (e.touches.length === 2 && touchRef.current.pinchDist > 0) {
      const dx = e.touches[0].clientX - e.touches[1].clientX;
      const dy = e.touches[0].clientY - e.touches[1].clientY;
      const dist = Math.hypot(dx, dy);
      const nextZoom = Math.max(
        0.5,
        Math.min(4, Number((touchRef.current.startZoom * (dist / touchRef.current.pinchDist)).toFixed(2)))
      );
      setZoom(nextZoom);
    }
  };

  const handleTouchEnd = (e) => {
    if (e.changedTouches.length === 1 && zoom <= 1 && layout !== 'vertical') {
      const dx = e.changedTouches[0].clientX - touchRef.current.startX;
      const dy = e.changedTouches[0].clientY - touchRef.current.startY;
      if (Math.abs(dx) > 55 && Math.abs(dx) > Math.abs(dy) * 1.3) {
        if (dx < 0) {
          direction === 'rtl' ? goPrev() : goNext();
        } else {
          direction === 'rtl' ? goNext() : goPrev();
        }
      }
    }
    touchRef.current.pinchDist = 0;
  };

  const handleAddBookmark = async (e) => {
    e.preventDefault();
    const label = bookmarkName.trim() || `Page ${currentPage + 1}`;
    const pageNum = currentPage + 1;

    if (comic.id > 0) {
      try {
        const created = await api.addBookmark(comic.id, pageNum, label);
        setBookmarks((prev) => [...prev, created]);
        setBookmarkName('');
      } catch {
        // ignore
      }
    } else {
      setBookmarks((prev) => [
        ...prev,
        { id: Date.now(), page_number: pageNum, name: label },
      ]);
      setBookmarkName('');
    }
  };

  const handleDeleteBookmark = async (bmId) => {
    if (comic.id > 0) {
      await api.deleteBookmark(comic.id, bmId).catch(() => {});
    }
    setBookmarks((prev) => prev.filter((b) => b.id !== bmId));
  };

  const getFitClass = () => {
    switch (fit) {
      case 'fitWidth':
        return 'w-full h-auto max-h-none';
      case 'fitHeight':
        return 'h-full w-auto max-w-none';
      case 'original':
        return 'w-auto h-auto max-w-none max-h-none';
      case 'fitPage':
      default:
        return 'max-w-full max-h-full w-auto h-auto object-contain';
    }
  };

  const rawTitle =
    comic.title ||
    (comic.path ? comic.path.split(/[\\/]/).pop() : 'Incuna Reader');
  const title = formatDisplayTitle(rawTitle, Boolean(settings?.smart_rename));

  const isLightBg = bgColor === '#ffffff' || bgColor === '#f4ecd8';

  return (
    <div className="fixed inset-0 z-40 flex flex-col bg-black text-slate-100 select-none">
      {controlsVisible && (
        <ReaderToolbar
          title={title}
        layout={layout}
        coverOffset={coverOffset}
        direction={direction}
        fit={fit}
        zoom={zoom}
        bgColor={bgColor}
        autoCrop={autoCrop}
        mangaEnhancer={mangaEnhancer}
        nightMode={nightMode}
        autoScrollEnabled={autoScrollEnabled}
        autoScrollPlaying={autoScrollPlaying}
        autoScrollSpeed={autoScrollSpeed}
        bookmarksOpen={bookmarksOpen}
        bookmarksCount={bookmarks.length}
        isFullscreen={isFullscreen}
        onClose={onClose}
        onLayoutChange={setLayout}
        onToggleCoverOffset={() => setCoverOffset((v) => !v)}
        onToggleDirection={() => setDirection((d) => (d === 'rtl' ? 'ltr' : 'rtl'))}
        onFitChange={(nextFit) => {
          setFit(nextFit);
          resetZoom();
        }}
        onZoomIn={zoomIn}
        onZoomOut={zoomOut}
        onResetZoom={resetZoom}
        onBgColorChange={setBgColor}
        onToggleAutoCrop={() => setAutoCrop((v) => !v)}
        onToggleMangaEnhancer={() => setMangaEnhancer((v) => !v)}
        onToggleNightMode={() => setNightMode((v) => !v)}
        onToggleAutoScroll={() => setAutoScrollPlaying((v) => !v)}
        onAutoScrollSpeedChange={(speed) => setAutoScrollSpeed(Math.max(1, Math.min(5, speed)))}
        onToggleBookmarks={() => setBookmarksOpen((v) => !v)}
        onToggleFullscreen={toggleFullscreen}
      />
      )}

      <div className="relative flex-1 flex overflow-hidden">
        <div
          ref={viewportRef}
          style={{ backgroundColor: bgColor }}
          onWheel={handleWheel}
          onMouseDown={handleMouseDown}
          onMouseMove={handleMouseMove}
          onMouseUp={handleMouseUp}
          onMouseLeave={handleMouseLeave}
          onContextMenu={handleContextMenu}
          onTouchStart={handleTouchStart}
          onTouchMove={handleTouchMove}
          onTouchEnd={handleTouchEnd}
          onDoubleClick={() => (zoom > 1 ? resetZoom() : setZoom(2))}
          className={`relative flex-1 h-full overflow-hidden flex items-center justify-center ${
            zoom > 1 && layout !== 'vertical' ? 'cursor-grab active:cursor-grabbing' : ''
          }`}
        >
          {loading ? (
            <div className={`text-sm ${isLightBg ? 'text-slate-700' : 'text-slate-300'}`}>
              {t('reader.loading')}
            </div>
          ) : error ? (
            <div className="px-4 py-3 rounded-xl bg-rose-500/15 border border-rose-500/40 text-rose-300 text-sm">
              {error}
            </div>
          ) : layout === 'vertical' ? (
            <div
              ref={verticalContainerRef}
              className="w-full h-full overflow-y-auto py-4 px-2 space-y-2 flex flex-col items-center"
            >
              {Array.from({ length: pageCount }, (_, idx) => (
                <VerticalPageSlot
                  key={`${idx}-${autoCrop}`}
                  pageIndex={idx}
                  pageUrl={getPageUrl(idx)}
                  isMounted={idx >= currentPage - 2 && idx <= currentPage + 3}
                  mangaEnhancer={mangaEnhancer}
                  nightMode={nightMode}
                  fit={fit}
                  zoom={zoom}
                  onVisible={setCurrentPage}
                />
              ))}
            </div>
          ) : (
            <>
              {zoom <= 1 && (
                <>
                  <button
                    type="button"
                    aria-label={direction === 'rtl' ? t('reader.nextPage') : t('reader.prevPage')}
                    onClick={direction === 'rtl' ? goNext : goPrev}
                    className="absolute left-0 top-0 bottom-0 w-1/5 z-10 opacity-0 hover:opacity-100 flex items-center justify-start pl-3 transition-opacity"
                  >
                    <span className="p-2 rounded-full bg-black/50 text-white backdrop-blur-sm">
                      <ChevronLeft className="w-5 h-5" />
                    </span>
                  </button>
                  <button
                    type="button"
                    aria-label={direction === 'rtl' ? t('reader.prevPage') : t('reader.nextPage')}
                    onClick={direction === 'rtl' ? goPrev : goNext}
                    className="absolute right-0 top-0 bottom-0 w-1/5 z-10 opacity-0 hover:opacity-100 flex items-center justify-end pr-3 transition-opacity"
                  >
                    <span className="p-2 rounded-full bg-black/50 text-white backdrop-blur-sm">
                      <ChevronRight className="w-5 h-5" />
                    </span>
                  </button>
                </>
              )}

              <div
                style={{
                  transform: `translate3d(${pan.x}px, ${pan.y}px, 0) scale(${zoom})`,
                  transition: dragRef.current.active ? 'none' : 'transform 120ms ease-out',
                }}
                className={`w-full h-full flex items-center justify-center gap-1.5 p-2 ${
                  fit === 'fitWidth' || fit === 'original' ? 'overflow-auto' : 'overflow-hidden'
                }`}
              >
                {spreadPages.map((idx) => (
                  <MangaPage
                    key={`${idx}-${autoCrop}`}
                    src={getPageUrl(idx)}
                    alt={`Page ${idx + 1}`}
                    enhanced={mangaEnhancer}
                    onLoad={(e) => markWidePageIfSpread(idx, e.currentTarget)}
                    style={{
                      filter: nightMode ? NIGHT_MODE_FILTER : 'none',
                      maxWidth: layout === 'dual' && spreadPages.length === 2 ? '49%' : undefined,
                    }}
                    className={`${getFitClass()} select-none shadow-2xl`}
                  />
                ))}
              </div>
            </>
          )}
        </div>

        {/* Loupe Magnifier Lens Overlay (220px diameter, hairline #22D3EE border) */}
        <div
          ref={loupeOverlayRef}
          aria-hidden="true"
          style={{
            position: 'fixed',
            left: 0,
            top: 0,
            width: `${LOUPE_SIZE}px`,
            height: `${LOUPE_SIZE}px`,
            borderRadius: '9999px',
            border: '1.5px solid #22D3EE',
            boxShadow: '0 0 0 1px rgba(34, 211, 238, 0.35), 0 14px 36px rgba(0, 0, 0, 0.72)',
            overflow: 'hidden',
            pointerEvents: 'none',
            zIndex: 60,
            opacity: 0,
            visibility: 'hidden',
            willChange: 'transform',
            backgroundColor: bgColor,
          }}
        >
          <canvas
            ref={loupeCanvasRef}
            style={{
              width: '100%',
              height: '100%',
              display: 'block',
              filter: nightMode ? NIGHT_MODE_FILTER : 'none',
            }}
          />
          <span
            ref={loupeBadgeRef}
            className="absolute bottom-2 left-1/2 -translate-x-1/2 px-1.5 py-0.5 rounded-full bg-slate-950/80 border border-[#22D3EE]/50 text-[10px] font-mono font-semibold text-[#22D3EE] tabular-nums leading-none"
          >
            {LOUPE_DEFAULT_ZOOM.toFixed(1)}x
          </span>
        </div>

        <BookmarksDrawer
          open={bookmarksOpen}
          comicId={comic.id}
          currentPage={currentPage}
          bookmarks={bookmarks}
          bookmarkName={bookmarkName}
          bookmarkInputRef={bookmarkInputRef}
          onBookmarkNameChange={setBookmarkName}
          onAddBookmark={handleAddBookmark}
          onDeleteBookmark={handleDeleteBookmark}
          onJumpToPage={(idx) => goToPage(idx)}
          onClose={() => setBookmarksOpen(false)}
        />

        {isAtLastPage && adjacent?.next && onOpenComic && (
          <div className="pointer-events-none absolute inset-x-0 bottom-4 z-20 flex justify-center px-4">
            <button
              type="button"
              onClick={handleOpenNextVolume}
              className="pointer-events-auto inline-flex items-center gap-2.5 px-4 py-2.5 rounded-full bg-slate-900/90 hover:bg-brand-600 text-slate-100 hover:text-white border border-slate-700/80 hover:border-brand-400 shadow-2xl backdrop-blur-md text-xs font-semibold transition-all"
              title={adjacent.next.title}
            >
              <span>
                {t('reader.endOfVolumePrompt', {
                  defaultValue: 'End of Volume — Press Enter or Click for Next Volume',
                })}
              </span>
              <ArrowRight className="w-3.5 h-3.5 shrink-0" />
            </button>
          </div>
        )}
      </div>

      {/* Bottom Scrubber Bar (Direction-aware for RTL Manga vs LTR Western) */}
      {controlsVisible && (
      <footer className="h-11 px-4 bg-slate-950/90 backdrop-blur-md border-t border-slate-800/90 flex items-center gap-3 shrink-0">
        <button
          type="button"
          onClick={direction === 'rtl' ? goNext : goPrev}
          disabled={direction === 'rtl' ? atLastSpread : atFirstSpread}
          className="p-1 text-slate-300 hover:text-white disabled:opacity-30"
        >
          <ChevronLeft className="w-4 h-4" />
        </button>

        <input
          type="range"
          dir={direction === 'rtl' ? 'rtl' : 'ltr'}
          min={0}
          max={Math.max(0, pageCount - 1)}
          value={currentPage}
          onChange={(e) => goToPage(Number(e.target.value))}
          className="flex-1 accent-brand-500 cursor-pointer"
        />

        <button
          type="button"
          onClick={direction === 'rtl' ? goPrev : goNext}
          disabled={direction === 'rtl' ? atFirstSpread : atLastSpread}
          className="p-1 text-slate-300 hover:text-white disabled:opacity-30"
        >
          <ChevronRight className="w-4 h-4" />
        </button>

        <span className="text-xs font-mono font-medium text-slate-300 tabular-nums shrink-0 min-w-[96px] text-right">
          {pageCount > 0
            ? `${String(currentPage + 1).padStart(3, '0')} / ${String(pageCount).padStart(3, '0')}`
            : '—'}
        </span>
      </footer>
      )}
    </div>
  );
}
