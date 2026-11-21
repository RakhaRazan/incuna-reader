import React, { useEffect, useRef } from 'react';
import { useTranslation } from 'react-i18next';
import {
  BookOpen,
  Star,
  CheckCircle2,
  CloudDownload,
  Bookmark,
  Trash2,
  Eye,
  EyeOff,
  User,
  Tag,
} from 'lucide-react';

export default function ContextMenu({
  menu,
  shelves,
  activeShelfId,
  enableMetadataExtractor = true,
  onClose,
  onOpenComic,
  onToggleFavorite,
  onSetStatus,
  onToggleOfflinePin,
  onToggleHidden,
  onAssignShelf,
  onRemoveFromShelf,
  onFilterAuthor,
  onFilterTag,
}) {
  const { t } = useTranslation();
  const ref = useRef(null);

  useEffect(() => {
    if (!menu) return;
    const handleDown = (e) => {
      if (ref.current && !ref.current.contains(e.target)) {
        onClose();
      }
    };
    const handleEsc = (e) => {
      if (e.key === 'Escape') onClose();
    };
    window.addEventListener('mousedown', handleDown);
    window.addEventListener('keydown', handleEsc);
    return () => {
      window.removeEventListener('mousedown', handleDown);
      window.removeEventListener('keydown', handleEsc);
    };
  }, [menu, onClose]);

  if (!menu) return null;

  const { comic, x, y } = menu;
  const parsedTags = enableMetadataExtractor && comic.tags
    ? comic.tags.split(',').map((s) => s.trim()).filter(Boolean)
    : [];
  const hasMetadata = enableMetadataExtractor && (Boolean(comic.author) || parsedTags.length > 0);
  const clampedX = Math.min(x, window.innerWidth - 240);
  const clampedY = Math.min(y, window.innerHeight - (hasMetadata ? 400 : 320));

  return (
    <div
      ref={ref}
      style={{ top: `${clampedY}px`, left: `${clampedX}px` }}
      className="fixed z-50 w-56 rounded-xl bg-white dark:bg-[#1a1f2e] border border-slate-200 dark:border-slate-700/80 shadow-2xl py-1.5 text-xs text-slate-700 dark:text-slate-200"
    >
      <button
        type="button"
        onClick={() => {
          onOpenComic(comic);
          onClose();
        }}
        className="w-full px-3 py-1.5 flex items-center gap-2 hover:bg-slate-100 dark:hover:bg-slate-800 font-medium"
      >
        <BookOpen className="w-3.5 h-3.5 text-brand-500" />
        <span>{t('contextMenu.open')}</span>
      </button>

      <button
        type="button"
        onClick={() => {
          onToggleFavorite(comic);
          onClose();
        }}
        className="w-full px-3 py-1.5 flex items-center gap-2 hover:bg-slate-100 dark:hover:bg-slate-800"
      >
        <Star className={`w-3.5 h-3.5 text-amber-500 ${comic.is_favorite ? 'fill-current' : ''}`} />
        <span>
          {comic.is_favorite
            ? t('contextMenu.removeFavorite')
            : t('contextMenu.addFavorite')}
        </span>
      </button>

      <button
        type="button"
        onClick={() => {
          onSetStatus(comic, comic.status === 'read' ? 'unread' : 'read');
          onClose();
        }}
        className="w-full px-3 py-1.5 flex items-center gap-2 hover:bg-slate-100 dark:hover:bg-slate-800"
      >
        <CheckCircle2 className="w-3.5 h-3.5 text-emerald-500" />
        <span>
          {comic.status === 'read' ? t('contextMenu.markUnread') : t('contextMenu.markRead')}
        </span>
      </button>

      <button
        type="button"
        onClick={() => {
          onToggleOfflinePin(comic);
          onClose();
        }}
        className="w-full px-3 py-1.5 flex items-center gap-2 hover:bg-slate-100 dark:hover:bg-slate-800"
      >
        <CloudDownload className="w-3.5 h-3.5 text-sky-500" />
        <span>
          {comic.is_offline_pinned
            ? t('contextMenu.unpinOffline')
            : t('contextMenu.pinOffline')}
        </span>
      </button>

      {onToggleHidden && (
        <button
          type="button"
          onClick={() => {
            onToggleHidden(comic);
            onClose();
          }}
          className="w-full px-3 py-1.5 flex items-center gap-2 hover:bg-slate-100 dark:hover:bg-slate-800"
        >
          {comic.is_hidden ? (
            <Eye className="w-3.5 h-3.5 text-slate-400" />
          ) : (
            <EyeOff className="w-3.5 h-3.5 text-slate-400" />
          )}
          <span>{comic.is_hidden ? 'Unhide Comic' : 'Hide Comic'}</span>
        </button>
      )}

      {activeShelfId > 0 && (
        <button
          type="button"
          onClick={() => {
            onRemoveFromShelf(activeShelfId, comic.id);
            onClose();
          }}
          className="w-full px-3 py-1.5 flex items-center gap-2 text-rose-500 hover:bg-rose-500/10"
        >
          <Trash2 className="w-3.5 h-3.5" />
          <span>{t('contextMenu.removeFromShelf')}</span>
        </button>
      )}

      {hasMetadata && (
        <>
          <div className="my-1 border-t border-slate-200 dark:border-slate-700/80" />
          <div className="px-3 py-1.5 space-y-1.5">
            {comic.author && (
              <div className="flex items-center gap-1.5 flex-wrap">
                <User className="w-3 h-3 text-cyan-600 dark:text-[#22D3EE] shrink-0" />
                <button
                  type="button"
                  onClick={() => {
                    onFilterAuthor?.(comic.author);
                    onClose();
                  }}
                  className="px-2 py-0.5 rounded-full text-[11px] font-medium bg-cyan-500/10 text-cyan-700 dark:text-[#22D3EE] border border-cyan-600/30 dark:border-[#22D3EE]/30 hover:bg-cyan-500/20 truncate max-w-[175px]"
                  title={`Filter by author: ${comic.author}`}
                >
                  {comic.author}
                </button>
              </div>
            )}
            {parsedTags.length > 0 && (
              <div className="flex items-start gap-1.5 flex-wrap">
                <Tag className="w-3 h-3 text-slate-400 shrink-0 mt-1" />
                <div className="flex flex-wrap gap-1 flex-1 min-w-0">
                  {parsedTags.slice(0, 6).map((tg) => (
                    <button
                      key={tg}
                      type="button"
                      onClick={() => {
                        onFilterTag?.(tg);
                        onClose();
                      }}
                      className="px-1.5 py-0.5 rounded text-[10px] font-medium bg-slate-100 dark:bg-slate-800 text-slate-700 dark:text-slate-300 hover:bg-cyan-500/15 hover:text-cyan-700 dark:hover:text-[#22D3EE] truncate max-w-[140px]"
                      title={`Filter by tag: ${tg}`}
                    >
                      {tg}
                    </button>
                  ))}
                </div>
              </div>
            )}
          </div>
        </>
      )}

      <div className="my-1 border-t border-slate-200 dark:border-slate-700/80" />

      <div className="px-3 py-1 text-[10px] font-semibold uppercase tracking-wider text-slate-400">
        {t('contextMenu.assignShelf')}
      </div>

      {shelves.length === 0 ? (
        <div className="px-3 py-1 text-[11px] text-slate-400 italic">
          {t('contextMenu.noShelves')}
        </div>
      ) : (
        <div className="max-h-36 overflow-y-auto">
          {shelves.map((shelf) => (
            <button
              key={shelf.id}
              type="button"
              onClick={() => {
                onAssignShelf(shelf.id, comic.id);
                onClose();
              }}
              className="w-full px-3 py-1.5 flex items-center gap-2 hover:bg-slate-100 dark:hover:bg-slate-800 truncate"
            >
              <Bookmark className="w-3.5 h-3.5 text-slate-400 shrink-0" />
              <span className="truncate">{shelf.name}</span>
            </button>
          ))}
        </div>
      )}
    </div>
  );
}
