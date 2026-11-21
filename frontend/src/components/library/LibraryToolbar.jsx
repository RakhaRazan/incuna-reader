import React from 'react';
import { useTranslation } from 'react-i18next';
import { LayoutGrid, FolderTree, ChevronRight, User, Tag, X } from 'lucide-react';

export default function LibraryToolbar({
  viewMode,
  folderPrefix,
  folderTrail,
  authorFilter,
  tagFilter,
  onSetViewMode,
  onResetToFolderRoot,
  onSelectBreadcrumb,
  onClearAuthor,
  onClearTag,
}) {
  const { t } = useTranslation();

  return (
    <div className="shrink-0 px-6 py-2.5 border-b border-[#DCE1E8] dark:border-[#222834] bg-white dark:bg-[#12151B] flex items-center justify-between gap-3 flex-wrap">
      {/* View Toggle */}
      <div className="inline-flex items-center rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] p-0.5 text-xs font-medium">
        <button
          type="button"
          onClick={() => onSetViewMode('grid')}
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
          onClick={onResetToFolderRoot}
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
            onClick={onResetToFolderRoot}
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
                  onClick={() => onSelectBreadcrumb(seg.path, idx)}
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
  );
}
