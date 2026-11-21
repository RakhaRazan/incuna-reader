import React, { useEffect, useRef, useState } from 'react';
import { renderEnhancedMangaToCanvas, isWebGL2Available } from '../utils/webglMangaEnhancer';

export default function MangaPage({
  src,
  alt,
  enhanced = false,
  style,
  className,
  onLoad,
}) {
  const canvasRef = useRef(null);
  const onLoadRef = useRef(onLoad);
  onLoadRef.current = onLoad;
  const [webglReady, setWebglReady] = useState(false);
  const [fallbackToImg, setFallbackToImg] = useState(false);

  const shouldUseWebGL = enhanced && !fallbackToImg && isWebGL2Available();

  useEffect(() => {
    if (!shouldUseWebGL) {
      setWebglReady(false);
      return;
    }

    let cancelled = false;
    const img = new Image();
    img.decoding = 'async';

    img.onload = () => {
      if (cancelled) return;
      onLoadRef.current?.({ currentTarget: img });

      const canvas = canvasRef.current;
      if (!canvas) {
        setFallbackToImg(true);
        return;
      }

      const ok = renderEnhancedMangaToCanvas(img, canvas);
      if (!ok) {
        setFallbackToImg(true);
      } else {
        setWebglReady(true);
      }
    };

    img.onerror = () => {
      if (!cancelled) setFallbackToImg(true);
    };

    img.src = src;

    return () => {
      cancelled = true;
    };
  }, [src, shouldUseWebGL]);

  // Reset fallback if user re-toggles enhancer
  useEffect(() => {
    if (enhanced) {
      setFallbackToImg(false);
    }
  }, [enhanced, src]);

  if (!shouldUseWebGL) {
    return (
      <img
        src={src}
        alt={alt}
        decoding="async"
        draggable={false}
        data-reader-page="true"
        onLoad={onLoad}
        style={style}
        className={className}
      />
    );
  }

  return (
    <canvas
      ref={canvasRef}
      aria-label={alt}
      role="img"
      draggable={false}
      data-reader-page="true"
      style={{
        ...style,
        opacity: webglReady ? 1 : 0,
      }}
      className={className}
    />
  );
}
