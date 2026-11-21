import { useCallback, useEffect, useRef } from 'react';

export const LOUPE_SIZE = 220;
export const LOUPE_MIN_ZOOM = 2.5;
export const LOUPE_MAX_ZOOM = 3.5;
export const LOUPE_DEFAULT_ZOOM = 3.0;

function getRenderedMediaRect(el, srcW, srcH) {
  const rect = el.getBoundingClientRect();
  if (!rect.width || !rect.height || !srcW || !srcH) return rect;
  const boxRatio = rect.width / rect.height;
  const mediaRatio = srcW / srcH;
  if (Math.abs(boxRatio - mediaRatio) < 0.01) return rect;

  if (mediaRatio > boxRatio) {
    const renderedH = rect.width / mediaRatio;
    const offsetY = (rect.height - renderedH) / 2;
    return {
      left: rect.left,
      top: rect.top + offsetY,
      right: rect.right,
      bottom: rect.top + offsetY + renderedH,
      width: rect.width,
      height: renderedH,
    };
  }

  const renderedW = rect.height * mediaRatio;
  const offsetX = (rect.width - renderedW) / 2;
  return {
    left: rect.left + offsetX,
    top: rect.top,
    right: rect.left + offsetX + renderedW,
    bottom: rect.bottom,
    width: renderedW,
    height: rect.height,
  };
}

export function useLoupeMagnifier({
  enabled,
  bgColor,
  viewportRef,
  pointerDownRef,
  dragRef,
}) {
  const loupeOverlayRef = useRef(null);
  const loupeCanvasRef = useRef(null);
  const loupeBadgeRef = useRef(null);
  const loupeStateRef = useRef({
    active: false,
    rightMouseDown: false,
    keyDown: false,
    x: 0,
    y: 0,
    hasPointer: false,
    zoom: LOUPE_DEFAULT_ZOOM,
    rafId: 0,
  });

  // Loupe Magnifier renderer (runs inside rAF, zero React re-renders per frame)
  const renderLoupeFrame = useCallback(() => {
    const state = loupeStateRef.current;
    state.rafId = 0;

    const overlay = loupeOverlayRef.current;
    const canvas = loupeCanvasRef.current;
    const viewport = viewportRef.current;
    if (!overlay || !canvas) return;

    if (!state.active || !enabled || !viewport) {
      overlay.style.opacity = '0';
      overlay.style.visibility = 'hidden';
      return;
    }

    const { x, y, zoom: loupeZoom } = state;
    const half = LOUPE_SIZE / 2;

    overlay.style.visibility = 'visible';
    overlay.style.opacity = '1';
    overlay.style.transform = `translate3d(${Math.round(x - half)}px, ${Math.round(y - half)}px, 0)`;

    if (loupeBadgeRef.current) {
      loupeBadgeRef.current.textContent = `${loupeZoom.toFixed(1)}x`;
    }

    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const backingSize = Math.round(LOUPE_SIZE * dpr);
    if (canvas.width !== backingSize || canvas.height !== backingSize) {
      canvas.width = backingSize;
      canvas.height = backingSize;
    }

    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    ctx.save();
    ctx.fillStyle = bgColor || '#000000';
    ctx.fillRect(0, 0, backingSize, backingSize);

    // Find the page media element (<img> or WebGL-enhanced <canvas>) under cursor
    const pages = viewport.querySelectorAll('[data-reader-page="true"]');
    let targetEl = null;
    let targetRect = null;
    let srcW = 0;
    let srcH = 0;

    for (let i = 0; i < pages.length; i += 1) {
      const el = pages[i];
      const w = el.tagName === 'IMG' ? el.naturalWidth : el.width;
      const h = el.tagName === 'IMG' ? el.naturalHeight : el.height;
      if (!w || !h) continue;
      const r = getRenderedMediaRect(el, w, h);
      if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) {
        targetEl = el;
        targetRect = r;
        srcW = w;
        srcH = h;
        break;
      }
    }

    if (targetEl && targetRect && targetRect.width > 0 && targetRect.height > 0) {
      const scaleX = srcW / targetRect.width;
      const scaleY = srcH / targetRect.height;

      const centerSrcX = (x - targetRect.left) * scaleX;
      const centerSrcY = (y - targetRect.top) * scaleY;

      const sw = (LOUPE_SIZE / loupeZoom) * scaleX;
      const sh = (LOUPE_SIZE / loupeZoom) * scaleY;
      const sx = centerSrcX - sw / 2;
      const sy = centerSrcY - sh / 2;

      // Clamp source rectangle safely so page edges show background without stretching
      const clampedSx = Math.max(0, sx);
      const clampedSy = Math.max(0, sy);
      const clampedEx = Math.min(srcW, sx + sw);
      const clampedEy = Math.min(srcH, sy + sh);
      const clampedSw = clampedEx - clampedSx;
      const clampedSh = clampedEy - clampedSy;

      if (clampedSw > 0 && clampedSh > 0) {
        const dx = ((clampedSx - sx) / sw) * backingSize;
        const dy = ((clampedSy - sy) / sh) * backingSize;
        const dw = (clampedSw / sw) * backingSize;
        const dh = (clampedSh / sh) * backingSize;

        ctx.imageSmoothingEnabled = true;
        ctx.imageSmoothingQuality = 'high';
        ctx.drawImage(targetEl, clampedSx, clampedSy, clampedSw, clampedSh, dx, dy, dw, dh);
      }
    }

    ctx.restore();
  }, [enabled, bgColor, viewportRef]);

  const scheduleLoupeFrame = useCallback(() => {
    const state = loupeStateRef.current;
    if (!state.rafId) {
      state.rafId = requestAnimationFrame(renderLoupeFrame);
    }
  }, [renderLoupeFrame]);

  const hideLoupeImmediately = useCallback(() => {
    const state = loupeStateRef.current;
    state.active = false;
    state.rightMouseDown = false;
    state.keyDown = false;
    if (state.rafId) {
      cancelAnimationFrame(state.rafId);
      state.rafId = 0;
    }
    const overlay = loupeOverlayRef.current;
    if (overlay) {
      overlay.style.opacity = '0';
      overlay.style.visibility = 'hidden';
    }
  }, []);

  useEffect(() => {
    if (!enabled) {
      hideLoupeImmediately();
    }
  }, [enabled, hideLoupeImmediately]);

  // Global key & mouse release listeners for Loupe Magnifier (hold L / right-click)
  useEffect(() => {
    const onKeyDown = (e) => {
      if (!enabled) return;
      if (e.repeat || e.ctrlKey || e.metaKey || e.altKey) return;
      if (e.key !== 'l' && e.key !== 'L') return;
      const tag = e.target?.tagName?.toLowerCase();
      if (tag === 'input' || tag === 'textarea' || tag === 'select') return;

      e.preventDefault();
      const state = loupeStateRef.current;
      state.keyDown = true;
      state.active = true;
      if (!state.hasPointer && viewportRef.current) {
        const r = viewportRef.current.getBoundingClientRect();
        state.x = r.left + r.width / 2;
        state.y = r.top + r.height / 2;
      }
      scheduleLoupeFrame();
    };

    const onKeyUp = (e) => {
      if (e.key !== 'l' && e.key !== 'L') return;
      const state = loupeStateRef.current;
      if (!state.keyDown) return;
      state.keyDown = false;
      if (!state.rightMouseDown) {
        hideLoupeImmediately();
      }
    };

    const onWindowMouseUp = (e) => {
      if (pointerDownRef) pointerDownRef.current = false;
      if (dragRef) dragRef.current.active = false;
      const state = loupeStateRef.current;
      if (state.rightMouseDown && (e.button === 2 || (e.buttons & 2) === 0)) {
        state.rightMouseDown = false;
        if (!state.keyDown) {
          hideLoupeImmediately();
        }
      }
    };

    const onWindowBlur = () => {
      if (pointerDownRef) pointerDownRef.current = false;
      if (dragRef) dragRef.current.active = false;
      hideLoupeImmediately();
    };

    window.addEventListener('keydown', onKeyDown);
    window.addEventListener('keyup', onKeyUp);
    window.addEventListener('mouseup', onWindowMouseUp);
    window.addEventListener('blur', onWindowBlur);
    return () => {
      window.removeEventListener('keydown', onKeyDown);
      window.removeEventListener('keyup', onKeyUp);
      window.removeEventListener('mouseup', onWindowMouseUp);
      window.removeEventListener('blur', onWindowBlur);
      hideLoupeImmediately();
    };
  }, [enabled, viewportRef, pointerDownRef, dragRef, scheduleLoupeFrame, hideLoupeImmediately]);

  const handleLoupeWheel = useCallback(
    (e) => {
      if (!loupeStateRef.current.active) return false;
      e.preventDefault();
      const delta = e.deltaY < 0 ? 0.25 : -0.25;
      loupeStateRef.current.zoom = Math.max(
        LOUPE_MIN_ZOOM,
        Math.min(LOUPE_MAX_ZOOM, Number((loupeStateRef.current.zoom + delta).toFixed(2)))
      );
      scheduleLoupeFrame();
      return true;
    },
    [scheduleLoupeFrame]
  );

  const activateRightClickLoupe = useCallback(
    (e) => {
      if (!enabled || e.button !== 2) return false;
      e.preventDefault();
      const state = loupeStateRef.current;
      state.rightMouseDown = true;
      state.active = true;
      state.x = e.clientX;
      state.y = e.clientY;
      state.hasPointer = true;
      scheduleLoupeFrame();
      return true;
    },
    [enabled, scheduleLoupeFrame]
  );

  const updatePointer = useCallback(
    (clientX, clientY) => {
      const state = loupeStateRef.current;
      state.x = clientX;
      state.y = clientY;
      state.hasPointer = true;
      if (state.active) {
        scheduleLoupeFrame();
      }
    },
    [scheduleLoupeFrame]
  );

  const releaseRightClickLoupe = useCallback((e) => {
    const state = loupeStateRef.current;
    if (state.rightMouseDown && (!e || e.button === 2 || (e.buttons & 2) === 0)) {
      state.rightMouseDown = false;
      if (!state.keyDown) {
        hideLoupeImmediately();
      }
    }
  }, [hideLoupeImmediately]);

  const handleLoupeMouseLeave = useCallback(() => {
    if (loupeStateRef.current.active && !loupeStateRef.current.keyDown) {
      hideLoupeImmediately();
    }
  }, [hideLoupeImmediately]);

  return {
    loupeOverlayRef,
    loupeCanvasRef,
    loupeBadgeRef,
    loupeStateRef,
    handleLoupeWheel,
    activateRightClickLoupe,
    updatePointer,
    releaseRightClickLoupe,
    handleLoupeMouseLeave,
  };
}
