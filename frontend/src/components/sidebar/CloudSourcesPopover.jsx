import React from 'react';
import { useTranslation } from 'react-i18next';
import { RefreshCw, Check, X } from 'lucide-react';

export default function CloudSourcesPopover({
  open,
  loading,
  sources,
  importingPath,
  onClose,
  onImportSource,
}) {
  const { t } = useTranslation();

  if (!open) return null;

  return (
    <div className="mb-2.5 p-2 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] shadow-lg space-y-1.5">
      <div className="flex items-center justify-between px-1">
        <span className="text-[10px] font-semibold uppercase tracking-wider text-slate-500 dark:text-slate-400">
          {t('sidebar.detectedCloud', { defaultValue: 'Detected Cloud Sync' })}
        </span>
        <button
          type="button"
          onClick={onClose}
          className="p-0.5 text-slate-400 hover:text-slate-600 dark:hover:text-slate-200"
        >
          <X className="w-3 h-3" />
        </button>
      </div>
      {loading ? (
        <div className="px-1 py-2 text-[11px] text-slate-400 flex items-center gap-1.5">
          <RefreshCw className="w-3 h-3 animate-spin" />
          <span>{t('sidebar.scanningCloud', { defaultValue: 'Scanning cloud folders...' })}</span>
        </div>
      ) : sources.length === 0 ? (
        <p className="px-1 py-1.5 text-[11px] text-slate-400 dark:text-slate-500">
          {t('sidebar.noCloudFolders', { defaultValue: 'No OneDrive, Google Drive, or Dropbox folders found.' })}
        </p>
      ) : (
        <div className="space-y-1 max-h-44 overflow-y-auto">
          {sources.map((src) => (
            <div
              key={src.path}
              className="flex items-center justify-between gap-2 px-1.5 py-1.5 rounded bg-white dark:bg-[#12151B] border border-[#DCE1E8] dark:border-[#222834]"
            >
              <div className="min-w-0">
                <div className="text-[11px] font-semibold text-slate-700 dark:text-slate-200 truncate">
                  {src.name}
                </div>
                <div title={src.path} className="text-[10px] font-mono text-slate-400 truncate">
                  {src.path}
                </div>
              </div>
              {src.already_added ? (
                <span className="inline-flex items-center gap-0.5 px-1.5 py-0.5 rounded text-[10px] font-semibold bg-emerald-500/15 text-emerald-600 dark:text-emerald-400 shrink-0">
                  <Check className="w-2.5 h-2.5" />
                  <span>{t('sidebar.added', { defaultValue: 'Added' })}</span>
                </span>
              ) : (
                <button
                  type="button"
                  disabled={Boolean(importingPath)}
                  onClick={() => onImportSource(src)}
                  className="px-2 py-1 rounded bg-cyan-600 dark:bg-[#22D3EE] text-white dark:text-[#0B0D10] text-[10px] font-semibold hover:opacity-90 disabled:opacity-50 shrink-0"
                >
                  {importingPath === src.path
                    ? '...'
                    : t('sidebar.add', { defaultValue: 'Add' })}
                </button>
              )}
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
