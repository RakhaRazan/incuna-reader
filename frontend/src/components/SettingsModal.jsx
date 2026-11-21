import React, { useEffect, useState } from 'react';
import { useTranslation } from 'react-i18next';
import {
  X,
  Cpu,
  Database,
  Trash2,
  Play,
  RotateCw,
  CheckCircle2,
  CloudDownload,
  FileCheck2,
  Sliders,
  Keyboard,
  HardDrive,
  RotateCcw,
} from 'lucide-react';
import { api } from '../api/client';
import { formatBytes } from '../utils/format';
import {
  SHORTCUT_DEFINITIONS,
  DEFAULT_SHORTCUTS,
  resolveShortcuts,
  formatKeyEvent,
} from '../hooks/useKeyboardShortcuts';

function ToggleRow({ label, description, checked, disabled, onToggle }) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={checked}
      disabled={disabled}
      onClick={onToggle}
      className="w-full flex items-center justify-between gap-4 px-3.5 py-2.5 rounded-lg border border-[#DCE1E8] dark:border-[#222834] bg-[#F4F6F9]/50 dark:bg-[#0B0D10]/50 hover:border-cyan-600/50 dark:hover:border-[#22D3EE]/50 transition-colors text-left disabled:opacity-50"
    >
      <div className="min-w-0">
        <div className="text-xs font-semibold text-slate-900 dark:text-[#F1F5F9]">
          {label}
        </div>
        {description && (
          <div className="text-[11px] text-slate-500 dark:text-slate-400 mt-0.5 leading-snug">
            {description}
          </div>
        )}
      </div>
      <span
        className={`relative inline-flex h-5 w-9 shrink-0 items-center rounded-full transition-colors ${
          checked ? 'bg-cyan-600 dark:bg-[#22D3EE]' : 'bg-slate-300 dark:bg-slate-700'
        }`}
      >
        <span
          className={`inline-block h-3.5 w-3.5 transform rounded-full bg-white dark:bg-[#0B0D10] shadow transition-transform ${
            checked ? 'translate-x-4.5' : 'translate-x-1'
          }`}
          style={{ transform: checked ? 'translateX(18px)' : 'translateX(3px)' }}
        />
      </span>
    </button>
  );
}

const READING_TOGGLES = [
  {
    key: 'default_manga_mode',
    label: 'Enable manga mode by default',
    description: 'Open books with right-to-left (RTL) reading direction by default.',
    defaultVal: false,
  },
  {
    key: 'smart_page_layout',
    label: 'Smart page layout',
    description: 'In Dual-Page mode, auto-detect wide spread images and render them solo at full width.',
    defaultVal: true,
  },
  {
    key: 'auto_detect_borders',
    label: 'Auto detect page borders',
    description: 'Enable automatic margin/border cropping when opening a book.',
    defaultVal: false,
  },
  {
    key: 'stack_pages_vertically',
    label: 'Stack pages vertically',
    description: 'Use vertical Webtoon scroll layout by default when opening books.',
    defaultVal: false,
  },
  {
    key: 'keep_screen_active',
    label: 'Keep screen active while reading',
    description: 'Prevent display sleep while the reader is open.',
    defaultVal: true,
  },
  {
    key: 'circular_progress_bar',
    label: 'Enable circular progress bar',
    description: 'Show a circular reading progress ring on the top-right of library cards.',
    defaultVal: true,
  },
];

const LIBRARY_TOGGLES = [
  {
    key: 'display_folders_before_files',
    label: 'Display folders before files',
    description: 'Show folder cards ahead of comic files in Folder / Series View.',
    defaultVal: true,
  },
  {
    key: 'smart_rename',
    label: 'Smart rename books and shelves',
    description: 'Clean displayed titles by stripping [Group], (Year), underscores, and file extensions.',
    defaultVal: false,
  },
  {
    key: 'auto_refresh_library',
    label: 'Auto refresh library',
    description: 'Automatically refresh the library view when background scans detect changes.',
    defaultVal: true,
  },
  {
    key: 'show_hidden_elements',
    label: 'Show hidden elements in library',
    description: 'Include hidden comics in library queries and folder counts.',
    defaultVal: false,
  },
];

const FEATURE_TOGGLES = [
  {
    key: 'enable_loupe_magnifier',
    label: 'Loupe Magnifier',
    description: 'Hold right-click magnifying glass in reader.',
    defaultVal: true,
  },
  {
    key: 'enable_manga_enhancer',
    label: 'Real-Time Manga Enhancer',
    description: 'WebGL ink sharpening & black-level contrast.',
    defaultVal: false,
  },
  {
    key: 'enable_continue_reading',
    label: 'Continue Reading Banner & History',
    description: 'Recent reads strip at top of library.',
    defaultVal: true,
  },
  {
    key: 'enable_auto_scroll',
    label: 'Auto-Scroll',
    description: 'Hands-free vertical/Webtoon scrolling controls.',
    defaultVal: true,
  },
  {
    key: 'enable_metadata_extractor',
    label: 'Tag & Author Extractor',
    description: 'ComicInfo.xml & PDF metadata badges + click-to-filter.',
    defaultVal: true,
  },
  {
    key: 'enable_random_button',
    label: 'Random Comic Button',
    description: 'Quick dice button in top toolbar to jump into a random unread manga.',
    defaultVal: true,
  },
];

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
  const resolvedShortcuts = resolveShortcuts(settings?.shortcuts);

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

  const pct =
    packStatus?.total > 0
      ? Math.min(100, Math.round((packStatus.processed / packStatus.total) * 100))
      : 0;

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
            className={`inline-flex items-center gap-1.5 px-3 py-2 text-xs font-semibold border-b-2 transition-colors ${
              activeTab === 'general'
                ? 'border-cyan-600 dark:border-[#22D3EE] text-cyan-700 dark:text-[#22D3EE]'
                : 'border-transparent text-slate-500 dark:text-slate-400 hover:text-slate-800 dark:hover:text-slate-200'
            }`}
          >
            <Sliders className="w-3.5 h-3.5" />
            <span>General</span>
          </button>
          <button
            type="button"
            onClick={() => setActiveTab('shortcuts')}
            className={`inline-flex items-center gap-1.5 px-3 py-2 text-xs font-semibold border-b-2 transition-colors ${
              activeTab === 'shortcuts'
                ? 'border-cyan-600 dark:border-[#22D3EE] text-cyan-700 dark:text-[#22D3EE]'
                : 'border-transparent text-slate-500 dark:text-slate-400 hover:text-slate-800 dark:hover:text-slate-200'
            }`}
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
            className={`inline-flex items-center gap-1.5 px-3 py-2 text-xs font-semibold border-b-2 transition-colors ${
              activeTab === 'storage'
                ? 'border-cyan-600 dark:border-[#22D3EE] text-cyan-700 dark:text-[#22D3EE]'
                : 'border-transparent text-slate-500 dark:text-slate-400 hover:text-slate-800 dark:hover:text-slate-200'
            }`}
          >
            <HardDrive className="w-3.5 h-3.5" />
            <span>Cover &amp; Storage</span>
          </button>
        </div>

        {/* Body */}
        <div className="p-6 space-y-5 overflow-y-auto flex-1">
          {activeTab === 'general' && (
            <>
              <div className="space-y-2">
                <h3 className="text-xs font-bold uppercase tracking-wider text-slate-500 dark:text-slate-400">
                  Reading
                </h3>
                <div className="space-y-2">
                  {READING_TOGGLES.map((item) => (
                    <ToggleRow
                      key={item.key}
                      label={item.label}
                      description={item.description}
                      checked={
                        settings?.[item.key] !== undefined
                          ? Boolean(settings[item.key])
                          : item.defaultVal
                      }
                      disabled={busy}
                      onToggle={() => handleToggleSetting(item.key, item.defaultVal)}
                    />
                  ))}
                </div>
              </div>

              <div className="space-y-2 pt-2 border-t border-[#DCE1E8] dark:border-[#222834]">
                <h3 className="text-xs font-bold uppercase tracking-wider text-slate-500 dark:text-slate-400">
                  Library settings
                </h3>
                <div className="space-y-2">
                  {LIBRARY_TOGGLES.map((item) => (
                    <ToggleRow
                      key={item.key}
                      label={item.label}
                      description={item.description}
                      checked={
                        settings?.[item.key] !== undefined
                          ? Boolean(settings[item.key])
                          : item.defaultVal
                      }
                      disabled={busy}
                      onToggle={() => handleToggleSetting(item.key, item.defaultVal)}
                    />
                  ))}
                </div>
              </div>

              <div className="space-y-2 pt-2 border-t border-[#DCE1E8] dark:border-[#222834]">
                <h3 className="text-xs font-bold uppercase tracking-wider text-slate-500 dark:text-slate-400">
                  Features &amp; Enhancements
                </h3>
                <div className="space-y-2">
                  {FEATURE_TOGGLES.map((item) => (
                    <ToggleRow
                      key={item.key}
                      label={item.label}
                      description={item.description}
                      checked={
                        settings?.[item.key] !== undefined
                          ? Boolean(settings[item.key])
                          : item.defaultVal
                      }
                      disabled={busy}
                      onToggle={() => handleToggleSetting(item.key, item.defaultVal)}
                    />
                  ))}
                </div>
              </div>
            </>
          )}

          {activeTab === 'shortcuts' && (
            <div className="space-y-3">
              <div className="flex items-center justify-between gap-2">
                <p className="text-xs text-slate-500 dark:text-slate-400">
                  Click <span className="font-semibold text-slate-700 dark:text-slate-200">Edit</span> and press any key combination to rebind.
                </p>
                <button
                  type="button"
                  disabled={busy}
                  onClick={handleResetAllShortcuts}
                  className="inline-flex items-center gap-1.5 px-3 py-1.5 rounded-lg bg-slate-200/80 dark:bg-[#171C24] hover:bg-slate-300/80 dark:hover:bg-[#1E2530] disabled:opacity-50 text-slate-700 dark:text-slate-200 text-xs font-semibold transition-colors shrink-0"
                >
                  <RotateCcw className="w-3.5 h-3.5" />
                  <span>Reset all shortcuts</span>
                </button>
              </div>

              <div className="divide-y divide-[#DCE1E8] dark:divide-[#222834] rounded-lg border border-[#DCE1E8] dark:border-[#222834] bg-[#F4F6F9]/40 dark:bg-[#0B0D10]/40 overflow-hidden">
                {SHORTCUT_DEFINITIONS.map((def) => {
                  const keys = resolvedShortcuts[def.id] || def.defaults;
                  const isRecording = recordingAction === def.id;

                  return (
                    <div
                      key={def.id}
                      className="px-3.5 py-2.5 flex items-center justify-between gap-3"
                    >
                      <span className="text-xs font-medium text-slate-800 dark:text-[#F1F5F9]">
                        {def.label}
                      </span>

                      <div className="flex items-center gap-2 shrink-0">
                        {isRecording ? (
                          <span className="px-2.5 py-1 rounded bg-cyan-500/15 border border-cyan-500/50 text-cyan-700 dark:text-[#22D3EE] text-[11px] font-mono animate-pulse">
                            Press any key...
                          </span>
                        ) : (
                          <div className="flex items-center gap-1 flex-wrap justify-end">
                            {keys.map((k) => (
                              <kbd
                                key={k}
                                className="px-2 py-0.5 rounded bg-white dark:bg-[#171C24] border border-[#DCE1E8] dark:border-[#2A3241] shadow-sm text-[11px] font-mono font-semibold text-slate-700 dark:text-slate-200"
                              >
                                {k}
                              </kbd>
                            ))}
                          </div>
                        )}

                        {isRecording ? (
                          <button
                            type="button"
                            onClick={() => setRecordingAction(null)}
                            className="px-2.5 py-1 rounded-md bg-slate-200 dark:bg-[#171C24] text-slate-700 dark:text-slate-300 text-[11px] font-semibold hover:opacity-90"
                          >
                            Cancel
                          </button>
                        ) : (
                          <button
                            type="button"
                            disabled={busy}
                            onClick={() => setRecordingAction(def.id)}
                            className="px-2.5 py-1 rounded-md bg-white dark:bg-[#171C24] border border-[#DCE1E8] dark:border-[#222834] hover:border-cyan-600 dark:hover:border-[#22D3EE] text-slate-700 dark:text-slate-200 text-[11px] font-semibold transition-colors disabled:opacity-50"
                          >
                            Edit
                          </button>
                        )}
                      </div>
                    </div>
                  );
                })}
              </div>
            </div>
          )}

          {activeTab === 'storage' && (
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
                onClick={() => handleSelectMode('ram_only')}
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
                onClick={() => handleSelectMode('persistent_pack')}
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
                          onClick={handleBuildPack}
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
                          onClick={handleRebuildCrispCovers}
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
                            onClick={handlePurgePack}
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
                    onClick={handleClearUnpinnedCache}
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
                          onChange={(e) => setIncludePdfAssoc(e.target.checked)}
                          className="rounded border-slate-400 text-brand-600 focus:ring-brand-500"
                        />
                        <span>{t('settings.includePdfAssoc', { defaultValue: 'Also associate .pdf files' })}</span>
                      </label>
                    </div>
                  </div>
                  <button
                    type="button"
                    disabled={busy}
                    onClick={handleRegisterFileTypes}
                    className="inline-flex items-center gap-1.5 px-3 py-1.5 rounded-lg bg-brand-600 hover:bg-brand-500 disabled:opacity-50 text-white text-xs font-semibold transition-colors shrink-0"
                  >
                    <FileCheck2 className="w-3.5 h-3.5" />
                    <span>{t('settings.setDefaultReader', { defaultValue: 'Set as Default Comic Reader' })}</span>
                  </button>
                </div>
              </div>
            </>
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
