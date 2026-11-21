import React from 'react';
import { useTranslation } from 'react-i18next';
import {
  CheckSquare,
  Bookmark,
  CheckCircle2,
  Circle,
  Star,
  CloudDownload,
  X,
} from 'lucide-react';

export default function BatchActionToolbar({
  selectedCount,
  batchBusy,
  shelves,
  shelfMenuOpen,
  onToggleShelfMenu,
  onSelectLoaded,
  onRunBatchAction,
  onClearSelection,
}) {
  const { t } = useTranslation();

  if (selectedCount === 0) return null;

  return (
    <div className="absolute bottom-5 left-1/2 -translate-x-1/2 z-40 flex items-center gap-2 px-3.5 py-2 rounded-xl bg-white/95 dark:bg-[#12151B]/95 backdrop-blur-md border border-cyan-600/40 dark:border-[#22D3EE]/40 shadow-2xl text-xs">
      <span className="font-mono font-semibold text-cyan-700 dark:text-[#22D3EE] px-2 py-1 rounded-md bg-cyan-500/10 dark:bg-[#22D3EE]/10 tabular-nums">
        {t('library.selectedCount', { count: selectedCount })}
      </span>

      <button
        type="button"
        disabled={batchBusy}
        onClick={onSelectLoaded}
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
          onClick={onToggleShelfMenu}
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
                    onRunBatchAction({ action: 'assign_shelf', shelf_id: shelf.id })
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
        onClick={() => onRunBatchAction({ action: 'set_status', status: 'read' })}
        className="inline-flex items-center gap-1.5 px-2.5 py-1.5 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] hover:border-emerald-500 text-slate-700 dark:text-[#F1F5F9] font-medium transition-colors"
      >
        <CheckCircle2 className="w-3.5 h-3.5 text-emerald-500" />
        <span>{t('library.markRead')}</span>
      </button>

      <button
        type="button"
        disabled={batchBusy}
        onClick={() => onRunBatchAction({ action: 'set_status', status: 'unread' })}
        className="inline-flex items-center gap-1.5 px-2.5 py-1.5 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] hover:border-slate-400 text-slate-700 dark:text-[#F1F5F9] font-medium transition-colors"
      >
        <Circle className="w-3.5 h-3.5 text-slate-400" />
        <span>{t('library.markUnread')}</span>
      </button>

      <button
        type="button"
        disabled={batchBusy}
        onClick={() => onRunBatchAction({ action: 'set_favorite', is_favorite: true })}
        className="inline-flex items-center gap-1.5 px-2.5 py-1.5 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] hover:border-amber-500 text-slate-700 dark:text-[#F1F5F9] font-medium transition-colors"
      >
        <Star className="w-3.5 h-3.5 text-amber-500" />
        <span>{t('library.favorite')}</span>
      </button>

      <button
        type="button"
        disabled={batchBusy}
        onClick={() => onRunBatchAction({ action: 'pin_offline', pinned: true })}
        className="inline-flex items-center gap-1.5 px-2.5 py-1.5 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] hover:border-sky-500 text-slate-700 dark:text-[#F1F5F9] font-medium transition-colors"
      >
        <CloudDownload className="w-3.5 h-3.5 text-sky-500" />
        <span>{t('library.pinOffline')}</span>
      </button>

      <button
        type="button"
        title={t('library.clearSelection')}
        onClick={onClearSelection}
        className="p-1.5 rounded-lg text-slate-400 hover:text-slate-700 dark:hover:text-[#F1F5F9] hover:bg-[#F4F6F9] dark:hover:bg-[#171C24] transition-colors"
      >
        <X className="w-4 h-4" />
      </button>
    </div>
  );
}
