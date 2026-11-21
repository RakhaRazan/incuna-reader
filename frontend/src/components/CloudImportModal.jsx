import React, { useEffect, useState, useCallback, useRef } from 'react';
import {
  X,
  Cloud,
  HardDrive,
  Folder,
  FileText,
  ChevronRight,
  RefreshCw,
  Check,
  LogOut,
  Download,
  ExternalLink,
} from 'lucide-react';
import { api, isAbortError } from '../api/client';
import { formatBytes } from '../utils/format';

const PROVIDERS = [
  { id: 'gdrive', name: 'Google Drive', hint: 'Google Drive for Desktop or OAuth Client ID' },
  { id: 'onedrive', name: 'OneDrive', hint: 'OneDrive Sync Folder or Microsoft App Client ID' },
];

export default function CloudImportModal({
  open,
  initialProvider = 'gdrive',
  onClose,
  onImportFolderPath,
  onImportCompleted,
}) {
  const [provider, setProvider] = useState(initialProvider);
  const [localSources, setLocalSources] = useState([]);
  const [localLoading, setLocalLoading] = useState(false);
  const [importingLocalPath, setImportingLocalPath] = useState('');

  const [cloudStatus, setCloudStatus] = useState(null);
  const [clientId, setClientId] = useState('');
  const [clientSecret, setClientSecret] = useState('');
  const [authBusy, setAuthBusy] = useState(false);
  const [authError, setAuthError] = useState('');

  const [folderTrail, setFolderTrail] = useState([{ id: 'root', name: 'Root' }]);
  const [items, setItems] = useState([]);
  const [listLoading, setListLoading] = useState(false);
  const [listError, setListError] = useState('');
  const [selectedMap, setSelectedMap] = useState(() => new Map());
  const [importBusy, setImportBusy] = useState(false);
  const [notice, setNotice] = useState('');

  const wasImportingRef = useRef(false);

  useEffect(() => {
    if (open) {
      setProvider(initialProvider || 'gdrive');
      setNotice('');
      setAuthError('');
      setListError('');
    }
  }, [open, initialProvider]);

  useEffect(() => {
    if (!open) return;
    setClientId(localStorage.getItem(`koma_cloud_cid_${provider}`) || '');
    setClientSecret('');
    setFolderTrail([{ id: 'root', name: provider === 'gdrive' ? 'My Drive' : 'OneDrive' }]);
    setSelectedMap(new Map());
    setListError('');
    setAuthError('');
  }, [open, provider]);

  // Fetch local cloud sync folders
  useEffect(() => {
    if (!open) return;
    const controller = new AbortController();
    setLocalLoading(true);
    api
      .getCloudSources(controller.signal)
      .then((res) => {
        if (!controller.signal.aborted) {
          setLocalSources(Array.isArray(res?.sources) ? res.sources : []);
        }
      })
      .catch(() => {
        if (!controller.signal.aborted) setLocalSources([]);
      })
      .finally(() => {
        if (!controller.signal.aborted) setLocalLoading(false);
      });
    return () => controller.abort();
  }, [open]);

  // Fetch and poll /api/cloud/status
  useEffect(() => {
    if (!open) return;
    let active = true;
    const fetchStatus = () => {
      api
        .getCloudStatus()
        .then((st) => {
          if (!active) return;
          setCloudStatus(st);
          const running = Boolean(st?.import?.running);
          if (wasImportingRef.current && !running) {
            onImportCompleted?.();
            setNotice('Cloud import finished.');
          }
          wasImportingRef.current = running;
        })
        .catch(() => {});
    };
    fetchStatus();
    const timer = setInterval(fetchStatus, 1200);
    return () => {
      active = false;
      clearInterval(timer);
    };
  }, [open, onImportCompleted]);

  const currentFolder = folderTrail[folderTrail.length - 1] || { id: 'root', name: 'Root' };
  const isConnected = Boolean(cloudStatus?.[provider]?.connected);

  const loadFolder = useCallback(
    (folderId) => {
      if (!isConnected) return;
      const controller = new AbortController();
      setListLoading(true);
      setListError('');
      api
        .listCloudFolder(provider, folderId === 'root' ? '' : folderId, controller.signal)
        .then((res) => {
          if (controller.signal.aborted) return;
          setItems(Array.isArray(res?.items) ? res.items : []);
          setSelectedMap(new Map());
        })
        .catch((err) => {
          if (isAbortError(err) || controller.signal.aborted) return;
          setListError(err?.message || 'Failed to list cloud folder.');
          setItems([]);
        })
        .finally(() => {
          if (!controller.signal.aborted) setListLoading(false);
        });
      return () => controller.abort();
    },
    [provider, isConnected]
  );

  useEffect(() => {
    if (!open || !isConnected) {
      setItems([]);
      return;
    }
    return loadFolder(currentFolder.id);
  }, [open, isConnected, currentFolder.id, loadFolder]);

  if (!open) return null;

  const matchingLocalSources = localSources.filter((src) => {
    const text = `${src.provider || ''} ${src.name || ''} ${src.path || ''}`.toLowerCase();
    return provider === 'gdrive' ? text.includes('google') || text.includes('gdrive') : text.includes('onedrive');
  });

  const handleImportLocal = async (src) => {
    if (!onImportFolderPath || importingLocalPath) return;
    setImportingLocalPath(src.path);
    setNotice('');
    try {
      await onImportFolderPath(src.path);
      setLocalSources((prev) =>
        prev.map((item) => (item.path === src.path ? { ...item, already_added: true } : item))
      );
      setNotice(`Started importing local sync folder: ${src.name}`);
    } catch (err) {
      setNotice(err?.message || 'Failed to import local folder.');
    } finally {
      setImportingLocalPath('');
    }
  };

  const handleConnectAccount = async (e) => {
    e.preventDefault();
    const trimmedId = clientId.trim();
    if (!trimmedId || authBusy) return;
    localStorage.setItem(`koma_cloud_cid_${provider}`, trimmedId);
    setAuthBusy(true);
    setAuthError('');
    setNotice('');
    try {
      const res = await api.startCloudAuth(provider, trimmedId, clientSecret.trim());
      if (res?.auth_url) {
        window.open(res.auth_url, '_blank', 'noopener,noreferrer');
      }
      setNotice('Browser sign-in started. Complete authorization in your browser.');
    } catch (err) {
      setAuthError(err?.message || 'Failed to start OAuth sign-in.');
    } finally {
      setAuthBusy(false);
    }
  };

  const handleDisconnect = async () => {
    setAuthBusy(true);
    setAuthError('');
    try {
      await api.disconnectCloud(provider);
      setItems([]);
      setSelectedMap(new Map());
      setCloudStatus((prev) =>
        prev ? { ...prev, [provider]: { ...prev[provider], connected: false } } : prev
      );
    } finally {
      setAuthBusy(false);
    }
  };

  const toggleSelectItem = (item) => {
    setSelectedMap((prev) => {
      const next = new Map(prev);
      if (next.has(item.id)) next.delete(item.id);
      else next.set(item.id, item);
      return next;
    });
  };

  const handleImportCurrentFolder = async () => {
    if (importBusy) return;
    setImportBusy(true);
    setNotice('');
    setListError('');
    try {
      await api.importCloudItems({
        provider,
        folder_id: currentFolder.id || 'root',
        folder_name: currentFolder.name,
      });
      setNotice(`Queued folder "${currentFolder.name}" for import.`);
    } catch (err) {
      setListError(err?.message || 'Failed to start cloud folder import.');
    } finally {
      setImportBusy(false);
    }
  };

  const handleImportSelected = async () => {
    if (importBusy || selectedMap.size === 0) return;
    setImportBusy(true);
    setNotice('');
    setListError('');
    try {
      const selectedItems = Array.from(selectedMap.values()).map((it) => ({
        id: it.id,
        name: it.name,
        is_folder: Boolean(it.is_folder),
      }));
      await api.importCloudItems({ provider, items: selectedItems });
      setNotice(`Queued ${selectedItems.length} item(s) for import.`);
      setSelectedMap(new Map());
    } catch (err) {
      setListError(err?.message || 'Failed to start cloud import.');
    } finally {
      setImportBusy(false);
    }
  };

  const importState = cloudStatus?.import;

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/65 backdrop-blur-sm p-4">
      <div className="w-full max-w-2xl rounded-xl bg-white dark:bg-[#12151B] border border-[#DCE1E8] dark:border-[#222834] shadow-2xl overflow-hidden flex flex-col max-h-[88vh]">
        {/* Header */}
        <div className="px-6 py-4 border-b border-[#DCE1E8] dark:border-[#222834] flex items-center justify-between shrink-0">
          <div className="flex items-center gap-2.5">
            <Cloud className="w-5 h-5 text-cyan-600 dark:text-[#22D3EE]" />
            <div>
              <h2 className="text-base font-bold text-slate-900 dark:text-[#F1F5F9]">
                Cloud Import
              </h2>
              <p className="text-xs text-slate-500 dark:text-slate-400">
                Import comics from Google Drive or OneDrive via local sync or direct OAuth.
              </p>
            </div>
          </div>
          <button
            type="button"
            onClick={onClose}
            className="p-1.5 rounded-lg text-slate-400 hover:text-slate-700 dark:hover:text-[#F1F5F9] hover:bg-[#F4F6F9] dark:hover:bg-[#171C24]"
          >
            <X className="w-4 h-4" />
          </button>
        </div>

        {/* Provider Switcher */}
        <div className="px-6 pt-3 border-b border-[#DCE1E8] dark:border-[#222834] flex items-center gap-2 bg-[#F4F6F9]/40 dark:bg-[#0B0D10]/40 shrink-0">
          {PROVIDERS.map((p) => (
            <button
              key={p.id}
              type="button"
              onClick={() => setProvider(p.id)}
              className={`px-3.5 py-2 text-xs font-semibold border-b-2 transition-colors ${
                provider === p.id
                  ? 'border-cyan-600 dark:border-[#22D3EE] text-cyan-700 dark:text-[#22D3EE]'
                  : 'border-transparent text-slate-500 dark:text-slate-400 hover:text-slate-800 dark:hover:text-slate-200'
              }`}
            >
              {p.name}
            </button>
          ))}
        </div>

        {/* Body */}
        <div className="p-6 space-y-5 overflow-y-auto flex-1">
          {/* Option 1: Local Sync Folder (Instant) */}
          <div className="p-4 rounded-xl border border-[#DCE1E8] dark:border-[#222834] bg-[#F4F6F9]/50 dark:bg-[#0B0D10]/50 space-y-2.5">
            <div className="flex items-center justify-between">
              <div className="flex items-center gap-2">
                <HardDrive className="w-4 h-4 text-emerald-500 shrink-0" />
                <h3 className="text-xs font-bold uppercase tracking-wider text-slate-700 dark:text-slate-200">
                  Local Sync Folder (Instant)
                </h3>
              </div>
              <span className="text-[10px] font-mono text-slate-400">
                Windows Mounted Drive
              </span>
            </div>

            {localLoading ? (
              <div className="flex items-center gap-2 text-xs text-slate-400 py-1">
                <RefreshCw className="w-3.5 h-3.5 animate-spin" />
                <span>Scanning mounted cloud drives...</span>
              </div>
            ) : matchingLocalSources.length === 0 ? (
              <p className="text-xs text-slate-500 dark:text-slate-400">
                No local {provider === 'gdrive' ? 'Google Drive' : 'OneDrive'} sync folder detected on this PC. Use Direct Cloud Account below.
              </p>
            ) : (
              <div className="space-y-1.5">
                {matchingLocalSources.map((src) => (
                  <div
                    key={src.path}
                    className="flex items-center justify-between gap-3 px-3 py-2 rounded-lg bg-white dark:bg-[#12151B] border border-[#DCE1E8] dark:border-[#222834]"
                  >
                    <div className="min-w-0">
                      <div className="text-xs font-semibold text-slate-800 dark:text-[#F1F5F9] truncate">
                        {src.name}
                      </div>
                      <div className="text-[11px] font-mono text-slate-400 truncate" title={src.path}>
                        {src.path}
                      </div>
                    </div>
                    {src.already_added ? (
                      <span className="inline-flex items-center gap-1 px-2.5 py-1 rounded-md text-[11px] font-semibold bg-emerald-500/15 text-emerald-600 dark:text-emerald-400 shrink-0">
                        <Check className="w-3 h-3" />
                        <span>Added</span>
                      </span>
                    ) : (
                      <button
                        type="button"
                        disabled={Boolean(importingLocalPath)}
                        onClick={() => handleImportLocal(src)}
                        className="px-3 py-1.5 rounded-lg bg-cyan-600 dark:bg-[#22D3EE] text-white dark:text-[#0B0D10] text-xs font-semibold hover:opacity-90 disabled:opacity-50 shrink-0"
                      >
                        {importingLocalPath === src.path ? 'Importing...' : 'Import'}
                      </button>
                    )}
                  </div>
                ))}
              </div>
            )}
          </div>

          {/* Option 2: Direct Cloud Account (No Desktop App Required) */}
          <div className="p-4 rounded-xl border border-[#DCE1E8] dark:border-[#222834] bg-[#F4F6F9]/50 dark:bg-[#0B0D10]/50 space-y-3">
            <div className="flex items-center justify-between gap-2">
              <div className="flex items-center gap-2">
                <Cloud className="w-4 h-4 text-cyan-600 dark:text-[#22D3EE] shrink-0" />
                <h3 className="text-xs font-bold uppercase tracking-wider text-slate-700 dark:text-slate-200">
                  Direct Cloud Account (No Desktop App Required)
                </h3>
              </div>
              {isConnected && (
                <button
                  type="button"
                  disabled={authBusy}
                  onClick={handleDisconnect}
                  className="inline-flex items-center gap-1 px-2.5 py-1 rounded-md bg-rose-500/15 hover:bg-rose-500/25 text-rose-600 dark:text-rose-400 text-[11px] font-semibold"
                >
                  <LogOut className="w-3 h-3" />
                  <span>Disconnect</span>
                </button>
              )}
            </div>

            {!isConnected ? (
              <form onSubmit={handleConnectAccount} className="space-y-2.5">
                <div className="grid grid-cols-1 sm:grid-cols-2 gap-2.5">
                  <div>
                    <label className="block text-[11px] font-semibold text-slate-600 dark:text-slate-300 mb-1">
                      OAuth Client ID
                    </label>
                    <input
                      type="text"
                      value={clientId}
                      onChange={(e) => setClientId(e.target.value)}
                      placeholder={
                        provider === 'gdrive'
                          ? 'xxxx.apps.googleusercontent.com'
                          : '00000000-0000-0000-0000-000000000000'
                      }
                      className="w-full px-3 py-1.5 rounded-lg bg-white dark:bg-[#12151B] border border-[#DCE1E8] dark:border-[#222834] text-xs text-slate-900 dark:text-[#F1F5F9] focus:outline-none focus:border-cyan-600 dark:focus:border-[#22D3EE]"
                    />
                  </div>
                  <div>
                    <label className="block text-[11px] font-semibold text-slate-600 dark:text-slate-300 mb-1">
                      Client Secret (optional)
                    </label>
                    <input
                      type="password"
                      value={clientSecret}
                      onChange={(e) => setClientSecret(e.target.value)}
                      placeholder="Optional"
                      className="w-full px-3 py-1.5 rounded-lg bg-white dark:bg-[#12151B] border border-[#DCE1E8] dark:border-[#222834] text-xs text-slate-900 dark:text-[#F1F5F9] focus:outline-none focus:border-cyan-600 dark:focus:border-[#22D3EE]"
                    />
                  </div>
                </div>

                <div className="flex items-center justify-between gap-2 pt-1">
                  <span className="text-[11px] text-slate-500 dark:text-slate-400">
                    {cloudStatus?.auth_in_progress && cloudStatus?.auth_provider === provider
                      ? 'Waiting for browser OAuth callback...'
                      : 'Authorize read-only access in your browser.'}
                  </span>
                  <button
                    type="submit"
                    disabled={authBusy || !clientId.trim()}
                    className="inline-flex items-center gap-1.5 px-3.5 py-1.5 rounded-lg bg-cyan-600 dark:bg-[#22D3EE] hover:opacity-90 disabled:opacity-50 text-white dark:text-[#0B0D10] text-xs font-semibold shrink-0"
                  >
                    <ExternalLink className="w-3.5 h-3.5" />
                    <span>Connect Account</span>
                  </button>
                </div>

                {(authError || (cloudStatus?.auth_error && cloudStatus?.auth_provider === provider)) && (
                  <div className="px-3 py-2 rounded-lg bg-rose-500/10 border border-rose-500/30 text-xs text-rose-500">
                    {authError || cloudStatus.auth_error}
                  </div>
                )}
              </form>
            ) : (
              <div className="space-y-3">
                {/* Cloud Folder Breadcrumbs & Actions */}
                <div className="flex flex-wrap items-center justify-between gap-2">
                  <nav className="flex items-center gap-1 text-xs font-mono overflow-x-auto">
                    {folderTrail.map((seg, idx) => (
                      <React.Fragment key={seg.id}>
                        {idx > 0 && <ChevronRight className="w-3.5 h-3.5 text-slate-400 shrink-0" />}
                        <button
                          type="button"
                          onClick={() => setFolderTrail((prev) => prev.slice(0, idx + 1))}
                          className={`px-2 py-0.5 rounded transition-colors shrink-0 ${
                            idx === folderTrail.length - 1
                              ? 'bg-cyan-500/15 text-cyan-700 dark:text-[#22D3EE] font-semibold'
                              : 'text-slate-600 dark:text-slate-300 hover:bg-slate-200 dark:hover:bg-[#171C24]'
                          }`}
                        >
                          {seg.name}
                        </button>
                      </React.Fragment>
                    ))}
                  </nav>

                  <div className="flex items-center gap-2 shrink-0">
                    <button
                      type="button"
                      disabled={importBusy}
                      onClick={handleImportCurrentFolder}
                      className="inline-flex items-center gap-1.5 px-2.5 py-1.5 rounded-lg bg-white dark:bg-[#12151B] border border-[#DCE1E8] dark:border-[#222834] hover:border-cyan-600 dark:hover:border-[#22D3EE] text-slate-700 dark:text-[#F1F5F9] text-xs font-semibold disabled:opacity-50"
                    >
                      <Download className="w-3.5 h-3.5 text-cyan-600 dark:text-[#22D3EE]" />
                      <span>Import Folder</span>
                    </button>
                    <button
                      type="button"
                      disabled={importBusy || selectedMap.size === 0}
                      onClick={handleImportSelected}
                      className="inline-flex items-center gap-1.5 px-2.5 py-1.5 rounded-lg bg-cyan-600 dark:bg-[#22D3EE] hover:opacity-90 disabled:opacity-50 text-white dark:text-[#0B0D10] text-xs font-semibold"
                    >
                      <Download className="w-3.5 h-3.5" />
                      <span>
                        Import Selected Files{selectedMap.size > 0 ? ` (${selectedMap.size})` : ''}
                      </span>
                    </button>
                  </div>
                </div>

                {/* Cloud Folder Contents */}
                <div className="rounded-lg border border-[#DCE1E8] dark:border-[#222834] bg-white dark:bg-[#12151B] max-h-56 overflow-y-auto divide-y divide-[#DCE1E8] dark:divide-[#222834]">
                  {listLoading ? (
                    <div className="p-4 flex items-center justify-center gap-2 text-xs text-slate-400">
                      <RefreshCw className="w-3.5 h-3.5 animate-spin" />
                      <span>Loading cloud folder...</span>
                    </div>
                  ) : listError ? (
                    <div className="p-3 text-xs text-rose-500">{listError}</div>
                  ) : items.length === 0 ? (
                    <div className="p-4 text-center text-xs text-slate-400">
                      No folders or supported comic files in this directory.
                    </div>
                  ) : (
                    items.map((item) => {
                      const checked = selectedMap.has(item.id);
                      return (
                        <div
                          key={item.id}
                          className="px-3 py-2 flex items-center justify-between gap-2 hover:bg-[#F4F6F9] dark:hover:bg-[#171C24]"
                        >
                          <div className="flex items-center gap-2.5 min-w-0 flex-1">
                            <input
                              type="checkbox"
                              checked={checked}
                              onChange={() => toggleSelectItem(item)}
                              className="rounded border-slate-400 text-cyan-600 focus:ring-cyan-500 shrink-0"
                            />
                            {item.is_folder ? (
                              <button
                                type="button"
                                onClick={() =>
                                  setFolderTrail((prev) => [...prev, { id: item.id, name: item.name }])
                                }
                                className="flex items-center gap-2 min-w-0 text-left text-xs font-semibold text-slate-800 dark:text-[#F1F5F9] hover:text-cyan-600 dark:hover:text-[#22D3EE]"
                              >
                                <Folder className="w-4 h-4 text-cyan-600 dark:text-[#22D3EE] shrink-0" />
                                <span className="truncate">{item.name}</span>
                              </button>
                            ) : (
                              <div className="flex items-center gap-2 min-w-0 text-xs text-slate-700 dark:text-slate-200">
                                <FileText className="w-4 h-4 text-slate-400 shrink-0" />
                                <span className="truncate">{item.name}</span>
                              </div>
                            )}
                          </div>
                          {!item.is_folder && item.size > 0 && (
                            <span className="text-[11px] font-mono text-slate-400 tabular-nums shrink-0">
                              {formatBytes(item.size)}
                            </span>
                          )}
                        </div>
                      );
                    })
                  )}
                </div>

                {importState?.running && (
                  <div className="px-3 py-2 rounded-lg bg-cyan-500/10 border border-cyan-500/30 text-xs text-cyan-700 dark:text-[#22D3EE] flex items-center justify-between gap-2">
                    <span className="truncate">
                      Importing: {importState.current_file || '...'}
                    </span>
                    <span className="font-mono tabular-nums shrink-0">
                      {importState.completed ?? 0} / {importState.total ?? 0}
                    </span>
                  </div>
                )}
              </div>
            )}
          </div>

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
            Close
          </button>
        </div>
      </div>
    </div>
  );
}
