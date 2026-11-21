import React from 'react';
import ToggleRow from './ToggleRow';

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

export default function GeneralSettingsTab({ settings, busy, onToggleSetting }) {
  return (
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
              onToggle={() => onToggleSetting(item.key, item.defaultVal)}
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
              onToggle={() => onToggleSetting(item.key, item.defaultVal)}
            />
          ))}
        </div>
      </div>
    </>
  );
}
