import React from 'react';
import { History } from 'lucide-react';
import { api } from '../../api/client';
import { formatDisplayTitle } from '../../utils/format';

const padNum = (n) => String(n).padStart(3, '0');

export default function ContinueReadingBanner({
  items,
  coverVersion,
  smartRename,
  onOpenComic,
}) {
  if (!items || items.length === 0) return null;

  return (
    <div className="shrink-0 px-6 py-3 border-b border-[#DCE1E8] dark:border-[#222834] bg-white/70 dark:bg-[#12151B]/70">
      <div className="flex items-center gap-2 mb-2">
        <History className="w-3.5 h-3.5 text-cyan-600 dark:text-[#22D3EE]" />
        <h3 className="text-[11px] font-bold uppercase tracking-wider text-slate-600 dark:text-slate-300">
          Continue Reading
        </h3>
      </div>
      <div className="flex items-stretch gap-3 overflow-x-auto pb-1">
        {items.map((item) => {
          const pct =
            item.page_count > 0
              ? Math.min(100, Math.round((item.current_page / item.page_count) * 100))
              : item.status === 'read'
                ? 100
                : 0;
          return (
            <button
              key={`cr-${item.id}`}
              type="button"
              onClick={() => onOpenComic?.(item)}
              className="group shrink-0 w-56 flex items-center gap-2.5 p-2 rounded-lg bg-white dark:bg-[#0B0D10] border border-[#DCE1E8] dark:border-[#222834] hover:border-cyan-600/80 dark:hover:border-[#22D3EE]/80 transition-colors text-left"
            >
              <div className="relative w-11 h-16 rounded overflow-hidden bg-slate-200 dark:bg-[#171C24] shrink-0">
                <img
                  src={api.getCoverUrl(item.id, coverVersion)}
                  alt={item.title}
                  loading="lazy"
                  decoding="async"
                  className="w-full h-full object-cover"
                />
              </div>
              <div className="min-w-0 flex-1 flex flex-col justify-between h-14">
                <div className="text-xs font-semibold text-slate-900 dark:text-[#F1F5F9] line-clamp-2 leading-tight group-hover:text-cyan-700 dark:group-hover:text-[#22D3EE]">
                  {formatDisplayTitle(item.title, smartRename)}
                </div>
                <div className="space-y-1">
                  <div className="flex items-center justify-between text-[10px] font-mono tabular-nums text-slate-500 dark:text-slate-400">
                    <span>
                      {item.page_count > 0
                        ? `${padNum(item.current_page)} / ${padNum(item.page_count)}`
                        : item.status}
                    </span>
                    <span>{pct}%</span>
                  </div>
                  <div className="h-1 w-full rounded-full bg-slate-200 dark:bg-[#171C24] overflow-hidden">
                    <div
                      className="h-full bg-cyan-600 dark:bg-[#22D3EE]"
                      style={{ width: `${pct}%` }}
                    />
                  </div>
                </div>
              </div>
            </button>
          );
        })}
      </div>
    </div>
  );
}
