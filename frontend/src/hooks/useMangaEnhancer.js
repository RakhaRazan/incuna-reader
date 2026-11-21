import { useEffect, useRef, useState } from 'react';
import {
  renderEnhancedMangaToCanvas,
  isWebGL2Available,
} from '../utils/webglMangaEnhancer';

export function useMangaEnhancer({ src, enhanced, canvasRef, onLoad }) {
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
  }, [src, shouldUseWebGL, canvasRef]);

  // Reset fallback if user re-toggles enhancer or changes page src
  useEffect(() => {
    if (enhanced) {
      setFallbackToImg(false);
    }
  }, [enhanced, src]);

  return { shouldUseWebGL, webglReady };
}
