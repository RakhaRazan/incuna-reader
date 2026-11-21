import React from 'react';
import { useTranslation } from 'react-i18next';
import { Search, X, Sun, Moon, Settings, SlidersHorizontal, ArrowUpDown, Dices } from 'lucide-react';

export default function TopBar({
  search,
  onSearchChange,
  sort,
  onSortChange,
  cardMinWidth,
  onCardMinWidthChange,
  theme,
  onToggleTheme,
  language,
  onLanguageChange,
  onOpenSettings,
  visibleCount,
  showRandomButton = true,
  randomBusy = false,
  onRandomComic,
}) {
  const { t } = useTranslation();

  return (
    <header className="h-14 px-4 bg-white dark:bg-[#12151B] border-b border-[#DCE1E8] dark:border-[#222834] flex items-center justify-between gap-3 shrink-0">
      {/* FTS5 Search Input */}
      <div className="relative flex-1 max-w-md">
        <Search className="w-4 h-4 text-slate-400 dark:text-slate-500 absolute left-3 top-1/2 -translate-y-1/2 pointer-events-none" />
        <input
          type="text"
          value={search}
          onChange={(e) => onSearchChange(e.target.value)}
          placeholder={t('topbar.searchPlaceholder')}
          className="w-full pl-9 pr-8 py-1.5 text-xs rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] focus:border-cyan-600 dark:focus:border-[#22D3EE] text-slate-900 dark:text-[#F1F5F9] placeholder:text-slate-400 dark:placeholder:text-slate-500 focus:outline-none transition-colors"
        />
        {search && (
          <button
            type="button"
            title={t('topbar.clearSearch')}
            onClick={() => onSearchChange('')}
            className="absolute right-2.5 top-1/2 -translate-y-1/2 text-slate-400 hover:text-slate-700 dark:hover:text-[#F1F5F9]"
          >
            <X className="w-3.5 h-3.5" />
          </button>
        )}
      </div>

      {/* Right Controls */}
      <div className="flex items-center gap-2">
        <span className="hidden xl:inline-block text-[11px] font-mono tabular-nums font-medium text-slate-500 dark:text-slate-400 mr-1">
          {t('app.itemsCount', { count: visibleCount.toLocaleString() })}
        </span>

        {/* Sort Selector */}
        <div className="flex items-center gap-1.5 px-2.5 py-1.5 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] text-xs">
          <ArrowUpDown className="w-3.5 h-3.5 text-cyan-600 dark:text-[#22D3EE] shrink-0" />
          <select
            aria-label={t('topbar.sortBy')}
            value={sort}
            onChange={(e) => onSortChange(e.target.value)}
            className="bg-transparent text-slate-700 dark:text-[#F1F5F9] font-medium focus:outline-none cursor-pointer"
          >
            <option value="title_asc" className="dark:bg-[#12151B]">
              {t('topbar.sortTitleAsc')}
            </option>
            <option value="title_desc" className="dark:bg-[#12151B]">
              {t('topbar.sortTitleDesc')}
            </option>
            <option value="added_desc" className="dark:bg-[#12151B]">
              {t('topbar.sortAddedDesc')}
            </option>
            <option value="last_read_desc" className="dark:bg-[#12151B]">
              {t('topbar.sortLastReadDesc')}
            </option>
            <option value="size_desc" className="dark:bg-[#12151B]">
              {t('topbar.sortSizeDesc')}
            </option>
          </select>
        </div>

        {/* Grid Density Slider */}
        <div
          title={t('topbar.density')}
          className="hidden md:flex items-center gap-2 px-2.5 py-1.5 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834]"
        >
          <SlidersHorizontal className="w-3.5 h-3.5 text-slate-400" />
          <input
            type="range"
            min={130}
            max={250}
            step={10}
            value={cardMinWidth}
            onChange={(e) => onCardMinWidthChange(Number(e.target.value))}
            className="w-20 accent-[#22D3EE] cursor-pointer"
          />
        </div>

        {/* Language Switcher */}
        <div className="flex items-center rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] p-0.5 text-[11px] font-mono font-semibold">
          {['en', 'id', 'ja'].map((lng) => (
            <button
              key={lng}
              type="button"
              onClick={() => onLanguageChange(lng)}
              className={`px-2 py-1 rounded-md uppercase transition-colors ${
                language === lng
                  ? 'bg-white dark:bg-[#171C24] text-cyan-700 dark:text-[#22D3EE] border border-[#DCE1E8] dark:border-[#222834]'
                  : 'text-slate-500 dark:text-slate-400 hover:text-slate-800 dark:hover:text-[#F1F5F9] border border-transparent'
              }`}
            >
              {lng}
            </button>
          ))}
        </div>

        {/* Theme Toggle */}
        <button
          type="button"
          onClick={onToggleTheme}
          title={theme === 'dark' ? t('topbar.themeDark') : t('topbar.themeLight')}
          className="p-2 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] text-slate-600 dark:text-slate-300 hover:border-cyan-600 dark:hover:border-[#22D3EE] transition-colors"
        >
          {theme === 'dark' ? (
            <Sun className="w-4 h-4 text-[#F59E0B]" />
          ) : (
            <Moon className="w-4 h-4 text-slate-700" />
          )}
        </button>

        {/* Random Unread Comic Button */}
        {showRandomButton && (
          <button
            type="button"
            disabled={randomBusy}
            onClick={onRandomComic}
            title="Open random unread comic"
            className="inline-flex items-center gap-1.5 px-2.5 py-2 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] text-slate-700 dark:text-[#F1F5F9] hover:border-cyan-600 dark:hover:border-[#22D3EE] hover:text-cyan-700 dark:hover:text-[#22D3EE] disabled:opacity-50 text-xs font-semibold transition-colors"
          >
            <Dices className={`w-4 h-4 text-cyan-600 dark:text-[#22D3EE] ${randomBusy ? 'animate-spin' : ''}`} />
            <span className="hidden lg:inline">Random</span>
          </button>
        )}

        {/* Settings Button */}
        <button
          type="button"
          onClick={onOpenSettings}
          title={t('topbar.settings')}
          className="p-2 rounded-lg bg-[#F4F6F9] dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] text-slate-600 dark:text-slate-300 hover:border-cyan-600 dark:hover:border-[#22D3EE] transition-colors"
        >
          <Settings className="w-4 h-4" />
        </button>
      </div>
    </header>
  );
}
