import React from 'react';
import { useTranslation } from 'react-i18next';
import { X, Cpu, Database, HardDrive } from 'lucide-react';
import { formatBytes } from '../utils/format';

export default function AboutModal({ open, onClose, health }) {
  const { t } = useTranslation();
  if (!open) return null;

  return (
    <div
      onClick={onClose}
      className="fixed inset-0 z-50 flex items-center justify-center bg-black/65 backdrop-blur-sm p-4"
    >
      <div
        onClick={(e) => e.stopPropagation()}
        className="w-full max-w-md rounded-xl bg-white dark:bg-[#12151B] border border-[#DCE1E8] dark:border-[#222834] shadow-2xl overflow-hidden"
      >
        <div className="px-5 py-4 border-b border-[#DCE1E8] dark:border-[#222834] flex items-center justify-between">
          <div className="flex items-center gap-3">
            <img src="/logo.svg" alt="Incuna Reader" className="w-8 h-8 shrink-0" />
            <div>
              <h2 className="text-sm font-bold tracking-tight text-slate-900 dark:text-[#F1F5F9]">
                {t('app.title')}
              </h2>
              <div className="text-[11px] font-mono tabular-nums text-cyan-700 dark:text-[#22D3EE]">
                v{health?.version || '0.1.0'} · PORTABLE WIN32
              </div>
            </div>
          </div>
          <button
            type="button"
            onClick={onClose}
            className="p-1.5 rounded-lg text-slate-400 hover:text-slate-700 dark:hover:text-[#F1F5F9] hover:bg-slate-100 dark:hover:bg-[#171C24]"
          >
            <X className="w-4 h-4" />
          </button>
        </div>

        <div className="p-5 space-y-4 text-xs">
          <p className="text-slate-600 dark:text-slate-400 leading-relaxed">
            {t('about.description')}
          </p>

          <div className="grid grid-cols-3 gap-2">
            <div className="p-2.5 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834]">
              <div className="flex items-center gap-1.5 text-[10px] uppercase tracking-wider text-slate-500">
                <Database className="w-3 h-3 text-cyan-600 dark:text-[#22D3EE]" />
                <span>{t('about.indexed')}</span>
              </div>
              <div className="mt-1 font-mono tabular-nums font-semibold text-sm text-slate-900 dark:text-[#F1F5F9]">
                {(health?.comics_count ?? 0).toLocaleString()}
              </div>
            </div>

            <div className="p-2.5 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834]">
              <div className="flex items-center gap-1.5 text-[10px] uppercase tracking-wider text-slate-500">
                <Cpu className="w-3 h-3 text-cyan-600 dark:text-[#22D3EE]" />
                <span>{t('about.ramCache')}</span>
              </div>
              <div className="mt-1 font-mono tabular-nums font-semibold text-sm text-slate-900 dark:text-[#F1F5F9]">
                {formatBytes(health?.cover_ram_bytes ?? 0)}
              </div>
            </div>

            <div className="p-2.5 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834]">
              <div className="flex items-center gap-1.5 text-[10px] uppercase tracking-wider text-slate-500">
                <HardDrive className="w-3 h-3 text-amber-500 dark:text-[#F59E0B]" />
                <span>{t('about.fts5')}</span>
              </div>
              <div className="mt-1 font-mono tabular-nums font-semibold text-sm text-emerald-600 dark:text-emerald-400">
                {health?.fts5 ? 'ACTIVE' : 'OFF'}
              </div>
            </div>
          </div>

          {health?.portable_root && (
            <div className="p-2.5 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834]">
              <div className="text-[10px] uppercase tracking-wider text-slate-500 mb-1">
                {t('about.portableRoot')}
              </div>
              <div className="font-mono text-[11px] text-slate-700 dark:text-slate-300 break-all">
                {health.portable_root}
              </div>
            </div>
          )}
        </div>

        <div className="px-5 py-3 bg-[#F4F6F9] dark:bg-[#0B0D10] border-t border-[#DCE1E8] dark:border-[#222834] flex justify-end">
          <button
            type="button"
            onClick={onClose}
            className="px-4 py-1.5 rounded-lg bg-cyan-600 dark:bg-[#22D3EE] text-white dark:text-[#0B0D10] text-xs font-semibold hover:opacity-90 transition-opacity"
          >
            {t('settings.close')}
          </button>
        </div>
      </div>
    </div>
  );
}
