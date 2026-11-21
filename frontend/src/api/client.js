const JSON_HEADERS = { 'Content-Type': 'application/json' };

export class ApiError extends Error {
  constructor(message, status) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
  }
}

async function request(url, options = {}) {
  const res = await fetch(url, options);
  if (!res.ok) {
    let message = `HTTP ${res.status}`;
    try {
      const data = await res.json();
      if (data?.error) message = data.error;
    } catch {
      // ignore non-JSON error bodies
    }
    throw new ApiError(message, res.status);
  }
  return res.json();
}

export function isAbortError(err) {
  return err?.name === 'AbortError';
}

export const api = {
  // Health & Settings
  getHealth: (signal) => request('/api/health', { signal }),
  getSettings: (signal) => request('/api/settings', { signal }),
  updateSettings: (patch, signal) =>
    request('/api/settings', {
      method: 'POST',
      headers: JSON_HEADERS,
      body: JSON.stringify(patch),
      signal,
    }),
  getStartupFile: (signal) => request('/api/startup-file', { signal }),
  clearStartupFile: () => request('/api/startup-file', { method: 'DELETE' }),

  // Reader Session
  openReader: (payload, signal) =>
    request('/api/reader/open', {
      method: 'POST',
      headers: JSON_HEADERS,
      body: JSON.stringify(payload),
      signal,
    }),
  closeReader: () => request('/api/reader/close', { method: 'POST' }),
  getReaderPageUrl: (pageIndex, autoCrop = false, maxWidth = 1800) =>
    `/api/reader/page/${pageIndex}?auto_crop=${autoCrop ? '1' : '0'}&max_width=${maxWidth}`,

  // Bookmarks
  getBookmarks: (comicId, signal) => request(`/api/comics/${comicId}/bookmarks`, { signal }),
  getAdjacentComics: (comicId, shelfId = 0, signal) => {
    const qs = shelfId > 0 ? `?shelf_id=${shelfId}` : '';
    return request(`/api/comics/${comicId}/adjacent${qs}`, { signal });
  },
  registerFileTypes: (includePdf = false) =>
    request('/api/system/register-file-types', {
      method: 'POST',
      headers: JSON_HEADERS,
      body: JSON.stringify({ include_pdf: includePdf }),
    }),
  setKeepAwake: (active) =>
    request('/api/system/keep-awake', {
      method: 'POST',
      headers: JSON_HEADERS,
      body: JSON.stringify({ active: Boolean(active) }),
    }),
  setFullscreen: (active) =>
    request('/api/system/fullscreen', {
      method: 'POST',
      headers: JSON_HEADERS,
      body: JSON.stringify({ active: Boolean(active) }),
    }),
  addBookmark: (comicId, pageNumber, name) =>
    request(`/api/comics/${comicId}/bookmarks`, {
      method: 'POST',
      headers: JSON_HEADERS,
      body: JSON.stringify({ page_number: pageNumber, name }),
    }),
  deleteBookmark: (comicId, bookmarkId) =>
    request(`/api/comics/${comicId}/bookmarks/${bookmarkId}`, { method: 'DELETE' }),

  // Library & Search
  queryLibrary: (
    {
      search = '',
      smartview = '',
      smartviewId = 0,
      shelfId = 0,
      folderPrefix = '',
      author = '',
      tag = '',
      sort = 'title_asc',
      includeHidden = false,
      offset = 0,
      limit = 150,
    },
    signal
  ) => {
    const params = new URLSearchParams();
    if (search) params.set('search', search);
    if (smartview) params.set('smartview', smartview);
    if (smartviewId > 0) params.set('smartview_id', String(smartviewId));
    if (shelfId > 0) params.set('shelf_id', String(shelfId));
    if (folderPrefix) params.set('folder_prefix', folderPrefix);
    if (author) params.set('author', author);
    if (tag) params.set('tag', tag);
    if (sort) params.set('sort', sort);
    if (includeHidden) params.set('include_hidden', '1');
    params.set('offset', String(offset));
    params.set('limit', String(limit));
    return request(`/api/library?${params.toString()}`, { signal });
  },
  getContinueReading: (limit = 10, includeHidden = false, signal) => {
    const params = new URLSearchParams({ limit: String(limit) });
    if (includeHidden) params.set('include_hidden', '1');
    return request(`/api/library/continue-reading?${params.toString()}`, { signal });
  },
  getRandomComic: (
    {
      shelfId = 0,
      smartview = '',
      smartviewId = 0,
      author = '',
      tag = '',
      includeHidden = false,
    } = {},
    signal
  ) => {
    const params = new URLSearchParams();
    if (shelfId > 0) params.set('shelf_id', String(shelfId));
    if (smartview) params.set('smartview', smartview);
    if (smartviewId > 0) params.set('smartview_id', String(smartviewId));
    if (author) params.set('author', author);
    if (tag) params.set('tag', tag);
    if (includeHidden) params.set('include_hidden', '1');
    const qs = params.toString();
    return request(`/api/library/random${qs ? `?${qs}` : ''}`, { signal });
  },
  getLibraryFolders: (parentPath = '', signal, includeHidden = false) => {
    const params = new URLSearchParams();
    if (parentPath) params.set('parent_path', parentPath);
    if (includeHidden) params.set('include_hidden', '1');
    const qs = params.toString();
    return request(`/api/library/folders${qs ? `?${qs}` : ''}`, { signal });
  },

  // Covers (with AbortController cancellation support for fast-scroll)
  getCoverUrl: (comicId, coverVersion = 0) =>
    `/api/covers/${comicId}${coverVersion ? `?v=${coverVersion}` : ''}`,

  fetchCoverBlobUrl: async (comicId, coverVersion = 0, signal) => {
    const url = `/api/covers/${comicId}${coverVersion ? `?v=${coverVersion}` : ''}`;
    const res = await fetch(url, { signal });
    if (res.status === 204 || !res.ok) {
      throw new ApiError('Cover unavailable', res.status);
    }
    const blob = await res.blob();
    return URL.createObjectURL(blob);
  },

  cancelCovers: (ids, signal) => {
    if (!ids || ids.length === 0) return Promise.resolve({ cancelled: 0 });
    return request('/api/covers/cancel', {
      method: 'POST',
      headers: JSON_HEADERS,
      body: JSON.stringify({ ids }),
      signal,
    }).catch(() => ({ cancelled: 0 }));
  },
  setCoversViewport: (visibleIds, signal) =>
    request('/api/covers/viewport', {
      method: 'POST',
      headers: JSON_HEADERS,
      body: JSON.stringify({ visible_ids: visibleIds || [] }),
      signal,
    }).catch(() => ({ active_count: 0 })),
  buildCoverPack: () => request('/api/covers/pack/build', { method: 'POST' }),
  rebuildCoverPack: () => request('/api/covers/pack/rebuild', { method: 'POST' }),
  getCoverPackStatus: (signal) => request('/api/covers/pack/status', { signal }),
  purgeCoverPack: () => request('/api/covers/pack/purge', { method: 'POST' }),

  // Watched Folders, Native Explorer Dialogs & Importer
  pickFolder: () => request('/api/dialog/pick-folder', { method: 'POST' }),
  pickFiles: () => request('/api/dialog/pick-files', { method: 'POST' }),
  getWatchedFolders: (signal) => request('/api/library/watched-folders', { signal }),
  importFolder: (path) =>
    request('/api/library/import-folder', {
      method: 'POST',
      headers: JSON_HEADERS,
      body: JSON.stringify({ path }),
    }),
  deleteFolder: (id) => request(`/api/library/folders/${id}`, { method: 'DELETE' }),
  getScanStatus: (signal) => request('/api/library/scan-status', { signal }),
  getCloudSources: (signal) => request('/api/storage/cloud-sources', { signal }),
  startCloudAuth: (provider, clientId, clientSecret = '') =>
    request('/api/cloud/auth/start', {
      method: 'POST',
      headers: JSON_HEADERS,
      body: JSON.stringify({
        provider,
        client_id: clientId,
        client_secret: clientSecret,
      }),
    }),
  getCloudStatus: (signal) => request('/api/cloud/status', { signal }),
  disconnectCloud: (provider) =>
    request('/api/cloud/disconnect', {
      method: 'POST',
      headers: JSON_HEADERS,
      body: JSON.stringify({ provider }),
    }),
  listCloudFolder: (provider, folderId = '', signal) => {
    const params = new URLSearchParams({ provider });
    if (folderId) params.set('folder_id', folderId);
    return request(`/api/cloud/list?${params.toString()}`, { signal });
  },
  importCloudItems: (payload) =>
    request('/api/cloud/import', {
      method: 'POST',
      headers: JSON_HEADERS,
      body: JSON.stringify(payload),
    }),
  getOfflineStats: (signal) => request('/api/storage/offline-stats', { signal }),
  clearUnpinnedCache: () => request('/api/storage/clear-unpinned-cache', { method: 'POST' }),
  pinOfflineBatch: (payload) =>
    request('/api/library/pin-offline-batch', {
      method: 'POST',
      headers: JSON_HEADERS,
      body: JSON.stringify(payload),
    }),

  // SmartViews
  getSmartviews: (signal) => request('/api/smartviews', { signal }),
  createSmartview: (payload) =>
    request('/api/smartviews', {
      method: 'POST',
      headers: JSON_HEADERS,
      body: JSON.stringify(payload),
    }),
  deleteSmartview: (id) => request(`/api/smartviews/${id}`, { method: 'DELETE' }),

  // Shelves
  getShelves: (signal) => request('/api/shelves', { signal }),
  createShelf: (name) =>
    request('/api/shelves', {
      method: 'POST',
      headers: JSON_HEADERS,
      body: JSON.stringify({ name }),
    }),
  renameShelf: (id, name) =>
    request(`/api/shelves/${id}`, {
      method: 'PUT',
      headers: JSON_HEADERS,
      body: JSON.stringify({ name }),
    }),
  deleteShelf: (id) => request(`/api/shelves/${id}`, { method: 'DELETE' }),
  addComicToShelf: (shelfId, comicId) =>
    request(`/api/shelves/${shelfId}/comics`, {
      method: 'POST',
      headers: JSON_HEADERS,
      body: JSON.stringify({ comic_id: comicId }),
    }),
  removeComicFromShelf: (shelfId, comicId) =>
    request(`/api/shelves/${shelfId}/comics/${comicId}`, { method: 'DELETE' }),

  // Comic Actions
  updateProgress: (comicId, patch) =>
    request(`/api/comics/${comicId}/progress`, {
      method: 'POST',
      headers: JSON_HEADERS,
      body: JSON.stringify(patch),
    }),
  setComicHidden: (comicId, hidden) =>
    request(`/api/comics/${comicId}/hidden`, {
      method: 'POST',
      headers: JSON_HEADERS,
      body: JSON.stringify({ hidden: Boolean(hidden) }),
    }),
  pinOffline: (comicId, pinned) =>
    request(`/api/comics/${comicId}/pin-offline`, {
      method: pinned ? 'POST' : 'DELETE',
      headers: JSON_HEADERS,
      body: pinned ? JSON.stringify({ pinned: true }) : undefined,
    }),
  batchAction: (payload) =>
    request('/api/comics/batch', {
      method: 'POST',
      headers: JSON_HEADERS,
      body: JSON.stringify(payload),
    }),
};
