import React, { useRef } from 'react';
import { useMangaEnhancer } from '../hooks/useMangaEnhancer';

export default function MangaPage({
  src,
  alt,
  enhanced = false,
  style,
  className,
  onLoad,
}) {
  const canvasRef = useRef(null);
  const { shouldUseWebGL, webglReady } = useMangaEnhancer({
    src,
    enhanced,
    canvasRef,
    onLoad,
  });

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
