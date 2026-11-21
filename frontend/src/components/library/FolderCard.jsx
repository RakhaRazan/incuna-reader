import React, { memo } from 'react';
import { Folder, ChevronRight } from 'lucide-react';
import { formatDisplayTitle } from '../../utils/format';
import { CoverImage } from '../ComicCard';

const FolderCard = memo(function FolderCard({
  folder,
  coverHeight,
  coverVersion,
  deferCover,
  smartRename,
  onEnterFolder,
}) {
  return (
    <button
      type="button"
      onClick={() => onEnterFolder(folder)}
      className="group relative w-full h-full rounded-lg bg-white dark:bg-[#12151B] border border-[#DCE1E8] dark:border-[#222834] hover:border-cyan-600/80 dark:hover:border-[#22D3EE]/80 overflow-hidden text-left flex flex-col cursor-pointer"
    >
      <div
        style={{ height: `${coverHeight}px` }}
        className="relative w-full bg-slate-200 dark:bg-[#0B0D10] overflow-hidden flex items-center justify-center shrink-0"
      >
        {folder.preview_comic_id > 0 ? (
          <CoverImage
            comicId={folder.preview_comic_id}
            coverVersion={coverVersion}
            title={folder.name}
            deferCover={deferCover}
            isFolder
          />
        ) : (
          <Folder className="w-12 h-12 text-cyan-600/70 dark:text-[#22D3EE]/70" />
        )}
        <div className="absolute top-2 left-2 z-20 p-1.5 rounded-md bg-[#0B0D10]/80 border border-[#222834] text-[#22D3EE]">
          <Folder className="w-4 h-4" />
        </div>
        <span className="absolute bottom-2 right-2 z-20 px-2 py-0.5 rounded text-[11px] font-mono font-semibold tabular-nums bg-[#0B0D10]/85 border border-[#222834] text-[#F1F5F9]">
          {folder.comic_count.toLocaleString()}
        </span>
      </div>
      <div className="px-2.5 py-2 flex-1 flex items-center justify-between gap-2 min-w-0">
        <span
          title={folder.name}
          className="text-xs font-semibold text-slate-900 dark:text-[#F1F5F9] line-clamp-2 leading-snug"
        >
          {formatDisplayTitle(folder.name, smartRename)}
        </span>
        <ChevronRight className="w-3.5 h-3.5 text-slate-400 group-hover:text-cyan-600 dark:group-hover:text-[#22D3EE] shrink-0" />
      </div>
    </button>
  );
});

export default FolderCard;
