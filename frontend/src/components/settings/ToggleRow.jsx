import React from 'react';

export default function ToggleRow({ label, description, checked, disabled, onToggle }) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={checked}
      disabled={disabled}
      onClick={onToggle}
      className="w-full flex items-center justify-between gap-4 px-3.5 py-2.5 rounded-lg border border-[#DCE1E8] dark:border-[#222834] bg-[#F4F6F9]/50 dark:bg-[#0B0D10]/50 hover:border-cyan-600/50 dark:hover:border-[#22D3EE]/50 transition-colors text-left disabled:opacity-50"
    >
      <div className="min-w-0">
        <div className="text-xs font-semibold text-slate-900 dark:text-[#F1F5F9]">
          {label}
        </div>
        {description && (
          <div className="text-[11px] text-slate-500 dark:text-slate-400 mt-0.5 leading-snug">
            {description}
          </div>
        )}
      </div>
      <span
        className={`relative inline-flex h-5 w-9 shrink-0 items-center rounded-full transition-colors ${
          checked ? 'bg-cyan-600 dark:bg-[#22D3EE]' : 'bg-slate-300 dark:bg-slate-700'
        }`}
      >
        <span
          className={`inline-block h-3.5 w-3.5 transform rounded-full bg-white dark:bg-[#0B0D10] shadow transition-transform ${
            checked ? 'translate-x-4.5' : 'translate-x-1'
          }`}
          style={{ transform: checked ? 'translateX(18px)' : 'translateX(3px)' }}
        />
      </span>
    </button>
  );
}
