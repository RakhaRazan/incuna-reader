import React from 'react';
import { RotateCcw } from 'lucide-react';
import { SHORTCUT_DEFINITIONS, resolveShortcuts } from '../../hooks/useKeyboardShortcuts';

export default function ShortcutsSettingsTab({
  shortcuts,
  busy,
  recordingAction,
  onStartRecord,
  onCancelRecord,
  onResetAll,
}) {
  const resolvedShortcuts = resolveShortcuts(shortcuts);

  return (
    <div className="space-y-3">
      <div className="flex items-center justify-between gap-2">
        <p className="text-xs text-slate-500 dark:text-slate-400">
          Click <span className="font-semibold text-slate-700 dark:text-slate-200">Edit</span> and press any key combination to rebind.
        </p>
        <button
          type="button"
          disabled={busy}
          onClick={onResetAll}
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
                    onClick={onCancelRecord}
                    className="px-2.5 py-1 rounded-md bg-slate-200 dark:bg-[#171C24] text-slate-700 dark:text-slate-300 text-[11px] font-semibold hover:opacity-90"
                  >
                    Cancel
                  </button>
                ) : (
                  <button
                    type="button"
                    disabled={busy}
                    onClick={() => onStartRecord(def.id)}
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
  );
}
