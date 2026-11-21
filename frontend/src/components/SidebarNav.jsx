import React, { useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import {
  Library,
  Star,
  Sparkles,
  BookOpen,
  CheckCircle2,
  HardDrive,
  Cloud,
  CloudDownload,
  FolderOpen,
  FolderPlus,
  FilePlus2,
  Plus,
  Bookmark,
  Pencil,
  Trash2,
  RefreshCw,
  Check,
  X,
  Info,
} from 'lucide-react';
import { api } from '../api/client';
import { formatDisplayTitle } from '../utils/format';

const SMARTVIEW_ICONS = {
  unread: BookOpen,
  reading: Sparkles,
  completed: CheckCircle2,
  large_files: HardDrive,
  offline_pinned: CloudDownload,
};

export default function SidebarNav({
  allComicsCount,
  activeFilter,
  onSelectFilter,
  smartviews,
  onOpenNewSmartview,
  onDeleteSmartview,
  shelves,
  smartRename = false,
  onCreateShelf,
  onRenameShelf,
  onDeleteShelf,
  folders,
  scanStatus,
  onPickFolder,
  onImportFolderPath,
  onPickFiles,
  onDeleteFolder,
  onOpenCloudImport,
  onOpenAbout,
}) {
  const { t } = useTranslation();

  const [addingShelf, setAddingShelf] = useState(false);
  const [newShelfName, setNewShelfName] = useState('');
  const [editingShelfId, setEditingShelfId] = useState(null);
  const [editingShelfName, setEditingShelfName] = useState('');

  const [cloudOpen, setCloudOpen] = useState(false);
  const [cloudLoading, setCloudLoading] = useState(false);
  const [cloudSources, setCloudSources] = useState([]);
  const [importingCloudPath, setImportingCloudPath] = useState('');
  const cloudPopoverRef = useRef(null);

  useEffect(() => {
    if (!cloudOpen) return;
    const onDocClick = (e) => {
      if (cloudPopoverRef.current && !cloudPopoverRef.current.contains(e.target)) {
        setCloudOpen(false);
      }
    };
    document.addEventListener('mousedown', onDocClick);
    return () => document.removeEventListener('mousedown', onDocClick);
  }, [cloudOpen]);

  const handleToggleCloudPopover = async () => {
    const next = !cloudOpen;
    setCloudOpen(next);
    if (!next) return;
    setCloudLoading(true);
    try {
      const res = await api.getCloudSources();
      setCloudSources(Array.isArray(res?.sources) ? res.sources : []);
    } catch {
      setCloudSources([]);
    } finally {
      setCloudLoading(false);
    }
  };

  const handleImportCloudSource = async (src) => {
    if (!onImportFolderPath || importingCloudPath) return;
    setImportingCloudPath(src.path);
    try {
      await onImportFolderPath(src.path);
      setCloudSources((prev) =>
        prev.map((item) =>
          item.path === src.path ? { ...item, already_added: true } : item
        )
      );
    } catch {
      // ignore if scan already active
    } finally {
      setImportingCloudPath('');
    }
  };

  const handleCreateShelfSubmit = async (e) => {
    e.preventDefault();
    const trimmed = newShelfName.trim();
    if (!trimmed) return;
    await onCreateShelf(trimmed);
    setNewShelfName('');
    setAddingShelf(false);
  };

  const handleRenameSubmit = async (e, id) => {
    e.preventDefault();
    const trimmed = editingShelfName.trim();
    if (!trimmed) return;
    await onRenameShelf(id, trimmed);
    setEditingShelfId(null);
  };

  const visibleSmartviews = smartviews.filter((sv) => sv.slug !== 'favorites');

  const navItemCls = (isActive) =>
    `w-full flex items-center justify-between px-2.5 py-1.5 rounded-lg text-xs font-medium transition-colors border-l-2 ${
      isActive
        ? 'border-cyan-600 dark:border-[#22D3EE] bg-cyan-500/10 dark:bg-[#22D3EE]/10 text-cyan-700 dark:text-[#22D3EE]'
        : 'border-transparent text-slate-700 dark:text-slate-300 hover:bg-[#F4F6F9] dark:hover:bg-[#171C24]'
    }`;

  return (
    <aside className="w-64 shrink-0 h-full bg-white dark:bg-[#12151B] border-r border-[#DCE1E8] dark:border-[#222834] flex flex-col overflow-hidden">
      {/* Brand Header */}
      <div className="h-14 px-4 flex items-center justify-between gap-2 border-b border-[#DCE1E8] dark:border-[#222834] shrink-0">
        <div className="flex items-center gap-2.5 min-w-0">
          <img src="/logo.svg" alt="Incuna Reader" className="w-8 h-8 shrink-0" />
          <div className="min-w-0">
            <div className="text-sm font-bold tracking-tight text-slate-900 dark:text-[#F1F5F9] leading-none">
              {t('app.title')}
            </div>
            <div className="text-[10px] font-mono uppercase tracking-wider text-slate-500 dark:text-slate-400 mt-1">
              {t('app.subtitle')}
            </div>
          </div>
        </div>
        <button
          type="button"
          onClick={onOpenAbout}
          title={t('about.title')}
          className="p-1.5 rounded-lg text-slate-400 hover:text-cyan-600 dark:hover:text-[#22D3EE] hover:bg-[#F4F6F9] dark:hover:bg-[#171C24] transition-colors shrink-0"
        >
          <Info className="w-4 h-4" />
        </button>
      </div>

      {/* Scrollable Sections */}
      <div className="flex-1 overflow-y-auto px-3 py-3 space-y-5">
        {/* Library Core */}
        <div>
          <div className="px-2.5 mb-1.5 text-[10px] font-semibold uppercase tracking-wider text-slate-400 dark:text-slate-500">
            {t('sidebar.library')}
          </div>
          <div className="space-y-0.5">
            <button
              type="button"
              onClick={() => onSelectFilter({ type: 'all' })}
              className={navItemCls(activeFilter.type === 'all')}
            >
              <span className="flex items-center gap-2">
                <Library className="w-4 h-4 shrink-0" />
                <span>{t('sidebar.allComics')}</span>
              </span>
              <span className="text-[11px] font-mono tabular-nums px-1.5 py-0.5 rounded bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] text-slate-600 dark:text-slate-300">
                {allComicsCount.toLocaleString()}
              </span>
            </button>

            <button
              type="button"
              onClick={() => onSelectFilter({ type: 'smartview', slug: 'favorites' })}
              className={navItemCls(
                activeFilter.type === 'smartview' && activeFilter.slug === 'favorites'
              )}
            >
              <span className="flex items-center gap-2">
                <Star className="w-4 h-4 shrink-0 text-amber-500" />
                <span>{t('sidebar.favorites')}</span>
              </span>
            </button>
          </div>
        </div>

        {/* SmartViews */}
        <div>
          <div className="px-2.5 mb-1.5 flex items-center justify-between">
            <span className="text-[10px] font-semibold uppercase tracking-wider text-slate-400 dark:text-slate-500">
              {t('sidebar.smartviews')}
            </span>
            <button
              type="button"
              onClick={onOpenNewSmartview}
              title={t('sidebar.newSmartview')}
              className="p-0.5 rounded text-slate-400 hover:text-brand-500 hover:bg-slate-100 dark:hover:bg-slate-800"
            >
              <Plus className="w-3.5 h-3.5" />
            </button>
          </div>
          <div className="space-y-0.5">
            {visibleSmartviews.map((sv) => {
              const Icon = SMARTVIEW_ICONS[sv.slug] || Sparkles;
              const isActive =
                activeFilter.type === 'smartview' && activeFilter.slug === sv.slug;
              const label = sv.is_builtin
                ? t(`smartviewNames.${sv.slug}`, { defaultValue: sv.name })
                : sv.name;

              return (
                <div key={sv.id} className="group flex items-center gap-1">
                  <button
                    type="button"
                    onClick={() => onSelectFilter({ type: 'smartview', slug: sv.slug })}
                    className={navItemCls(isActive)}
                  >
                    <span className="flex items-center gap-2 truncate">
                      <Icon className="w-4 h-4 shrink-0" />
                      <span className="truncate">{label}</span>
                    </span>
                  </button>
                  {!sv.is_builtin && (
                    <button
                      type="button"
                      onClick={() => onDeleteSmartview(sv.id)}
                      className="opacity-0 group-hover:opacity-100 p-1 text-slate-400 hover:text-rose-500 transition-opacity"
                    >
                      <Trash2 className="w-3.5 h-3.5" />
                    </button>
                  )}
                </div>
              );
            })}
          </div>
        </div>

        {/* Shelves */}
        <div>
          <div className="px-2.5 mb-1.5 flex items-center justify-between">
            <span className="text-[10px] font-semibold uppercase tracking-wider text-slate-400 dark:text-slate-500">
              {t('sidebar.shelves')}
            </span>
            <button
              type="button"
              onClick={() => setAddingShelf((v) => !v)}
              title={t('sidebar.newShelf')}
              className="p-0.5 rounded text-slate-400 hover:text-brand-500 hover:bg-slate-100 dark:hover:bg-slate-800"
            >
              <Plus className="w-3.5 h-3.5" />
            </button>
          </div>

          {addingShelf && (
            <form onSubmit={handleCreateShelfSubmit} className="px-1.5 mb-2 flex items-center gap-1">
              <input
                type="text"
                autoFocus
                value={newShelfName}
                onChange={(e) => setNewShelfName(e.target.value)}
                placeholder={t('sidebar.shelfPlaceholder')}
                className="flex-1 min-w-0 px-2 py-1 text-xs rounded-md bg-slate-100 dark:bg-slate-800 border border-slate-300 dark:border-slate-700 focus:outline-none focus:border-brand-500"
              />
              <button
                type="submit"
                className="p-1 rounded-md bg-brand-600 text-white hover:bg-brand-500"
              >
                <Check className="w-3.5 h-3.5" />
              </button>
              <button
                type="button"
                onClick={() => setAddingShelf(false)}
                className="p-1 rounded-md text-slate-400 hover:text-slate-600"
              >
                <X className="w-3.5 h-3.5" />
              </button>
            </form>
          )}

          <div className="space-y-0.5">
            {shelves.map((shelf) => {
              const isActive =
                activeFilter.type === 'shelf' && activeFilter.shelfId === shelf.id;

              if (editingShelfId === shelf.id) {
                return (
                  <form
                    key={shelf.id}
                    onSubmit={(e) => handleRenameSubmit(e, shelf.id)}
                    className="px-1.5 py-0.5 flex items-center gap-1"
                  >
                    <input
                      type="text"
                      autoFocus
                      value={editingShelfName}
                      onChange={(e) => setEditingShelfName(e.target.value)}
                      className="flex-1 min-w-0 px-2 py-1 text-xs rounded-md bg-slate-100 dark:bg-slate-800 border border-brand-500 focus:outline-none"
                    />
                    <button type="submit" className="p-1 text-emerald-500">
                      <Check className="w-3.5 h-3.5" />
                    </button>
                    <button
                      type="button"
                      onClick={() => setEditingShelfId(null)}
                      className="p-1 text-slate-400"
                    >
                      <X className="w-3.5 h-3.5" />
                    </button>
                  </form>
                );
              }

              return (
                <div key={shelf.id} className="group flex items-center gap-0.5">
                  <button
                    type="button"
                    onClick={() =>
                      onSelectFilter({ type: 'shelf', shelfId: shelf.id, name: shelf.name })
                    }
                    className={navItemCls(isActive)}
                  >
                    <span className="flex items-center gap-2 truncate">
                      <Bookmark className="w-4 h-4 shrink-0" />
                      <span className="truncate">{formatDisplayTitle(shelf.name, smartRename)}</span>
                    </span>
                    <span className="text-[11px] font-mono tabular-nums text-slate-400">
                      {shelf.comic_count ?? 0}
                    </span>
                  </button>
                  <div className="hidden group-hover:flex items-center shrink-0">
                    <button
                      type="button"
                      title={t('sidebar.renameShelf')}
                      onClick={() => {
                        setEditingShelfId(shelf.id);
                        setEditingShelfName(shelf.name);
                      }}
                      className="p-1 text-slate-400 hover:text-brand-500"
                    >
                      <Pencil className="w-3 h-3" />
                    </button>
                    <button
                      type="button"
                      title={t('sidebar.deleteShelf')}
                      onClick={() => onDeleteShelf(shelf.id)}
                      className="p-1 text-slate-400 hover:text-rose-500"
                    >
                      <Trash2 className="w-3 h-3" />
                    </button>
                  </div>
                </div>
              );
            })}
          </div>
        </div>

        {/* Watched Folders */}
        <div className="relative" ref={cloudPopoverRef}>
          <div className="px-2.5 mb-2 flex items-center justify-between gap-1">
            <span className="text-[10px] font-semibold uppercase tracking-wider text-slate-400 dark:text-slate-500">
              {t('sidebar.watchedFolders')}
            </span>
            <button
              type="button"
              onClick={handleToggleCloudPopover}
              title={t('sidebar.cloudFoldersHint', { defaultValue: 'Detect OneDrive, Google Drive & Dropbox' })}
              className={`inline-flex items-center gap-1 px-1.5 py-0.5 rounded text-[10px] font-semibold transition-colors ${
                cloudOpen
                  ? 'bg-cyan-500/15 text-cyan-600 dark:text-[#22D3EE]'
                  : 'text-slate-500 dark:text-slate-400 hover:text-cyan-600 dark:hover:text-[#22D3EE] hover:bg-slate-100 dark:hover:bg-slate-800'
              }`}
            >
              <Cloud className="w-3 h-3 shrink-0" />
              <span>{t('sidebar.cloudFolders', { defaultValue: 'Cloud Folders' })}</span>
            </button>
          </div>

          {cloudOpen && (
            <div className="mb-2.5 p-2 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] shadow-lg space-y-1.5">
              <div className="flex items-center justify-between px-1">
                <span className="text-[10px] font-semibold uppercase tracking-wider text-slate-500 dark:text-slate-400">
                  {t('sidebar.detectedCloud', { defaultValue: 'Detected Cloud Sync' })}
                </span>
                <button
                  type="button"
                  onClick={() => setCloudOpen(false)}
                  className="p-0.5 text-slate-400 hover:text-slate-600 dark:hover:text-slate-200"
                >
                  <X className="w-3 h-3" />
                </button>
              </div>
              {cloudLoading ? (
                <div className="px-1 py-2 text-[11px] text-slate-400 flex items-center gap-1.5">
                  <RefreshCw className="w-3 h-3 animate-spin" />
                  <span>{t('sidebar.scanningCloud', { defaultValue: 'Scanning cloud folders...' })}</span>
                </div>
              ) : cloudSources.length === 0 ? (
                <p className="px-1 py-1.5 text-[11px] text-slate-400 dark:text-slate-500">
                  {t('sidebar.noCloudFolders', { defaultValue: 'No OneDrive, Google Drive, or Dropbox folders found.' })}
                </p>
              ) : (
                <div className="space-y-1 max-h-44 overflow-y-auto">
                  {cloudSources.map((src) => (
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
                          disabled={Boolean(importingCloudPath)}
                          onClick={() => handleImportCloudSource(src)}
                          className="px-2 py-1 rounded bg-cyan-600 dark:bg-[#22D3EE] text-white dark:text-[#0B0D10] text-[10px] font-semibold hover:opacity-90 disabled:opacity-50 shrink-0"
                        >
                          {importingCloudPath === src.path
                            ? '...'
                            : t('sidebar.add', { defaultValue: 'Add' })}
                        </button>
                      )}
                    </div>
                  ))}
                </div>
              )}
            </div>
          )}

          <div className="grid grid-cols-2 gap-1.5 mb-2.5">
            <button
              type="button"
              onClick={onPickFolder}
              className="flex items-center justify-center gap-1.5 px-2.5 py-2 rounded-lg bg-cyan-600 dark:bg-[#22D3EE] hover:opacity-90 text-white dark:text-[#0B0D10] text-[11px] font-semibold transition-opacity"
            >
              <FolderPlus className="w-3.5 h-3.5 shrink-0" />
              <span className="truncate">{t('sidebar.importFolder')}</span>
            </button>
            <button
              type="button"
              onClick={onPickFiles}
              className="flex items-center justify-center gap-1.5 px-2.5 py-2 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] hover:border-cyan-600 dark:hover:border-[#22D3EE] text-slate-700 dark:text-[#F1F5F9] text-[11px] font-semibold transition-colors"
            >
              <FilePlus2 className="w-3.5 h-3.5 shrink-0 text-cyan-600 dark:text-[#22D3EE]" />
              <span className="truncate">{t('sidebar.openFile')}</span>
            </button>
          </div>

          <div className="grid grid-cols-2 gap-1.5 mb-2.5">
            <button
              type="button"
              onClick={() => onOpenCloudImport?.('gdrive')}
              className="flex items-center justify-center gap-1.5 px-2.5 py-1.5 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] hover:border-cyan-600 dark:hover:border-[#22D3EE] text-slate-700 dark:text-[#F1F5F9] text-[11px] font-semibold transition-colors"
            >
              <Cloud className="w-3.5 h-3.5 shrink-0 text-emerald-500" />
              <span className="truncate">Google Drive</span>
            </button>
            <button
              type="button"
              onClick={() => onOpenCloudImport?.('onedrive')}
              className="flex items-center justify-center gap-1.5 px-2.5 py-1.5 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] hover:border-cyan-600 dark:hover:border-[#22D3EE] text-slate-700 dark:text-[#F1F5F9] text-[11px] font-semibold transition-colors"
            >
              <Cloud className="w-3.5 h-3.5 shrink-0 text-sky-500" />
              <span className="truncate">OneDrive</span>
            </button>
          </div>

          {folders.length === 0 ? (
            <p className="px-2.5 text-[11px] text-slate-400 dark:text-slate-500">
              {t('sidebar.noFolders')}
            </p>
          ) : (
            <div className="space-y-1">
              {folders.map((folder) => {
                const isOnline = folder.is_online !== false;
                return (
                  <div
                    key={folder.id}
                    className="group flex items-center justify-between px-2.5 py-1.5 rounded-lg text-xs text-slate-600 dark:text-slate-400 hover:bg-slate-100 dark:hover:bg-slate-800/60"
                  >
                    <div className="flex items-center gap-2 min-w-0">
                      <span
                        title={
                          isOnline
                            ? t('sidebar.folderOnline', { defaultValue: 'Online' })
                            : t('sidebar.folderOffline', { defaultValue: 'Drive Unplugged / Offline' })
                        }
                        className={`w-2 h-2 rounded-full shrink-0 ${
                          isOnline
                            ? 'bg-cyan-500 dark:bg-[#22D3EE]'
                            : 'bg-amber-500'
                        }`}
                      />
                      <FolderOpen className="w-3.5 h-3.5 shrink-0 text-slate-400" />
                      <span
                        title={`${isOnline ? 'Online' : 'Drive Unplugged / Offline'}: ${folder.path}`}
                        className={`truncate ${!isOnline ? 'text-amber-600 dark:text-amber-400' : ''}`}
                      >
                        {folder.name || folder.path}
                      </span>
                    </div>
                    <div className="flex items-center gap-1 shrink-0">
                      {!isOnline && (
                        <span className="text-[9px] font-mono uppercase px-1 py-0.5 rounded bg-amber-500/15 text-amber-600 dark:text-amber-400">
                          {t('sidebar.offlineBadge', { defaultValue: 'Offline' })}
                        </span>
                      )}
                      <button
                        type="button"
                        title={t('sidebar.removeFolder')}
                        onClick={() => onDeleteFolder(folder.id)}
                        className="opacity-0 group-hover:opacity-100 p-0.5 text-slate-400 hover:text-rose-500 transition-opacity"
                      >
                        <Trash2 className="w-3 h-3" />
                      </button>
                    </div>
                  </div>
                );
              })}
            </div>
          )}
        </div>
      </div>

      {/* Live Auto-Sync Status Footer */}
      <div className="px-3.5 py-2.5 border-t border-[#DCE1E8] dark:border-[#222834] bg-[#F4F6F9] dark:bg-[#0B0D10] shrink-0">
        {scanStatus?.scanning ? (
          <div className="flex items-center gap-2 text-[11px] font-mono tabular-nums text-cyan-700 dark:text-[#22D3EE] font-medium">
            <RefreshCw className="w-3.5 h-3.5 animate-spin shrink-0" />
            <span className="truncate">
              {t('sidebar.scanning', { count: (scanStatus.files_indexed ?? 0).toLocaleString() })}
            </span>
          </div>
        ) : (
          <div className="flex items-center gap-2 text-[11px] font-mono text-emerald-600 dark:text-emerald-400 font-medium">
            <span className="w-2 h-2 rounded-full bg-emerald-500 animate-pulse shrink-0" />
            <span className="truncate">{t('sidebar.autoSyncActive')}</span>
          </div>
        )}
      </div>
    </aside>
  );
}
