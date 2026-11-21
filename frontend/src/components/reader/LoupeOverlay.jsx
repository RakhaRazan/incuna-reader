import React from 'react';
import { LOUPE_SIZE, LOUPE_DEFAULT_ZOOM } from '../../hooks/useLoupeMagnifier';

export default function LoupeOverlay({
  overlayRef,
  canvasRef,
  badgeRef,
  bgColor,
  nightModeFilter,
}) {
  return (
    <div
      ref={overlayRef}
      aria-hidden="true"
      style={{
        position: 'fixed',
        left: 0,
        top: 0,
        width: `${LOUPE_SIZE}px`,
        height: `${LOUPE_SIZE}px`,
        borderRadius: '9999px',
        border: '1.5px solid #22D3EE',
        boxShadow: '0 0 0 1px rgba(34, 211, 238, 0.35), 0 14px 36px rgba(0, 0, 0, 0.72)',
        overflow: 'hidden',
        pointerEvents: 'none',
        zIndex: 60,
        opacity: 0,
        visibility: 'hidden',
        willChange: 'transform',
        backgroundColor: bgColor,
      }}
    >
      <canvas
        ref={canvasRef}
        style={{
          width: '100%',
          height: '100%',
          display: 'block',
          filter: nightModeFilter,
        }}
      />
      <span
        ref={badgeRef}
        className="absolute bottom-2 left-1/2 -translate-x-1/2 px-1.5 py-0.5 rounded-full bg-slate-950/80 border border-[#22D3EE]/50 text-[10px] font-mono font-semibold text-[#22D3EE] tabular-nums leading-none"
      >
        {LOUPE_DEFAULT_ZOOM.toFixed(1)}x
      </span>
    </div>
  );
}
