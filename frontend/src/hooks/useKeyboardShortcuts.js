import { useEffect } from 'react';

export const SHORTCUT_DEFINITIONS = [
  { id: 'search_library', label: 'Search in library', scope: 'library', defaults: ['F3'] },
  { id: 'toggle_fullscreen', label: 'Toggle full screen', scope: 'reader', defaults: ['Ctrl+F'] },
  { id: 'go_to_library', label: 'Go to library', scope: 'reader', defaults: ['Backspace'] },
  { id: 'toggle_controls', label: 'Toggle reading controls', scope: 'reader', defaults: ['Escape'] },
  { id: 'read_forward', label: 'Read forward', scope: 'reader', defaults: ['Space', 'ArrowDown', 'ArrowRight'] },
  { id: 'read_backwards', label: 'Read backwards', scope: 'reader', defaults: ['ArrowUp', 'ArrowLeft'] },
  { id: 'alt_forward', label: 'Alt forward', scope: 'reader', defaults: ['S'] },
  { id: 'alt_backwards', label: 'Alt backwards', scope: 'reader', defaults: ['Z'] },
  { id: 'next_page', label: 'Go to next page', scope: 'reader', defaults: ['PageDown'] },
  { id: 'prev_page', label: 'Go to previous page', scope: 'reader', defaults: ['PageUp'] },
  { id: 'first_page', label: 'Go to first page', scope: 'reader', defaults: ['Home'] },
  { id: 'last_page', label: 'Go to last page', scope: 'reader', defaults: ['End'] },
  { id: 'next_book', label: 'Go to next book', scope: 'reader', defaults: ['Ctrl+PageDown'] },
  { id: 'prev_book', label: 'Go to previous book', scope: 'reader', defaults: ['Ctrl+PageUp'] },
  { id: 'cycle_night_mode', label: 'Cycle night mode', scope: 'reader', defaults: ['N'] },
  { id: 'toggle_manga_mode', label: 'Toggle manga mode', scope: 'reader', defaults: ['M'] },
  { id: 'cycle_page_mode', label: 'Cycle page mode', scope: 'reader', defaults: ['P'] },
  { id: 'cycle_fit_mode', label: 'Cycle fit mode', scope: 'reader', defaults: ['F'] },
];

export const DEFAULT_SHORTCUTS = SHORTCUT_DEFINITIONS.reduce((acc, def) => {
  acc[def.id] = [...def.defaults];
  return acc;
}, {});

const MODIFIER_KEYS = new Set(['Control', 'Shift', 'Alt', 'Meta']);

export function formatKeyEvent(e) {
  if (!e || MODIFIER_KEYS.has(e.key)) return null;
  const parts = [];
  if (e.ctrlKey || e.metaKey) parts.push('Ctrl');
  if (e.altKey) parts.push('Alt');
  if (e.shiftKey) parts.push('Shift');

  let key = e.key;
  if (key === ' ') key = 'Space';
  else if (key.length === 1 && /[a-z]/i.test(key)) key = key.toUpperCase();

  parts.push(key);
  return parts.join('+');
}

export function normalizeBindingList(value) {
  if (Array.isArray(value)) return value.filter(Boolean);
  if (typeof value === 'string' && value.trim()) {
    if (value === ' ') return ['Space'];
    return [value.trim()];
  }
  return [];
}

export function resolveShortcuts(savedShortcuts) {
  const resolved = { ...DEFAULT_SHORTCUTS };
  if (!savedShortcuts || typeof savedShortcuts !== 'object') return resolved;

  const hasNewSchema = Object.prototype.hasOwnProperty.call(savedShortcuts, 'read_forward');
  for (const def of SHORTCUT_DEFINITIONS) {
    const raw = savedShortcuts[def.id];
    if (Array.isArray(raw)) {
      resolved[def.id] = raw.filter(Boolean);
    } else if (hasNewSchema && typeof raw === 'string' && raw.trim()) {
      resolved[def.id] = normalizeBindingList(raw);
    }
  }
  return resolved;
}

export function matchesShortcut(e, binding) {
  const combo = formatKeyEvent(e);
  if (!combo) return false;
  const list = normalizeBindingList(binding);
  const comboLower = combo.toLowerCase();
  return list.some((item) => item.toLowerCase() === comboLower);
}

export function useReaderShortcuts({
  enabled = true,
  shortcuts,
  direction,
  layout,
  bookmarksOpen,
  onGoNext,
  onGoPrev,
  onFirstPage,
  onLastPage,
  onNextBook,
  onPrevBook,
  onToggleFullscreen,
  onToggleDirection,
  onCycleLayout,
  onCycleFit,
  onToggleNightMode,
  onToggleControls,
  onOpenBookmarks,
  onCloseBookmarks,
  onZoomIn,
  onZoomOut,
  onResetZoom,
  onNextVolume,
  onCloseReader,
}) {
  useEffect(() => {
    if (!enabled) return;
    const map = resolveShortcuts(shortcuts);

    const handleKeyDown = (e) => {
      const tag = e.target?.tagName?.toLowerCase();
      if (tag === 'input' || tag === 'textarea' || tag === 'select') {
        if (e.key === 'Escape') {
          e.target.blur();
          onCloseBookmarks?.();
        }
        return;
      }

      if (matchesShortcut(e, map.toggle_controls)) {
        e.preventDefault();
        if (bookmarksOpen) {
          onCloseBookmarks?.();
        } else {
          onToggleControls?.();
        }
        return;
      }

      if (matchesShortcut(e, map.go_to_library)) {
        e.preventDefault();
        onCloseReader?.();
        return;
      }

      if (matchesShortcut(e, map.toggle_fullscreen)) {
        e.preventDefault();
        onToggleFullscreen?.();
        return;
      }

      if (matchesShortcut(e, map.next_book)) {
        e.preventDefault();
        onNextBook?.();
        return;
      }

      if (matchesShortcut(e, map.prev_book)) {
        e.preventDefault();
        onPrevBook?.();
        return;
      }

      if (matchesShortcut(e, map.first_page)) {
        e.preventDefault();
        onFirstPage?.();
        return;
      }

      if (matchesShortcut(e, map.last_page)) {
        e.preventDefault();
        onLastPage?.();
        return;
      }

      if (matchesShortcut(e, map.next_page)) {
        e.preventDefault();
        onGoNext?.();
        return;
      }

      if (matchesShortcut(e, map.prev_page)) {
        e.preventDefault();
        onGoPrev?.();
        return;
      }

      if (matchesShortcut(e, map.read_forward)) {
        if (e.key === ' ' && layout === 'vertical') return;
        e.preventDefault();
        if (direction === 'rtl' && (e.key === 'ArrowLeft' || e.key === 'ArrowRight')) {
          e.key === 'ArrowLeft' ? onGoNext?.() : onGoPrev?.();
        } else {
          onGoNext?.();
        }
        return;
      }

      if (matchesShortcut(e, map.read_backwards)) {
        e.preventDefault();
        if (direction === 'rtl' && (e.key === 'ArrowLeft' || e.key === 'ArrowRight')) {
          e.key === 'ArrowLeft' ? onGoNext?.() : onGoPrev?.();
        } else {
          onGoPrev?.();
        }
        return;
      }

      if (matchesShortcut(e, map.alt_forward)) {
        e.preventDefault();
        onGoNext?.();
        return;
      }

      if (matchesShortcut(e, map.alt_backwards)) {
        e.preventDefault();
        onGoPrev?.();
        return;
      }

      if (matchesShortcut(e, map.cycle_night_mode)) {
        e.preventDefault();
        onToggleNightMode?.();
        return;
      }

      if (matchesShortcut(e, map.toggle_manga_mode)) {
        e.preventDefault();
        onToggleDirection?.();
        return;
      }

      if (matchesShortcut(e, map.cycle_page_mode)) {
        e.preventDefault();
        onCycleLayout?.();
        return;
      }

      if (matchesShortcut(e, map.cycle_fit_mode)) {
        e.preventDefault();
        onCycleFit?.();
        return;
      }

      // Secondary convenience shortcuts
      switch (e.key) {
        case 'b':
        case 'B':
          e.preventDefault();
          onOpenBookmarks?.();
          break;
        case '=':
        case '+':
          e.preventDefault();
          onZoomIn?.();
          break;
        case '-':
          e.preventDefault();
          onZoomOut?.();
          break;
        case '0':
          e.preventDefault();
          onResetZoom?.();
          break;
        case 'Enter':
          if (onNextVolume) {
            e.preventDefault();
            onNextVolume();
          }
          break;
        default:
          break;
      }
    };

    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [
    enabled,
    shortcuts,
    direction,
    layout,
    bookmarksOpen,
    onGoNext,
    onGoPrev,
    onFirstPage,
    onLastPage,
    onNextBook,
    onPrevBook,
    onToggleFullscreen,
    onToggleDirection,
    onCycleLayout,
    onCycleFit,
    onToggleNightMode,
    onToggleControls,
    onOpenBookmarks,
    onCloseBookmarks,
    onZoomIn,
    onZoomOut,
    onResetZoom,
    onNextVolume,
    onCloseReader,
  ]);
}

export function useLibraryShortcuts({ enabled = true, shortcuts, onFocusSearch, onEscape }) {
  useEffect(() => {
    if (!enabled) return;
    const map = resolveShortcuts(shortcuts);

    const handleKeyDown = (e) => {
      const tag = e.target?.tagName?.toLowerCase();
      const inInput = tag === 'input' || tag === 'textarea' || tag === 'select';

      if (matchesShortcut(e, map.search_library)) {
        e.preventDefault();
        onFocusSearch?.();
        return;
      }

      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'f') {
        e.preventDefault();
        onFocusSearch?.();
        return;
      }

      if (!inInput && e.key === '/') {
        e.preventDefault();
        onFocusSearch?.();
        return;
      }

      if (e.key === 'Escape') {
        onEscape?.();
      }
    };

    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [enabled, shortcuts, onFocusSearch, onEscape]);
}
