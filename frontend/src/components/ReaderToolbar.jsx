import React from 'react';
import { useTranslation } from 'react-i18next';
import {
  ArrowLeft,
  Columns2,
  FileText,
  AlignVerticalJustifyStart,
  Maximize2,
  Minimize2,
  Bookmark,
  Moon,
  Crop,
  ZoomIn,
  ZoomOut,
  RotateCcw,
  BookOpen,
  Sparkles,
  Play,
  Pause,
  Minus,
  Plus,
} from 'lucide-react';

const BG_OPTIONS = [
  { id: 'black', value: '#000000', labelKey: 'reader.bgBlack', swatch: 'bg-black border-slate-600' },
  { id: 'darkgray', value: '#1a1d24', labelKey: 'reader.bgDarkGray', swatch: 'bg-[#1a1d24] border-slate-600' },
  { id: 'sepia', value: '#f4ecd8', labelKey: 'reader.bgSepia', swatch: 'bg-[#f4ecd8] border-amber-300' },
  { id: 'white', value: '#ffffff', labelKey: 'reader.bgWhite', swatch: 'bg-white border-slate-300' },
];

export default function ReaderToolbar({
  title,
  layout,
  coverOffset,
  direction,
  fit,
  zoom,
  bgColor,
  autoCrop,
  mangaEnhancer,
  nightMode,
  autoScrollEnabled,
  autoScrollPlaying,
  autoScrollSpeed,
  bookmarksOpen,
  bookmarksCount,
  isFullscreen,
  onClose,
  onLayoutChange,
  onToggleCoverOffset,
  onToggleDirection,
  onFitChange,
  onZoomIn,
  onZoomOut,
  onResetZoom,
  onBgColorChange,
  onToggleAutoCrop,
  onToggleMangaEnhancer,
  onToggleNightMode,
  onToggleAutoScroll,
  onAutoScrollSpeedChange,
  onToggleBookmarks,
  onToggleFullscreen,
}) {
  const { t } = useTranslation();

  return (
    <header className="h-12 px-3 bg-slate-950/90 backdrop-blur-md border-b border-slate-800/90 flex items-center justify-between gap-2 shrink-0">
      <div className="flex items-center gap-2 min-w-0">
        <button
          type="button"
          onClick={onClose}
          className="inline-flex items-center gap-1.5 px-2.5 py-1.5 rounded-lg bg-slate-800/80 hover:bg-slate-700 text-xs font-medium text-slate-200 transition-colors shrink-0"
        >
          <ArrowLeft className="w-3.5 h-3.5" />
          <span>{t('reader.back')}</span>
        </button>
        <span title={title} className="text-xs font-semibold text-slate-200 truncate">
          {title}
        </span>
      </div>

      {/* Center / Right Controls */}
      <div className="flex items-center gap-1.5 overflow-x-auto">
        {/* Layout Switcher */}
        <div className="flex items-center rounded-lg bg-slate-900 p-0.5 border border-slate-800 text-[11px]">
          <button
            type="button"
            onClick={() => onLayoutChange('single')}
            title={t('reader.single')}
            className={`inline-flex items-center gap-1 px-2 py-1 rounded-md ${
              layout === 'single' ? 'bg-brand-600 text-white' : 'text-slate-400 hover:text-white'
            }`}
          >
            <FileText className="w-3.5 h-3.5" />
            <span className="hidden lg:inline">{t('reader.single')}</span>
          </button>
          <button
            type="button"
            onClick={() => onLayoutChange('dual')}
            title={t('reader.dual')}
            className={`inline-flex items-center gap-1 px-2 py-1 rounded-md ${
              layout === 'dual' ? 'bg-brand-600 text-white' : 'text-slate-400 hover:text-white'
            }`}
          >
            <Columns2 className="w-3.5 h-3.5" />
            <span className="hidden lg:inline">{t('reader.dual')}</span>
          </button>
          <button
            type="button"
            onClick={() => onLayoutChange('vertical')}
            title={t('reader.vertical')}
            className={`inline-flex items-center gap-1 px-2 py-1 rounded-md ${
              layout === 'vertical' ? 'bg-brand-600 text-white' : 'text-slate-400 hover:text-white'
            }`}
          >
            <AlignVerticalJustifyStart className="w-3.5 h-3.5" />
            <span className="hidden lg:inline">{t('reader.vertical')}</span>
          </button>
        </div>

        {/* Cover Offset (Page 1 Solo) — Dual-Page Mode only */}
        {layout === 'dual' && (
          <button
            type="button"
            onClick={onToggleCoverOffset}
            title={t('reader.coverOffsetTitle', { defaultValue: 'Cover Offset (Page 1 Solo)' })}
            className={`inline-flex items-center gap-1 px-2 py-1.5 rounded-lg text-[11px] font-medium border transition-colors shrink-0 ${
              coverOffset
                ? 'bg-brand-600/25 border-brand-500/60 text-brand-300'
                : 'bg-slate-900 border-slate-800 text-slate-400 hover:text-white'
            }`}
          >
            <BookOpen className="w-3.5 h-3.5" />
            <span className="hidden xl:inline">
              {t('reader.coverOffset', { defaultValue: 'Cover Offset (Page 1 Solo)' })}
            </span>
          </button>
        )}

        {/* Reading Direction (Manga RTL vs Western LTR) */}
        <button
          type="button"
          onClick={onToggleDirection}
          className={`px-2.5 py-1.5 rounded-lg text-[11px] font-semibold border transition-colors shrink-0 ${
            direction === 'rtl'
              ? 'bg-amber-500/15 border-amber-500/40 text-amber-300'
              : 'bg-sky-500/15 border-sky-500/40 text-sky-300'
          }`}
        >
          {direction === 'rtl' ? t('reader.rtl') : t('reader.ltr')}
        </button>

        {/* Viewport Fit */}
        <select
          value={fit}
          onChange={(e) => onFitChange(e.target.value)}
          className="px-2 py-1.5 rounded-lg bg-slate-900 border border-slate-800 text-[11px] text-slate-200 focus:outline-none cursor-pointer"
        >
          <option value="fitPage">{t('reader.fitPage')}</option>
          <option value="fitWidth">{t('reader.fitWidth')}</option>
          <option value="fitHeight">{t('reader.fitHeight')}</option>
          <option value="original">{t('reader.original')}</option>
        </select>

        {/* Zoom Controls */}
        <div className="hidden sm:flex items-center gap-0.5 bg-slate-900 border border-slate-800 rounded-lg px-1 py-0.5">
          <button
            type="button"
            onClick={onZoomOut}
            className="p-1 text-slate-400 hover:text-white"
          >
            <ZoomOut className="w-3.5 h-3.5" />
          </button>
          <span className="text-[11px] font-mono tabular-nums w-11 text-center text-slate-300">
            {Math.round(zoom * 100)}%
          </span>
          <button
            type="button"
            onClick={onZoomIn}
            className="p-1 text-slate-400 hover:text-white"
          >
            <ZoomIn className="w-3.5 h-3.5" />
          </button>
          {zoom !== 1 && (
            <button
              type="button"
              onClick={onResetZoom}
              className="p-1 text-slate-400 hover:text-white"
            >
              <RotateCcw className="w-3 h-3" />
            </button>
          )}
        </div>

        {/* Background Color Picker */}
        <div className="hidden md:flex items-center gap-1 px-2 py-1 rounded-lg bg-slate-900 border border-slate-800">
          {BG_OPTIONS.map((opt) => (
            <button
              key={opt.id}
              type="button"
              title={t(opt.labelKey)}
              onClick={() => onBgColorChange(opt.value)}
              className={`w-3.5 h-3.5 rounded-full border ${opt.swatch} ${
                bgColor === opt.value ? 'ring-2 ring-brand-500 ring-offset-1 ring-offset-slate-900' : ''
              }`}
            />
          ))}
        </div>

        {/* Auto-Crop Borders */}
        <button
          type="button"
          onClick={onToggleAutoCrop}
          title={t('reader.autoCrop')}
          className={`inline-flex items-center gap-1 px-2 py-1.5 rounded-lg text-[11px] font-medium border transition-colors shrink-0 ${
            autoCrop
              ? 'bg-brand-600/25 border-brand-500/60 text-brand-300'
              : 'bg-slate-900 border-slate-800 text-slate-400 hover:text-white'
          }`}
        >
          <Crop className="w-3.5 h-3.5" />
          <span className="hidden xl:inline">{t('reader.autoCrop')}</span>
        </button>

        {/* Real-Time Manga Enhancer (WebGL2 Shader) */}
        <button
          type="button"
          onClick={onToggleMangaEnhancer}
          title={t('reader.mangaEnhancer', { defaultValue: 'Real-Time Manga Enhancer (WebGL)' })}
          className={`inline-flex items-center gap-1 px-2 py-1.5 rounded-lg text-[11px] font-medium border transition-colors shrink-0 ${
            mangaEnhancer
              ? 'bg-cyan-500/20 border-cyan-400/60 text-cyan-300'
              : 'bg-slate-900 border-slate-800 text-slate-400 hover:text-white'
          }`}
        >
          <Sparkles className="w-3.5 h-3.5" />
          <span className="hidden xl:inline">
            {t('reader.enhance', { defaultValue: 'Enhance' })}
          </span>
        </button>

        {/* Hands-Free Auto-Scroll Controller (Vertical / Webtoon Mode) */}
        {autoScrollEnabled && layout === 'vertical' && (
          <div
            className={`inline-flex items-center gap-1 px-1.5 py-0.5 rounded-full border backdrop-blur-md shadow-lg transition-colors shrink-0 ${
              autoScrollPlaying
                ? 'bg-cyan-500/15 border-cyan-400/60 text-cyan-200'
                : 'bg-slate-900/95 border-slate-700/80 text-slate-300'
            }`}
            title={t('reader.autoScroll', { defaultValue: 'Hands-Free Auto-Scroll' })}
          >
            <button
              type="button"
              onClick={onToggleAutoScroll}
              aria-label={autoScrollPlaying ? 'Pause Auto-Scroll' : 'Play Auto-Scroll'}
              className={`inline-flex items-center justify-center w-6 h-6 rounded-full transition-colors ${
                autoScrollPlaying
                  ? 'bg-cyan-400 text-slate-950 hover:bg-cyan-300'
                  : 'bg-slate-800 text-slate-200 hover:bg-slate-700 hover:text-white'
              }`}
            >
              {autoScrollPlaying ? (
                <Pause className="w-3 h-3 fill-current" />
              ) : (
                <Play className="w-3 h-3 fill-current ml-0.5" />
              )}
            </button>
            <button
              type="button"
              onClick={() => onAutoScrollSpeedChange(Math.max(1, autoScrollSpeed - 1))}
              disabled={autoScrollSpeed <= 1}
              aria-label="Decrease Auto-Scroll Speed"
              className="p-0.5 text-slate-400 hover:text-white disabled:opacity-30 transition-colors"
            >
              <Minus className="w-3 h-3" />
            </button>
            <button
              type="button"
              onClick={() => onAutoScrollSpeedChange(autoScrollSpeed >= 5 ? 1 : autoScrollSpeed + 1)}
              title="Cycle Auto-Scroll Speed (1x–5x)"
              className="text-[11px] font-mono font-semibold tabular-nums min-w-[22px] text-center hover:text-white"
            >
              {autoScrollSpeed}x
            </button>
            <button
              type="button"
              onClick={() => onAutoScrollSpeedChange(Math.min(5, autoScrollSpeed + 1))}
              disabled={autoScrollSpeed >= 5}
              aria-label="Increase Auto-Scroll Speed"
              className="p-0.5 text-slate-400 hover:text-white disabled:opacity-30 transition-colors"
            >
              <Plus className="w-3 h-3" />
            </button>
          </div>
        )}

        {/* Night Mode */}
        <button
          type="button"
          onClick={onToggleNightMode}
          title={t('reader.nightMode')}
          className={`inline-flex items-center gap-1 px-2 py-1.5 rounded-lg text-[11px] font-medium border transition-colors shrink-0 ${
            nightMode
              ? 'bg-indigo-600/25 border-indigo-400/60 text-indigo-300'
              : 'bg-slate-900 border-slate-800 text-slate-400 hover:text-white'
          }`}
        >
          <Moon className="w-3.5 h-3.5" />
          <span className="hidden xl:inline">{t('reader.nightMode')}</span>
        </button>

        {/* Bookmarks Drawer Toggle */}
        <button
          type="button"
          onClick={onToggleBookmarks}
          title={t('reader.bookmarks')}
          className={`inline-flex items-center gap-1 px-2 py-1.5 rounded-lg text-[11px] font-medium border transition-colors shrink-0 ${
            bookmarksOpen
              ? 'bg-amber-500/25 border-amber-400/60 text-amber-300'
              : 'bg-slate-900 border-slate-800 text-slate-400 hover:text-white'
          }`}
        >
          <Bookmark className="w-3.5 h-3.5" />
          <span className="tabular-nums">{bookmarksCount}</span>
        </button>

        {/* Fullscreen */}
        <button
          type="button"
          onClick={onToggleFullscreen}
          title={t('reader.fullscreen')}
          className="p-1.5 rounded-lg bg-slate-900 border border-slate-800 text-slate-400 hover:text-white shrink-0"
        >
          {isFullscreen ? <Minimize2 className="w-3.5 h-3.5" /> : <Maximize2 className="w-3.5 h-3.5" />}
        </button>
      </div>
    </header>
  );
}
