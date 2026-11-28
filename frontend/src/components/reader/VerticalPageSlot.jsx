import React, { memo, useCallback, useEffect, useRef, useState } from 'react';
import MangaPage from '../MangaPage';

const VerticalPageSlot = memo(function VerticalPageSlot({
  pageIndex,
  pageUrl,
  isMounted,
  mangaEnhancer,
  nightModeFilter,
  fit,
  zoom,
  onVisible,
}) {
  const slotRef = useRef(null);
  const [aspectRatio, setAspectRatio] = useState(0.707);
  const [nearViewport, setNearViewport] = useState(false);

  useEffect(() => {
    const el = slotRef.current;
    if (!el) return;

    // 1. Generous mount/load margin so tall Webtoon slices mount before entering viewport
    const mountObserver = new IntersectionObserver(
      (entries) => {
        if (entries[0]) {
          setNearViewport(entries[0].isIntersecting);
        }
      },
      { rootMargin: '200% 0px', threshold: 0 }
    );
    mountObserver.observe(el);

    // 2. Center-band active page tracker (works for 800x6000px slices that exceed viewport height)
    const activeObserver = new IntersectionObserver(
      (entries) => {
        if (entries[0]?.isIntersecting) {
          onVisible(pageIndex);
        }
      },
      { rootMargin: '-45% 0px -45% 0px', threshold: 0 }
    );
    activeObserver.observe(el);

    return () => {
      mountObserver.disconnect();
      activeObserver.disconnect();
    };
  }, [pageIndex, onVisible]);

  const handleLoad = useCallback((e) => {
    const img = e?.currentTarget;
    if (img?.naturalWidth && img?.naturalHeight) {
      setAspectRatio(img.naturalWidth / img.naturalHeight);
    }
  }, []);

  const widthStyle =
    fit === 'original'
      ? { width: 'auto', maxWidth: 'none' }
      : { width: `${Math.round(100 * zoom)}%`, maxWidth: zoom === 1 ? '960px' : 'none' };

  const placeholderStyle =
    fit === 'original'
      ? { width: 'min(720px, 90vw)', aspectRatio: String(aspectRatio) }
      : { ...widthStyle, aspectRatio: String(aspectRatio) };

  const shouldMount = isMounted || nearViewport;

  return (
    <div
      ref={slotRef}
      data-page-index={pageIndex}
      className="w-full flex justify-center min-h-[360px]"
    >
      {shouldMount ? (
        <MangaPage
          src={pageUrl}
          alt={`Page ${pageIndex + 1}`}
          enhanced={mangaEnhancer}
          onLoad={handleLoad}
          style={{
            ...widthStyle,
            filter: nightModeFilter,
          }}
          className="h-auto object-contain select-none shadow-lg"
        />
      ) : (
        <div style={placeholderStyle} className="bg-slate-800/25 rounded" />
      )}
    </div>
  );
});

export default VerticalPageSlot;
