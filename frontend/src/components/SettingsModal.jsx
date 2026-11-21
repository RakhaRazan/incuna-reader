import React, { useEffect, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { X, Sliders, Keyboard, HardDrive } from 'lucide-react';
import { api } from '../api/client';
import { formatBytes } from '../utils/format';
import {
  DEFAULT_SHORTCUTS,
  resolveShortcuts,
  formatKeyEvent,
} from '../hooks/useKeyboardShortcuts';
import GeneralSettingsTab from './settings/GeneralSettingsTab';
import FeaturesSettingsTab from './settings/FeaturesSettingsTab';
import ShortcutsSettingsTab from './settings/ShortcutsSettingsTab';
import StorageSettingsTab from './settings/StorageSettingsTab';

export default function SettingsModal({
  open,
  onClose,
  settings,
  health,
  onSettingsUpdated,
  onCoversChanged,
}) {
  const { t } = useTranslation();
  const [activeTab, setActiveTab] = useState('general');
  const [packStatus, setPackStatus] = useState(null);
  const [offlineStats, setOfflineStats] = useState(null);
  const [includePdfAssoc, setIncludePdfAssoc] = useState(false);
  const [busy, setBusy] = useState(false);
  const [notice, setNotice] = useState('');
  const [recordingAction, setRecordingAction] = useState(null);

  const coverMode = settings?.cover_mode || 'persistent_pack';

  useEffect(() => {
    if (!open) {
      setRecordingAction(null);
      return;
    }
    setNotice('');
    api.getCoverPackStatus().then(setPackStatus).catch(() => {});
    api.getOfflineStats().then(setOfflineStats).catch(() => {});
  }, [open]);

  // Poll pack build progress while building
  useEffect(() => {
    if (!open || !packStatus?.building) return;
    const timer = setInterval(() => {
      api
        .getCoverPackStatus()
        .then((st) => {
          setPackStatus(st);
          if (!st.building) {
            onCoversChanged?.();
          }
        })
        .catch(() => {});
    }, 450);
    return () => clearInterval(timer);
  }, [open, packStatus?.building, onCoversChanged]);

  // Key capture listener when recording a shortcut
  useEffect(() => {
    if (!open || !recordingAction) return;

    const handleCapture = async (e) => {
      e.preventDefault();
      e.stopPropagation();
      const combo = formatKeyEvent(e);
      if (!combo) return;

      const currentMap = resolveShortcuts(settings?.shortcuts);
      const nextMap = {
        ...currentMap,
        [recordingAction]: [combo],
      };
      setRecordingAction(null);
      setBusy(true);
      try {
        const updated = await api.updateSettings({ shortcuts: nextMap });
        onSettingsUpdated?.(updated);
      } finally {
        setBusy(false);
      }
    };

    window.addEventListener('keydown', handleCapture, true);
    return () => window.removeEventListener('keydown', handleCapture, true);
  }, [open, recordingAction, settings?.shortcuts, onSettingsUpdated]);

  if (!open) return null;

  const handleToggleSetting = async (key, defaultVal) => {
    if (busy) return;
    const current = settings?.[key] !== undefined ? Boolean(settings[key]) : defaultVal;
    const next = !current;
    setBusy(true);
    setNotice('');
    try {
      const updated = await api.updateSettings({ [key]: next });
      onSettingsUpdated?.(updated);
    } finally {
      setBusy(false);
    }
  };

  const handleResetAllShortcuts = async () => {
    if (busy) return;
    setRecordingAction(null);
    setBusy(true);
    setNotice('');
    try {
      const updated = await api.updateSettings({ shortcuts: { ...DEFAULT_SHORTCUTS } });
      onSettingsUpdated?.(updated);
      setNotice('All keyboard shortcuts restored to defaults.');
    } finally {
      setBusy(false);
    }
  };

  const handleSelectMode = async (mode) => {
    if (busy || mode === coverMode) return;
    setBusy(true);
    setNotice('');
    try {
      const updated = await api.updateSettings({ cover_mode: mode });
      onSettingsUpdated?.(updated);

      if (mode === 'persistent_pack') {
        const st = await api.buildCoverPack();
        setPackStatus(st);
      } else {
        const st = await api.getCoverPackStatus();
        setPackStatus(st);
        setNotice(t('settings.purgeSuccess'));
        onCoversChanged?.();
      }
    } finally {
      setBusy(false);
    }
  };

  const handleBuildPack = async () => {
    setBusy(true);
    setNotice('');
    try {
      const st = await api.buildCoverPack();
      setPackStatus(st);
    } finally {
      setBusy(false);
    }
  };

  const handleRebuildCrispCovers = async () => {
    setBusy(true);
    setNotice('');
    try {
      const st = await api.rebuildCoverPack();
      setPackStatus(st);
      onCoversChanged?.();
      setNotice(
        t('settings.rebuildCrispNotice', {
          defaultValue: 'Rebuilding crisp anti-aliased cover thumbnails in the background...',
        })
      );
    } finally {
      setBusy(false);
    }
  };

  const handlePurgePack = async () => {
    setBusy(true);
    setNotice('');
    try {
      await api.purgeCoverPack();
      if (coverMode !== 'ram_only') {
        const updated = await api.updateSettings({ cover_mode: 'ram_only' });
        onSettingsUpdated?.(updated);
      }
      const st = await api.getCoverPackStatus();
      setPackStatus(st);
      setNotice(t('settings.purgeSuccess'));
      onCoversChanged?.();
    } finally {
      setBusy(false);
    }
  };

  const handleClearUnpinnedCache = async () => {
    setBusy(true);
    setNotice('');
    try {
      const res = await api.clearUnpinnedCache();
      const stats = await api.getOfflineStats();
      setOfflineStats(stats);
      setNotice(
        t('settings.clearUnpinnedSuccess', {
          count: res?.removed_files ?? 0,
          size: formatBytes(res?.freed_bytes ?? 0),
          defaultValue: `Cleared ${res?.removed_files ?? 0} unpinned cache file(s) (${formatBytes(res?.freed_bytes ?? 0)} freed).`,
        })
      );
    } finally {
      setBusy(false);
    }
  };

  const handleRegisterFileTypes = async () => {
    setBusy(true);
    setNotice('');
    try {
      const res = await api.registerFileTypes(includePdfAssoc);
      const exts = (res?.extensions || []).join(', ');
      setNotice(
        t('settings.fileAssocSuccess', {
          extensions: exts,
          defaultValue: `Registered ${exts} in Windows Explorer (HKCU). Double-click any comic file to open in Incuna Reader.`,
        })
      );
    } catch (err) {
      setNotice(err?.message || 'Failed to register file associations.');
    } finally {
      setBusy(false);
    }
  };

  const tabBtnCls = (tab) =>
    `inline-flex items-center gap-1.5 px-3 py-2 text-xs font-semibold border-b-2 transition-colors ${
      activeTab === tab
        ? 'border-cyan-600 dark:border-[#22D3EE] text-cyan-700 dark:text-[#22D3EE]'
        : 'border-transparent text-slate-500 dark:text-slate-400 hover:text-slate-800 dark:hover:text-slate-200'
    }`;

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/65 backdrop-blur-sm p-4">
      <div className="w-full max-w-2xl rounded-xl bg-white dark:bg-[#12151B] border border-[#DCE1E8] dark:border-[#222834] shadow-2xl overflow-hidden flex flex-col max-h-[88vh]">
        {/* Header */}
        <div className="px-6 py-4 border-b border-[#DCE1E8] dark:border-[#222834] flex items-center justify-between shrink-0">
          <div>
            <h2 className="text-base font-bold text-slate-900 dark:text-[#F1F5F9]">
              {t('settings.title')}
            </h2>
            <p className="text-xs text-slate-500 dark:text-slate-400 mt-0.5">
              {t('settings.subtitle')}
            </p>
          </div>
          <button
            type="button"
            onClick={onClose}
            className="p-1.5 rounded-lg text-slate-400 hover:text-slate-700 dark:hover:text-[#F1F5F9] hover:bg-[#F4F6F9] dark:hover:bg-[#171C24]"
          >
            <X className="w-4 h-4" />
          </button>
        </div>

        {/* Tabs */}
        <div className="px-6 pt-3 border-b border-[#DCE1E8] dark:border-[#222834] flex items-center gap-1 shrink-0 bg-[#F4F6F9]/40 dark:bg-[#0B0D10]/40">
          <button
            type="button"
            onClick={() => {
              setActiveTab('general');
              setRecordingAction(null);
            }}
            className={tabBtnCls('general')}
          >
            <Sliders className="w-3.5 h-3.5" />
            <span>General</span>
          </button>
          <button
            type="button"
            onClick={() => setActiveTab('shortcuts')}
            className={tabBtnCls('shortcuts')}
          >
            <Keyboard className="w-3.5 h-3.5" />
            <span>Keyboard shortcuts</span>
          </button>
          <button
            type="button"
            onClick={() => {
              setActiveTab('storage');
              setRecordingAction(null);
            }}
            className={tabBtnCls('storage')}
          >
            <HardDrive className="w-3.5 h-3.5" />
            <span>Cover &amp; Storage</span>
          </button>
        </div>

        {/* Body */}
        <div className="p-6 space-y-5 overflow-y-auto flex-1">
          {activeTab === 'general' && (
            <>
              <GeneralSettingsTab
                settings={settings}
                busy={busy}
                onToggleSetting={handleToggleSetting}
              />
              <FeaturesSettingsTab
                settings={settings}
                busy={busy}
                onToggleSetting={handleToggleSetting}
              />
            </>
          )}

          {activeTab === 'shortcuts' && (
            <ShortcutsSettingsTab
              shortcuts={settings?.shortcuts}
              busy={busy}
              recordingAction={recordingAction}
              onStartRecord={setRecordingAction}
              onCancelRecord={() => setRecordingAction(null)}
              onResetAll={handleResetAllShortcuts}
            />
          )}

          {activeTab === 'storage' && (
            <StorageSettingsTab
              coverMode={coverMode}
              health={health}
              packStatus={packStatus}
              offlineStats={offlineStats}
              includePdfAssoc={includePdfAssoc}
              busy={busy}
              onSelectMode={handleSelectMode}
              onBuildPack={handleBuildPack}
              onRebuildCrispCovers={handleRebuildCrispCovers}
              onPurgePack={handlePurgePack}
              onClearUnpinnedCache={handleClearUnpinnedCache}
              onIncludePdfChange={setIncludePdfAssoc}
              onRegisterFileTypes={handleRegisterFileTypes}
            />
          )}

          {notice && (
            <div className="px-3.5 py-2 rounded-lg bg-emerald-500/10 border border-emerald-500/30 text-xs text-emerald-600 dark:text-emerald-400">
              {notice}
            </div>
          )}
        </div>

        {/* Footer */}
        <div className="px-6 py-3.5 bg-slate-50 dark:bg-[#10131d] border-t border-slate-200 dark:border-slate-800 flex justify-end shrink-0">
          <button
            type="button"
            onClick={onClose}
            className="px-4 py-1.5 rounded-lg bg-slate-900 dark:bg-white text-white dark:text-slate-900 text-xs font-semibold hover:opacity-90 transition-opacity"
          >
            {t('settings.close')}
          </button>
        </div>
      </div>
    </div>
  );
}
