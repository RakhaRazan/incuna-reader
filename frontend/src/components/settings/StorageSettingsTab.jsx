import React from 'react';
import { useTranslation } from 'react-i18next';
import {
  Cpu,
  Database,
  Trash2,
  Play,
  RotateCw,
  CheckCircle2,
  CloudDownload,
  FileCheck2,
} from 'lucide-react';
import { formatBytes } from '../../utils/format';

export default function StorageSettingsTab({
  coverMode,
  health,
  packStatus,
  offlineStats,
  includePdfAssoc,
  busy,
  onSelectMode,
  onBuildPack,
  onRebuildCrispCovers,
  onPurgePack,
  onClearUnpinnedCache,
  onIncludePdfChange,
  onRegisterFileTypes,
}) {
  const { t } = useTranslation();

  const pct =
    packStatus?.total > 0
      ? Math.min(100, Math.round((packStatus.processed / packStatus.total) * 100))
      : 0;

  return (
    <>
      <div className="flex items-center justify-between">
        <div>
          <h3 className="text-sm font-semibold text-slate-900 dark:text-[#F1F5F9]">
            {t('settings.coverSystemTitle')}
          </h3>
          <p className="text-xs text-slate-500 dark:text-slate-400 mt-0.5">
            {t('settings.coverSystemDesc')}
          </p>
        </div>
        {health?.cover_ram_bytes != null && (
          <span className="text-[11px] font-mono tabular-nums font-medium px-2.5 py-1 rounded bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] text-slate-700 dark:text-slate-300 shrink-0">
            {t('settings.ramUsage', { size: formatBytes(health.cover_ram_bytes) })}
          </span>
        )}
      </div>

      {/* Mode 1: Zero-Disk RAM Cache */}
      <button
        type="button"
        onClick={() => onSelectMode('ram_only')}
        disabled={busy}
        className={`w-full text-left p-4 rounded-lg border transition-all ${
          coverMode === 'ram_only'
            ? 'border-cyan-600 dark:border-[#22D3EE] bg-cyan-500/5 dark:bg-[#22D3EE]/10'
            : 'border-[#DCE1E8] dark:border-[#222834] hover:border-slate-300 dark:hover:border-slate-700'
        }`}
      >
        <div className="flex items-center justify-between gap-2">
          <div className="flex items-center gap-2.5">
            <Cpu className="w-5 h-5 text-brand-500 shrink-0" />
            <span className="text-sm font-semibold text-slate-900 dark:text-white">
              {t('settings.mode1Title')}
            </span>
          </div>
          <div className="flex items-center gap-2 shrink-0">
            <span className="px-2 py-0.5 rounded-full text-[11px] font-semibold bg-emerald-500/15 text-emerald-600 dark:text-emerald-400">
              {t('settings.mode1Badge')}
            </span>
            {coverMode === 'ram_only' && (
              <CheckCircle2 className="w-4 h-4 text-brand-500" />
            )}
          </div>
        </div>
        <p className="mt-2 text-xs text-slate-600 dark:text-slate-400 leading-relaxed">
          {t('settings.mode1Desc')}
        </p>
      </button>

      {/* Mode 2: Persistent covers.pack */}
      <div
        onClick={() => onSelectMode('persistent_pack')}
        className={`w-full text-left p-4 rounded-xl border-2 transition-all cursor-pointer ${
          coverMode === 'persistent_pack'
            ? 'border-brand-500 bg-brand-50/50 dark:bg-brand-500/10'
            : 'border-slate-200 dark:border-slate-800 hover:border-slate-300 dark:hover:border-slate-700'
        }`}
      >
        <div className="flex items-center justify-between gap-2">
          <div className="flex items-center gap-2.5">
            <Database className="w-5 h-5 text-brand-500 shrink-0" />
            <span className="text-sm font-semibold text-slate-900 dark:text-white">
              {t('settings.mode2Title')}
            </span>
          </div>
          <div className="flex items-center gap-2 shrink-0">
            <span className="px-2 py-0.5 rounded-full text-[11px] font-semibold bg-brand-500/15 text-brand-600 dark:text-brand-400">
              {t('settings.mode2Badge')}
            </span>
            {coverMode === 'persistent_pack' && (
              <CheckCircle2 className="w-4 h-4 text-brand-500" />
            )}
          </div>
        </div>
        <p className="mt-2 text-xs text-slate-600 dark:text-slate-400 leading-relaxed">
          {t('settings.mode2Desc')}
        </p>

        {/* Pack Build Progress & Actions */}
        {(coverMode === 'persistent_pack' ||
          packStatus?.building ||
          (packStatus?.pack_size_bytes ?? 0) > 0) && (
          <div
            onClick={(e) => e.stopPropagation()}
            className="mt-4 pt-3 border-t border-slate-200/80 dark:border-slate-700/70 space-y-3"
          >
            {packStatus?.building && (
              <div className="space-y-1.5">
                <div className="flex items-center justify-between text-xs font-medium text-brand-600 dark:text-brand-400">
                  <span>{t('settings.buildingPack')}</span>
                  <span className="tabular-nums">{pct}%</span>
                </div>
                <div className="w-full h-2 rounded-full bg-slate-200 dark:bg-slate-800 overflow-hidden">
                  <div
                    className="h-full bg-brand-500 transition-all duration-200"
                    style={{ width: `${pct}%` }}
                  />
                </div>
                <div className="text-[11px] text-slate-500 dark:text-slate-400 tabular-nums">
                  {t('settings.packProgress', {
                    processed: packStatus.processed ?? 0,
                    total: packStatus.total ?? 0,
                    succeeded: packStatus.succeeded ?? 0,
                    failed: packStatus.failed ?? 0,
                  })}
                </div>
              </div>
            )}

            <div className="flex flex-wrap items-center justify-between gap-2">
              <span className="text-xs font-medium text-slate-600 dark:text-slate-300">
                {t('settings.packSize', {
                  size: formatBytes(packStatus?.pack_size_bytes ?? 0),
                })}
              </span>

              <div className="flex flex-wrap items-center gap-2">
                <button
                  type="button"
                  disabled={busy || packStatus?.building}
                  onClick={onBuildPack}
                  className="inline-flex items-center gap-1.5 px-3 py-1.5 rounded-lg bg-brand-600 hover:bg-brand-500 disabled:opacity-50 text-white text-xs font-semibold transition-colors"
                >
                  <Play className="w-3.5 h-3.5" />
                  <span>
                    {(packStatus?.pack_size_bytes ?? 0) > 0
                      ? t('settings.rebuildPack')
                      : t('settings.buildPack')}
                  </span>
                </button>

                <button
                  type="button"
                  disabled={busy || packStatus?.building}
                  onClick={onRebuildCrispCovers}
                  className="inline-flex items-center gap-1.5 px-3 py-1.5 rounded-lg bg-cyan-600 hover:bg-cyan-500 disabled:opacity-50 text-white text-xs font-semibold transition-colors"
                >
                  <RotateCw className="w-3.5 h-3.5" />
                  <span>
                    {t('settings.rebuildCrispCovers', { defaultValue: 'Rebuild Crisp Covers' })}
                  </span>
                </button>

                {(packStatus?.pack_size_bytes ?? 0) > 0 && (
                  <button
                    type="button"
                    disabled={busy || packStatus?.building}
                    onClick={onPurgePack}
                    className="inline-flex items-center gap-1.5 px-3 py-1.5 rounded-lg bg-rose-500/15 hover:bg-rose-500/25 text-rose-600 dark:text-rose-400 text-xs font-semibold transition-colors"
                  >
                    <Trash2 className="w-3.5 h-3.5" />
                    <span>{t('settings.purgePack')}</span>
                  </button>
                )}
              </div>
            </div>
          </div>
        )}
      </div>

      {/* Offline Cache Disk Usage */}
      <div className="p-4 rounded-xl border border-[#DCE1E8] dark:border-[#222834] bg-[#F4F6F9]/60 dark:bg-[#0B0D10]/60">
        <div className="flex flex-wrap items-center justify-between gap-3">
          <div className="flex items-start gap-2.5">
            <CloudDownload className="w-5 h-5 text-cyan-600 dark:text-[#22D3EE] shrink-0 mt-0.5" />
            <div>
              <h4 className="text-sm font-semibold text-slate-900 dark:text-white">
                {t('settings.offlineCacheTitle', { defaultValue: 'Offline Cache' })}
              </h4>
              <p className="text-xs text-slate-500 dark:text-slate-400 mt-0.5 tabular-nums">
                {t('settings.offlineCacheStats', {
                  size: formatBytes(offlineStats?.total_bytes ?? 0),
                  pinned: offlineStats?.pinned_count ?? 0,
                  files: offlineStats?.file_count ?? 0,
                  defaultValue: `${formatBytes(offlineStats?.total_bytes ?? 0)} used · ${offlineStats?.pinned_count ?? 0} pinned · ${offlineStats?.file_count ?? 0} file(s)`,
                })}
              </p>
            </div>
          </div>
          <button
            type="button"
            disabled={busy || (offlineStats?.file_count ?? 0) === 0}
            onClick={onClearUnpinnedCache}
            className="inline-flex items-center gap-1.5 px-3 py-1.5 rounded-lg bg-slate-200/80 dark:bg-[#171C24] hover:bg-rose-500/15 hover:text-rose-600 dark:hover:text-rose-400 disabled:opacity-50 text-slate-700 dark:text-slate-300 text-xs font-semibold transition-colors shrink-0"
          >
            <Trash2 className="w-3.5 h-3.5" />
            <span>{t('settings.clearUnpinnedCache', { defaultValue: 'Clear Unpinned Cache' })}</span>
          </button>
        </div>
      </div>

      {/* Windows Explorer File Association */}
      <div className="p-4 rounded-xl border border-[#DCE1E8] dark:border-[#222834] bg-[#F4F6F9]/60 dark:bg-[#0B0D10]/60">
        <div className="flex flex-wrap items-center justify-between gap-3">
          <div className="flex items-start gap-2.5">
            <FileCheck2 className="w-5 h-5 text-brand-500 shrink-0 mt-0.5" />
            <div>
              <h4 className="text-sm font-semibold text-slate-900 dark:text-white">
                {t('settings.fileAssocTitle', { defaultValue: 'Windows Explorer Integration' })}
              </h4>
              <p className="text-xs text-slate-500 dark:text-slate-400 mt-0.5">
                {t('settings.fileAssocDesc', {
                  defaultValue: 'Associate .cbz, .cbr, .cb7, .cbt, and .epub with Incuna Reader (per-user, no admin prompt).',
                })}
              </p>
              <label className="mt-2 inline-flex items-center gap-2 text-xs text-slate-600 dark:text-slate-300 cursor-pointer select-none">
                <input
                  type="checkbox"
                  checked={includePdfAssoc}
                  onChange={(e) => onIncludePdfChange(e.target.checked)}
                  className="rounded border-slate-400 text-brand-600 focus:ring-brand-500"
                />
                <span>{t('settings.includePdfAssoc', { defaultValue: 'Also associate .pdf files' })}</span>
              </label>
            </div>
          </div>
          <button
            type="button"
            disabled={busy}
            onClick={onRegisterFileTypes}
            className="inline-flex items-center gap-1.5 px-3 py-1.5 rounded-lg bg-brand-600 hover:bg-brand-500 disabled:opacity-50 text-white text-xs font-semibold transition-colors shrink-0"
          >
            <FileCheck2 className="w-3.5 h-3.5" />
            <span>{t('settings.setDefaultReader', { defaultValue: 'Set as Default Comic Reader' })}</span>
          </button>
        </div>
      </div>
    </>
  );
}
