import React, { memo, useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { BookOpen, CloudDownload, Star, MoreVertical, Check, Folder } from 'lucide-react';
import { formatBytes, formatDisplayTitle } from '../utils/format';

const MAX_CONCURRENT_COVERS = 6;
let activeCoverLoads = 0;
const coverQueue = [];

function drainCoverQueue() {
  while (activeCoverLoads < MAX_CONCURRENT_COVERS && coverQueue.length > 0) {
    const next = coverQueue.shift();
    if (!next.isCancelled()) {
      next.run();
      break;
    }
  }
}

function enqueueCoverLoad(task) {
  let cancelled = false;
  const run = () => {
    if (cancelled) {
      drainCoverQueue();
      return;
    }
    activeCoverLoads += 1;
    Promise.resolve()
      .then(task)
      .finally(() => {
        activeCoverLoads = Math.max(0, activeCoverLoads - 1);
        drainCoverQueue();
      });
  };

  if (activeCoverLoads < MAX_CONCURRENT_COVERS) {
    run();
  } else {
    coverQueue.push({ run, isCancelled: () => cancelled });
  }

  return () => {
    cancelled = true;
  };
}

export function CoverImage({ comicId, coverVersion, title, deferCover, isFolder = false }) {
  const [canMountImg, setCanMountImg] = useState(!deferCover);
  const [loaded, setLoaded] = useState(false);
  const [decodedSrc, setDecodedSrc] = useState('');
  const [retryCount, setRetryCount] = useState(0);
  const [failed, setFailed] = useState(false);
  const retryTimerRef = useRef(null);

  useEffect(() => {
    if (retryTimerRef.current) {
      clearTimeout(retryTimerRef.current);
      retryTimerRef.current = null;
    }
    setCanMountImg(!deferCover);
    setLoaded(false);
    setDecodedSrc('');
    setRetryCount(0);
    setFailed(false);

    return () => {
      if (retryTimerRef.current) {
        clearTimeout(retryTimerRef.current);
        retryTimerRef.current = null;
      }
    };
  }, [comicId]);

  useEffect(() => {
    if (!deferCover) {
      setCanMountImg(true);
    }
  }, [deferCover]);

  useEffect(() => {
    if (!coverVersion) return;
    setFailed((prevFailed) => {
      if (prevFailed) {
        setRetryCount(0);
        return false;
      }
      return prevFailed;
    });
  }, [coverVersion]);

  const baseUrl = `/api/covers/${comicId}`;
  const params = new URLSearchParams();
  if (coverVersion) params.set('v', String(coverVersion));
  if (retryCount > 0) params.set('r', String(retryCount));
  const qs = params.toString();
  const src = qs ? `${baseUrl}?${qs}` : baseUrl;

  useEffect(() => {
    if (!canMountImg || failed) return;

    let disposed = false;
    setLoaded(false);

    const cancelQueued = enqueueCoverLoad(
      () =>
        new Promise((resolve) => {
          if (disposed) {
            resolve();
            return;
          }

          const img = new Image();
          img.decoding = 'async';
          let settled = false;

          const finishSuccess = () => {
            if (settled) return;
            settled = true;
            if (!disposed) {
              setDecodedSrc(src);
              requestAnimationFrame(() => {
                if (!disposed) setLoaded(true);
              });
            }
            resolve();
          };

          const finishError = () => {
            if (settled) return;
            settled = true;
            if (!disposed) {
              if (retryCount < 2) {
                if (retryTimerRef.current) clearTimeout(retryTimerRef.current);
                const delayMs = retryCount === 0 ? 800 : 2000;
                retryTimerRef.current = setTimeout(() => {
                  if (!disposed) setRetryCount((c) => c + 1);
                }, delayMs);
              } else {
                setFailed(true);
              }
            }
            resolve();
          };

          img.onload = () => {
            if (typeof img.decode === 'function') {
              img.decode().then(finishSuccess, finishSuccess);
            } else {
              finishSuccess();
            }
          };
          img.onerror = finishError;
          img.src = src;

          if (typeof img.decode === 'function' && img.complete && img.naturalWidth > 0) {
            img.decode().then(finishSuccess, finishSuccess);
          }
        })
    );

    return () => {
      disposed = true;
      cancelQueued();
    };
  }, [canMountImg, failed, src, retryCount]);

  if (!canMountImg) {
    return <div className="w-full h-full bg-slate-200 dark:bg-[#0B0D10]" />;
  }

  if (failed) {
    if (isFolder) {
      return (
        <div className="w-full h-full flex items-center justify-center bg-slate-200 dark:bg-[#0B0D10]">
          <Folder className="w-12 h-12 text-cyan-600/70 dark:text-[#22D3EE]/70" />
        </div>
      );
    }
    return (
      <div className="w-full h-full flex flex-col items-center justify-center bg-slate-200/70 dark:bg-slate-800/80 text-slate-400 dark:text-slate-500 p-3 text-center">
        <BookOpen className="w-8 h-8 mb-1.5 opacity-60" />
        <span className="text-[11px] line-clamp-2 leading-tight opacity-75">{title}</span>
      </div>
    );
  }

  return (
    <>
      {!loaded && (
        <div className="absolute inset-0 bg-slate-200 dark:bg-[#0B0D10]" />
      )}
      {decodedSrc && (
        <img
          src={decodedSrc}
          alt=""
          decoding="async"
          style={{ imageRendering: 'auto', willChange: 'opacity' }}
          className={`relative z-10 w-full h-full object-cover transition-opacity duration-150 ${
            loaded ? 'opacity-100' : 'opacity-0'
          }`}
        />
      )}
    </>
  );
}

const ComicCard = memo(function ComicCard({
  comic,
  index,
  coverHeight,
  coverVersion,
  deferCover = false,
  isSelected = false,
  selectionMode = false,
  circularProgressBar = true,
  smartRename = false,
  enableMetadataExtractor = true,
  onOpenComic,
  onCardClick,
  onToggleSelect,
  onContextMenu,
  onToggleFavorite,
  onFilterAuthor,
}) {
  const { t } = useTranslation();

  if (!comic) {
    return (
      <div className="w-full h-full rounded-xl bg-white dark:bg-[#151924] border border-slate-200/80 dark:border-slate-800/80 overflow-hidden">
        <div
          style={{ height: `${coverHeight}px` }}
          className="w-full bg-slate-200 dark:bg-slate-800"
        />
        <div className="p-2.5 space-y-2">
          <div className="h-3.5 bg-slate-200 dark:bg-slate-800 rounded w-4/5" />
          <div className="h-2.5 bg-slate-200 dark:bg-slate-800 rounded w-1/2" />
        </div>
      </div>
    );
  }

  const progressPct =
    comic.page_count > 0
      ? Math.min(100, Math.round((comic.current_page / comic.page_count) * 100))
      : comic.status === 'read'
        ? 100
        : 0;

  const statusBadge =
    comic.status === 'read'
      ? { label: t('card.read'), cls: 'bg-emerald-500/90 text-white' }
      : comic.status === 'reading'
        ? { label: t('card.reading'), cls: 'bg-[#22D3EE]/90 text-[#0B0D10]' }
        : { label: t('card.unread'), cls: 'bg-[#0B0D10]/85 text-[#F1F5F9] border border-[#222834]' };

  const padNum = (n) => String(n).padStart(3, '0');
  const displayTitle = formatDisplayTitle(comic.title, smartRename);
  const ringRadius = 9;
  const ringCircumference = 2 * Math.PI * ringRadius;
  const ringOffset = ringCircumference * (1 - progressPct / 100);

  return (
    <div className="relative w-full h-full">
      <div
        tabIndex={0}
        onClick={(e) => {
          if (onCardClick) onCardClick(e, comic, index);
          else onOpenComic?.(comic);
        }}
        onContextMenu={(e) => {
          e.preventDefault();
          onContextMenu?.(e, comic);
        }}
        className={`tactical-card group relative w-full h-full rounded-lg bg-white dark:bg-[#12151B] border cursor-pointer overflow-hidden flex flex-col focus-visible:outline-none ${
          comic.is_hidden ? 'opacity-65' : ''
        } ${
          isSelected
            ? 'border-cyan-600 dark:border-[#22D3EE] ring-2 ring-cyan-500/60 dark:ring-[#22D3EE]/70'
            : 'border-[#DCE1E8] dark:border-[#222834] hover:border-cyan-600/80 dark:hover:border-[#22D3EE]/80 focus-visible:border-[#22D3EE]'
        }`}
      >
      {/* Tactical Corner Reticles */}
      <div className="tactical-reticle z-20">
        <span className="absolute top-0 left-0 w-2.5 h-2.5 border-t-2 border-l-2 border-cyan-600 dark:border-[#22D3EE]" />
        <span className="absolute top-0 right-0 w-2.5 h-2.5 border-t-2 border-r-2 border-cyan-600 dark:border-[#22D3EE]" />
        <span className="absolute bottom-0 left-0 w-2.5 h-2.5 border-b-2 border-l-2 border-cyan-600 dark:border-[#22D3EE]" />
        <span className="absolute bottom-0 right-0 w-2.5 h-2.5 border-b-2 border-r-2 border-cyan-600 dark:border-[#22D3EE]" />
      </div>

      {/* Cover Art */}
      <div
        style={{ height: `${coverHeight}px` }}
        className="relative w-full bg-slate-200 dark:bg-[#0B0D10] overflow-hidden shrink-0"
      >
        <CoverImage
          comicId={comic.id}
          coverVersion={coverVersion}
          title={comic.title}
          deferCover={deferCover}
        />

        {/* Top-left Multi-Select Checkbox */}
        <button
          type="button"
          aria-label={t('library.selectComic', { defaultValue: 'Select comic' })}
          onClick={(e) => {
            e.stopPropagation();
            onToggleSelect?.(e, comic, index);
          }}
          className={`absolute top-2 left-2 z-30 w-5 h-5 rounded flex items-center justify-center border ${
            isSelected
              ? 'bg-cyan-600 dark:bg-[#22D3EE] border-cyan-600 dark:border-[#22D3EE] text-white dark:text-[#0B0D10] opacity-100'
              : selectionMode
                ? 'bg-[#0B0D10]/75 border-white/60 text-transparent opacity-100 hover:border-[#22D3EE]'
                : 'bg-[#0B0D10]/75 border-white/60 text-transparent opacity-0 group-hover:opacity-100 hover:border-[#22D3EE]'
          }`}
        >
          <Check className="w-3.5 h-3.5 stroke-[3]" />
        </button>

        {/* Bottom-left Status Badge */}
        <span
          className={`absolute bottom-2 left-2 z-20 px-1.5 py-0.5 rounded text-[10px] font-mono font-semibold tracking-wider uppercase ${statusBadge.cls}`}
        >
          {statusBadge.label}
        </span>

        {/* Top-right Circular Progress, Offline Pin & Favorite Badges */}
        <div className="absolute top-2 right-2 z-20 flex items-center gap-1">
          {circularProgressBar && progressPct > 0 && (
            <div
              title={`${progressPct}%`}
              className="w-6 h-6 rounded-full bg-[#0B0D10]/80 backdrop-blur-sm flex items-center justify-center shadow"
            >
              <svg className="w-5 h-5 -rotate-90" viewBox="0 0 24 24">
                <circle
                  cx="12"
                  cy="12"
                  r={ringRadius}
                  fill="none"
                  stroke="rgba(255,255,255,0.2)"
                  strokeWidth="2.5"
                />
                <circle
                  cx="12"
                  cy="12"
                  r={ringRadius}
                  fill="none"
                  stroke={progressPct >= 100 ? '#10B981' : '#22D3EE'}
                  strokeWidth="2.5"
                  strokeLinecap="round"
                  strokeDasharray={ringCircumference}
                  strokeDashoffset={ringOffset}
                />
              </svg>
            </div>
          )}
          {comic.is_offline_pinned && (
            <span
              title={t('card.offlinePinned')}
              className="p-1 rounded bg-emerald-600/90 text-white"
            >
              <CloudDownload className="w-3 h-3" />
            </span>
          )}
          <button
            type="button"
            title={t('card.favorite')}
            onClick={(e) => onToggleFavorite?.(e, comic, index)}
            className={`p-1 rounded ${
              comic.is_favorite
                ? 'bg-[#F59E0B] text-[#0B0D10] opacity-100'
                : 'bg-[#0B0D10]/75 text-white/80 opacity-0 group-hover:opacity-100 hover:bg-[#0B0D10]'
            }`}
          >
            <Star
              className={`w-3.5 h-3.5 ${comic.is_favorite ? 'fill-current' : ''}`}
            />
          </button>
        </div>

        {/* Context menu button on hover */}
        <button
          type="button"
          onClick={(e) => {
            e.stopPropagation();
            onContextMenu?.(e, comic);
          }}
          className="absolute bottom-2 right-2 p-1 rounded bg-[#0B0D10]/75 text-white opacity-0 group-hover:opacity-100 hover:bg-[#0B0D10]"
        >
          <MoreVertical className="w-3.5 h-3.5" />
        </button>

        {/* Reading Progress Bar */}
        {progressPct > 0 && (
          <div className="absolute bottom-0 inset-x-0 h-[3px] bg-black/40">
            <div
              className="h-full bg-[#22D3EE]"
              style={{ width: `${progressPct}%` }}
            />
          </div>
        )}
      </div>

      {/* Card Metadata */}
      <div className="px-2.5 py-2 flex-1 flex flex-col justify-between min-w-0">
        <h4
          title={comic.title}
          className="text-xs font-semibold text-slate-900 dark:text-[#F1F5F9] line-clamp-2 leading-snug"
        >
          {displayTitle}
        </h4>
        <div className="flex items-center justify-between gap-1 text-[11px] font-mono tabular-nums text-slate-500 dark:text-slate-400 mt-1">
          {enableMetadataExtractor && comic.author ? (
            <button
              type="button"
              onClick={(e) => {
                e.stopPropagation();
                onFilterAuthor?.(comic.author);
              }}
              title={`Filter by author: ${comic.author}`}
              className="truncate text-[10px] font-sans font-medium px-1.5 py-0.5 rounded bg-cyan-500/10 text-cyan-700 dark:text-[#22D3EE] hover:bg-cyan-500/20 transition-colors"
            >
              {comic.author}
            </button>
          ) : (
            <span className="truncate">
              {comic.page_count > 0
                ? comic.current_page > 0
                  ? `${padNum(comic.current_page)} / ${padNum(comic.page_count)}`
                  : `${padNum(comic.page_count)} P`
                : t('card.noPages')}
            </span>
          )}
          <span className="uppercase font-medium text-[10px] px-1.5 py-0.5 rounded bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] text-slate-600 dark:text-slate-300 shrink-0 ml-1">
            {comic.file_size_bytes > 0
              ? formatBytes(comic.file_size_bytes)
              : comic.format}
          </span>
        </div>
      </div>
    </div>
    </div>
  );
});

export default ComicCard;
