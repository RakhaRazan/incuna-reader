import React, { useState } from 'react';
import { useTranslation } from 'react-i18next';
import { api } from './api/client';
import { useLibraryState } from './hooks/useLibraryState';
import { useLibraryShortcuts } from './hooks/useKeyboardShortcuts';
import SidebarNav from './components/SidebarNav';
import TopBar from './components/TopBar';
import LibraryView from './components/LibraryView';
import SettingsModal from './components/SettingsModal';
import AboutModal from './components/AboutModal';
import SmartViewModal from './components/SmartViewModal';
import CloudImportModal from './components/CloudImportModal';
import ContextMenu from './components/ContextMenu';
import ReaderView from './components/ReaderView';

export default function App() {
  const { i18n } = useTranslation();
  const lib = useLibraryState(i18n);

  const [settingsOpen, setSettingsOpen] = useState(false);
  const [aboutOpen, setAboutOpen] = useState(false);
  const [smartviewOpen, setSmartviewOpen] = useState(false);
  const [cloudImportProvider, setCloudImportProvider] = useState(null);
  const [contextMenu, setContextMenu] = useState(null);
  const [randomBusy, setRandomBusy] = useState(false);

  useLibraryShortcuts({
    enabled: !lib.activeComic,
    shortcuts: lib.settings?.shortcuts,
    onFocusSearch: () => {
      document.querySelector('input[type="text"]')?.focus();
    },
    onEscape: () => {
      if (contextMenu) setContextMenu(null);
      else if (settingsOpen) setSettingsOpen(false);
      else if (aboutOpen) setAboutOpen(false);
      else if (smartviewOpen) setSmartviewOpen(false);
      else if (cloudImportProvider) setCloudImportProvider(null);
    },
  });

  const activeSmartview = lib.activeFilter.type === 'smartview' ? lib.activeFilter.slug : '';
  const activeShelfId = lib.activeFilter.type === 'shelf' ? lib.activeFilter.shelfId : 0;

  const handleRandomComic = async () => {
    if (randomBusy) return;
    setRandomBusy(true);
    try {
      const res = await api.getRandomComic({
        shelfId: activeShelfId,
        smartview: activeSmartview,
        author: lib.authorFilter,
        tag: lib.tagFilter,
        includeHidden: Boolean(lib.settings?.show_hidden_elements),
      });
      if (res?.comic) {
        lib.setActiveComic(res.comic);
      }
    } catch {
      // No unread comics matched
    } finally {
      setRandomBusy(false);
    }
  };

  return (
    <div className="h-screen w-screen flex overflow-hidden bg-[#F4F6F9] dark:bg-[#0B0D10] text-slate-900 dark:text-[#F1F5F9]">
      <SidebarNav
        allComicsCount={lib.allComicsCount}
        activeFilter={lib.activeFilter}
        onSelectFilter={lib.setActiveFilter}
        smartviews={lib.smartviews}
        onOpenNewSmartview={() => setSmartviewOpen(true)}
        onDeleteSmartview={lib.handleDeleteSmartview}
        shelves={lib.shelves}
        smartRename={Boolean(lib.settings?.smart_rename)}
        onCreateShelf={lib.handleCreateShelf}
        onRenameShelf={lib.handleRenameShelf}
        onDeleteShelf={lib.handleDeleteShelf}
        folders={lib.folders}
        scanStatus={lib.scanStatus}
        onPickFolder={lib.handlePickFolder}
        onImportFolderPath={lib.handleImportFolderPath}
        onPickFiles={lib.handlePickFiles}
        onDeleteFolder={lib.handleDeleteFolder}
        onOpenCloudImport={(prov) => setCloudImportProvider(prov)}
        onOpenAbout={() => setAboutOpen(true)}
      />

      <div className="flex-1 flex flex-col min-w-0 h-full overflow-hidden">
        <TopBar
          search={lib.searchInput}
          onSearchChange={lib.setSearchInput}
          sort={lib.sort}
          onSortChange={lib.setSort}
          cardMinWidth={lib.cardMinWidth}
          onCardMinWidthChange={lib.setCardMinWidth}
          theme={lib.theme}
          onToggleTheme={lib.handleToggleTheme}
          language={i18n.language}
          onLanguageChange={lib.handleLanguageChange}
          onOpenSettings={() => setSettingsOpen(true)}
          visibleCount={lib.visibleCount}
          showRandomButton={lib.settings?.enable_random_button ?? true}
          randomBusy={randomBusy}
          onRandomComic={handleRandomComic}
        />

        <LibraryView
          search={lib.debouncedSearch}
          smartview={activeSmartview}
          shelfId={activeShelfId}
          authorFilter={lib.authorFilter}
          tagFilter={lib.tagFilter}
          sort={lib.sort}
          cardMinWidth={lib.cardMinWidth}
          refreshToken={lib.refreshToken}
          coverVersion={lib.coverVersion}
          shelves={lib.shelves}
          settings={lib.settings}
          onTotalChange={(total, isFolderFiltered) => {
            lib.setVisibleCount(total);
            if (
              !lib.debouncedSearch &&
              !activeSmartview &&
              !activeShelfId &&
              !lib.authorFilter &&
              !lib.tagFilter &&
              !isFolderFiltered
            ) {
              lib.setAllComicsCount(total);
            }
          }}
          onFilterAuthor={(author) => lib.setAuthorFilter(author)}
          onFilterTag={(tag) => lib.setTagFilter(tag)}
          onClearAuthor={() => lib.setAuthorFilter('')}
          onClearTag={() => lib.setTagFilter('')}
          onOpenComic={(comic) => lib.setActiveComic(comic)}
          onContextMenu={(e, comic) =>
            setContextMenu({ comic, x: e.clientX, y: e.clientY })
          }
          onComicMutated={() => {
            lib.triggerRefresh();
          }}
          onRefreshShelves={async () => lib.setShelves(await api.getShelves())}
          onPickFolder={lib.handlePickFolder}
          onPickFiles={lib.handlePickFiles}
        />

        {(lib.coverPackStatus?.building || lib.scanStatus?.scanning) && (
          <footer className="h-7 px-4 border-t border-[#DCE1E8] dark:border-[#222834] bg-white/95 dark:bg-[#12151B]/95 flex items-center justify-between text-[11px] font-mono text-slate-500 dark:text-slate-400 shrink-0">
            <span className="truncate">
              {lib.scanStatus?.scanning
                ? `Scanning folder: ${lib.scanStatus.files_indexed ?? 0} indexed`
                : 'covers.pack'}
            </span>
            {lib.coverPackStatus?.building && (
              <div className="flex items-center gap-2 px-2.5 py-0.5 rounded-full bg-cyan-500/10 border border-cyan-600/30 dark:border-[#22D3EE]/30 text-cyan-700 dark:text-[#22D3EE]">
                <span className="w-1.5 h-1.5 rounded-full bg-cyan-600 dark:bg-[#22D3EE] animate-pulse" />
                <span className="tabular-nums">
                  Indexing covers: {lib.coverPackStatus.processed ?? 0} / {lib.coverPackStatus.total ?? 0}
                  {lib.coverPackStatus.total > 0
                    ? ` (${Math.min(100, Math.round((lib.coverPackStatus.processed / lib.coverPackStatus.total) * 100))}%)`
                    : ''}
                </span>
              </div>
            )}
          </footer>
        )}
      </div>

      <SettingsModal
        open={settingsOpen}
        onClose={() => setSettingsOpen(false)}
        settings={lib.settings}
        health={lib.health}
        onSettingsUpdated={lib.setSettings}
        onCoversChanged={lib.triggerCoverRefresh}
      />

      <AboutModal
        open={aboutOpen}
        onClose={() => setAboutOpen(false)}
        health={lib.health}
      />

      <SmartViewModal
        open={smartviewOpen}
        onClose={() => setSmartviewOpen(false)}
        onCreated={async () => lib.setSmartviews(await api.getSmartviews())}
      />

      <CloudImportModal
        open={Boolean(cloudImportProvider)}
        initialProvider={cloudImportProvider || 'gdrive'}
        onClose={() => setCloudImportProvider(null)}
        onImportFolderPath={lib.handleImportFolderPath}
        onImportCompleted={() => {
          lib.triggerRefresh();
        }}
      />

      <ContextMenu
        menu={contextMenu}
        shelves={lib.shelves}
        activeShelfId={activeShelfId}
        enableMetadataExtractor={lib.settings?.enable_metadata_extractor ?? true}
        onClose={() => setContextMenu(null)}
        onOpenComic={(comic) => lib.setActiveComic(comic)}
        onToggleFavorite={lib.handleToggleFavorite}
        onSetStatus={lib.handleSetStatus}
        onToggleOfflinePin={lib.handleToggleOfflinePin}
        onToggleHidden={lib.handleToggleHidden}
        onAssignShelf={lib.handleAssignShelf}
        onRemoveFromShelf={lib.handleRemoveFromShelf}
        onFilterAuthor={(author) => lib.setAuthorFilter(author)}
        onFilterTag={(tag) => lib.setTagFilter(tag)}
      />

      {lib.activeComic && (
        <ReaderView
          comic={lib.activeComic}
          activeShelfId={activeShelfId}
          settings={lib.settings}
          onOpenComic={(nextComic) => lib.setActiveComic(nextComic)}
          onClose={() => {
            lib.setActiveComic(null);
            lib.triggerRefresh();
          }}
          onProgressSaved={() => {}}
        />
      )}
    </div>
  );
}
