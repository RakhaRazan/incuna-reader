import React from 'react';
import { useTranslation } from 'react-i18next';
import { Bookmark, Plus, Trash2, X } from 'lucide-react';

export default function BookmarksDrawer({
  open,
  comicId,
  currentPage,
  bookmarks,
  bookmarkName,
  bookmarkInputRef,
  onBookmarkNameChange,
  onAddBookmark,
  onDeleteBookmark,
  onJumpToPage,
  onClose,
}) {
  const { t } = useTranslation();

  if (!open) return null;

  return (
    <aside className="w-80 shrink-0 h-full bg-slate-950 border-l border-slate-800 flex flex-col z-20">
      <div className="px-4 py-3 border-b border-slate-800 flex items-center justify-between">
        <div className="flex items-center gap-2">
          <Bookmark className="w-4 h-4 text-amber-400" />
          <h3 className="text-xs font-bold text-white uppercase tracking-wider">
            {t('reader.bookmarks')}
          </h3>
        </div>
        <button
          type="button"
          onClick={onClose}
          className="p-1 text-slate-400 hover:text-white"
        >
          <X className="w-4 h-4" />
        </button>
      </div>

      {!comicId && (
        <div className="mx-3 mt-3 p-2.5 rounded-lg bg-amber-500/10 border border-amber-500/30 text-[11px] text-amber-300 leading-relaxed">
          {t('reader.standaloneNote')}
        </div>
      )}

      <form onSubmit={onAddBookmark} className="p-3 border-b border-slate-800 space-y-2">
        <input
          ref={bookmarkInputRef}
          type="text"
          value={bookmarkName}
          onChange={(e) => onBookmarkNameChange(e.target.value)}
          placeholder={t('reader.bookmarkPlaceholder')}
          className="w-full px-3 py-1.5 text-xs rounded-lg bg-slate-900 border border-slate-700 focus:border-brand-500 focus:outline-none text-white"
        />
        <button
          type="submit"
          className="w-full inline-flex items-center justify-center gap-1.5 px-3 py-1.5 rounded-lg bg-brand-600 hover:bg-brand-500 text-white text-xs font-semibold"
        >
          <Plus className="w-3.5 h-3.5" />
          <span>
            {t('reader.addBookmark')} ({t('reader.bookmarkPage', { page: currentPage + 1 })})
          </span>
        </button>
      </form>

      <div className="flex-1 overflow-y-auto p-3 space-y-1.5">
        {bookmarks.length === 0 ? (
          <p className="text-xs text-slate-500 text-center py-8 px-3">
            {t('reader.noBookmarks')}
          </p>
        ) : (
          bookmarks.map((bm) => (
            <div
              key={bm.id}
              className="group flex items-center justify-between gap-2 px-3 py-2 rounded-lg bg-slate-900/90 hover:bg-slate-800 border border-slate-800/80 transition-colors"
            >
              <button
                type="button"
                onClick={() => onJumpToPage((bm.page_number || 1) - 1)}
                className="flex-1 text-left min-w-0"
              >
                <div className="text-xs font-medium text-slate-100 truncate">
                  {bm.name || t('reader.bookmarkPage', { page: bm.page_number })}
                </div>
                <div className="text-[11px] text-amber-400/90 mt-0.5">
                  {t('reader.bookmarkPage', { page: bm.page_number })}
                </div>
              </button>
              <button
                type="button"
                onClick={() => onDeleteBookmark(bm.id)}
                className="opacity-0 group-hover:opacity-100 p-1 text-slate-500 hover:text-rose-400 transition-opacity"
              >
                <Trash2 className="w-3.5 h-3.5" />
              </button>
            </div>
          ))
        )}
      </div>
    </aside>
  );
}
