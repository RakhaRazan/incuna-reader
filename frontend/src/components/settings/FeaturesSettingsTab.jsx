import React from 'react';
import ToggleRow from './ToggleRow';

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

export default function FeaturesSettingsTab({ settings, busy, onToggleSetting }) {
  return (
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
            onToggle={() => onToggleSetting(item.key, item.defaultVal)}
          />
        ))}
      </div>
    </div>
  );
}
