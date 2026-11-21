import React, { useState } from 'react';
import { useTranslation } from 'react-i18next';
import { Sparkles, X } from 'lucide-react';
import { api } from '../api/client';

export default function SmartViewModal({ open, onClose, onCreated }) {
  const { t } = useTranslation();
  const [name, setName] = useState('');
  const [status, setStatus] = useState('');
  const [format, setFormat] = useState('');
  const [minSizeMb, setMinSizeMb] = useState('');
  const [pathContains, setPathContains] = useState('');
  const [onlyFavorites, setOnlyFavorites] = useState(false);
  const [onlyOffline, setOnlyOffline] = useState(false);
  const [error, setError] = useState('');

  if (!open) return null;

  const handleSubmit = async (e) => {
    e.preventDefault();
    const trimmed = name.trim();
    if (!trimmed) return;

    const query = {};
    if (status) query.status = status;
    if (format) query.format = format;
    const parsedMb = Number(minSizeMb);
    if (Number.isFinite(parsedMb) && parsedMb > 0) {
      query.min_size_mb = Math.floor(parsedMb);
    }
    if (pathContains.trim()) {
      query.path_contains = pathContains.trim();
    }
    if (onlyFavorites) query.is_favorite = true;
    if (onlyOffline) query.is_offline_pinned = true;

    const slug =
      'custom_' +
      trimmed
        .toLowerCase()
        .replace(/[^a-z0-9]+/g, '_')
        .replace(/^_|_$/g, '') +
      '_' +
      Date.now().toString(36);

    setError('');
    try {
      await api.createSmartview({ slug, name: trimmed, query });
      setName('');
      setStatus('');
      setFormat('');
      setMinSizeMb('');
      setPathContains('');
      setOnlyFavorites(false);
      setOnlyOffline(false);
      onCreated?.();
      onClose();
    } catch (err) {
      setError(err.message || 'Failed to create SmartView');
    }
  };

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/55 backdrop-blur-sm p-4">
      <form
        onSubmit={handleSubmit}
        className="w-full max-w-md rounded-2xl bg-white dark:bg-[#12151B] border border-[#DCE1E8] dark:border-[#222834] shadow-2xl overflow-hidden"
      >
        <div className="px-5 py-4 border-b border-[#DCE1E8] dark:border-[#222834] flex items-center justify-between">
          <div className="flex items-center gap-2">
            <Sparkles className="w-4 h-4 text-cyan-600 dark:text-[#22D3EE]" />
            <h2 className="text-sm font-bold text-slate-900 dark:text-[#F1F5F9]">
              {t('smartviewModal.title')}
            </h2>
          </div>
          <button
            type="button"
            onClick={onClose}
            className="p-1 rounded-lg text-slate-400 hover:text-slate-600 dark:hover:text-slate-200"
          >
            <X className="w-4 h-4" />
          </button>
        </div>

        <div className="p-5 space-y-3.5 text-xs">
          <div>
            <label className="block font-semibold text-slate-700 dark:text-slate-300 mb-1">
              {t('smartviewModal.nameLabel')}
            </label>
            <input
              type="text"
              autoFocus
              required
              value={name}
              onChange={(e) => setName(e.target.value)}
              placeholder={t('smartviewModal.namePlaceholder')}
              className="w-full px-3 py-2 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] focus:border-cyan-600 dark:focus:border-[#22D3EE] focus:outline-none text-slate-900 dark:text-[#F1F5F9]"
            />
          </div>

          <div className="grid grid-cols-2 gap-3">
            <div>
              <label className="block font-semibold text-slate-700 dark:text-slate-300 mb-1">
                {t('smartviewModal.statusLabel')}
              </label>
              <select
                value={status}
                onChange={(e) => setStatus(e.target.value)}
                className="w-full px-2.5 py-2 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] focus:border-cyan-600 dark:focus:border-[#22D3EE] focus:outline-none text-slate-900 dark:text-[#F1F5F9]"
              >
                <option value="">{t('smartviewModal.statusAny')}</option>
                <option value="unread">{t('smartviewModal.statusUnread')}</option>
                <option value="reading">{t('smartviewModal.statusReading')}</option>
                <option value="read">{t('smartviewModal.statusRead')}</option>
              </select>
            </div>

            <div>
              <label className="block font-semibold text-slate-700 dark:text-slate-300 mb-1">
                {t('smartviewModal.formatLabel')}
              </label>
              <select
                value={format}
                onChange={(e) => setFormat(e.target.value)}
                className="w-full px-2.5 py-2 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] focus:border-cyan-600 dark:focus:border-[#22D3EE] focus:outline-none text-slate-900 dark:text-[#F1F5F9]"
              >
                <option value="">{t('smartviewModal.formatAny')}</option>
                <option value="pdf">PDF</option>
                <option value="cbz">CBZ / ZIP</option>
                <option value="cbr">CBR / RAR</option>
                <option value="cb7">7Z / CB7</option>
                <option value="epub">EPUB</option>
              </select>
            </div>
          </div>

          <div className="grid grid-cols-2 gap-3">
            <div>
              <label className="block font-semibold text-slate-700 dark:text-slate-300 mb-1">
                {t('smartviewModal.minSizeMbLabel')}
              </label>
              <input
                type="number"
                min={0}
                step={50}
                value={minSizeMb}
                onChange={(e) => setMinSizeMb(e.target.value)}
                placeholder={t('smartviewModal.minSizeMbPlaceholder')}
                className="w-full px-3 py-2 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] focus:border-cyan-600 dark:focus:border-[#22D3EE] focus:outline-none text-slate-900 dark:text-[#F1F5F9]"
              />
            </div>

            <div>
              <label className="block font-semibold text-slate-700 dark:text-slate-300 mb-1">
                {t('smartviewModal.pathContainsLabel')}
              </label>
              <input
                type="text"
                value={pathContains}
                onChange={(e) => setPathContains(e.target.value)}
                placeholder={t('smartviewModal.pathContainsPlaceholder')}
                className="w-full px-3 py-2 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] focus:border-cyan-600 dark:focus:border-[#22D3EE] focus:outline-none text-slate-900 dark:text-[#F1F5F9]"
              />
            </div>
          </div>

          <div className="flex items-center gap-5 pt-1">
            <label className="inline-flex items-center gap-2 cursor-pointer text-slate-700 dark:text-slate-300">
              <input
                type="checkbox"
                checked={onlyFavorites}
                onChange={(e) => setOnlyFavorites(e.target.checked)}
                className="rounded accent-[#22D3EE]"
              />
              <span>{t('smartviewModal.onlyFavorites')}</span>
            </label>
            <label className="inline-flex items-center gap-2 cursor-pointer text-slate-700 dark:text-slate-300">
              <input
                type="checkbox"
                checked={onlyOffline}
                onChange={(e) => setOnlyOffline(e.target.checked)}
                className="rounded accent-[#22D3EE]"
              />
              <span>{t('smartviewModal.onlyOffline')}</span>
            </label>
          </div>

          {error && (
            <div className="text-xs text-rose-500 bg-rose-500/10 border border-rose-500/30 rounded-lg px-3 py-2">
              {error}
            </div>
          )}
        </div>

        <div className="px-5 py-3 bg-[#F4F6F9] dark:bg-[#0B0D10] border-t border-[#DCE1E8] dark:border-[#222834] flex justify-end gap-2">
          <button
            type="button"
            onClick={onClose}
            className="px-3.5 py-1.5 rounded-lg text-xs font-medium text-slate-600 dark:text-slate-300 hover:bg-slate-200 dark:hover:bg-[#171C24]"
          >
            {t('smartviewModal.cancel')}
          </button>
          <button
            type="submit"
            disabled={!name.trim()}
            className="px-4 py-1.5 rounded-lg bg-cyan-600 dark:bg-[#22D3EE] hover:opacity-90 disabled:opacity-50 text-white dark:text-[#0B0D10] text-xs font-semibold"
          >
            {t('smartviewModal.create')}
          </button>
        </div>
      </form>
    </div>
  );
}
