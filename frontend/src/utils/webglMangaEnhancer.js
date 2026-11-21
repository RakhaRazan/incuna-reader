// Lightweight shared WebGL2 Manga Enhancer
// Applies subtle Unsharp Mask ink sharpening + Black-Level Crush / White-Paper Normalization
// using a single shared WebGL2 context so even 200-page vertical webtoons never exhaust contexts.

const VERTEX_SHADER_SRC = `#version 300 es
precision highp float;

in vec2 a_position;
out vec2 v_uv;

void main() {
  v_uv = vec2(a_position.x * 0.5 + 0.5, 0.5 - a_position.y * 0.5);
  gl_Position = vec4(a_position, 0.0, 1.0);
}
`;

const FRAGMENT_SHADER_SRC = `#version 300 es
precision highp float;

uniform sampler2D u_image;
uniform vec2 u_texelSize;
uniform float u_sharpenAmount;
uniform float u_blackLevel;
uniform float u_whiteLevel;

in vec2 v_uv;
out vec4 outColor;

void main() {
  vec4 centerSample = texture(u_image, v_uv);
  vec3 center = centerSample.rgb;

  // 3x3 Gaussian blur kernel for subtle Unsharp Mask ink sharpening
  vec2 t = u_texelSize;
  vec3 blur =
    texture(u_image, v_uv + vec2(-t.x, -t.y)).rgb * 0.0625 +
    texture(u_image, v_uv + vec2( 0.0, -t.y)).rgb * 0.125  +
    texture(u_image, v_uv + vec2( t.x, -t.y)).rgb * 0.0625 +
    texture(u_image, v_uv + vec2(-t.x,  0.0)).rgb * 0.125  +
    center                                        * 0.25   +
    texture(u_image, v_uv + vec2( t.x,  0.0)).rgb * 0.125  +
    texture(u_image, v_uv + vec2(-t.x,  t.y)).rgb * 0.0625 +
    texture(u_image, v_uv + vec2( 0.0,  t.y)).rgb * 0.125  +
    texture(u_image, v_uv + vec2( t.x,  t.y)).rgb * 0.0625;

  // Subtle Unsharp Mask: crisp ink lines without harsh ringing on screentones
  vec3 detail = center - blur;
  vec3 sharpened = clamp(center + detail * u_sharpenAmount, 0.0, 1.0);

  // Black-Level Crush & White-Paper Normalization
  float denom = max(0.001, u_whiteLevel - u_blackLevel);
  vec3 normalized = clamp((sharpened - vec3(u_blackLevel)) / denom, 0.0, 1.0);

  // Gentle midtone contrast preservation
  vec3 enhanced = mix(normalized, smoothstep(0.0, 1.0, normalized), 0.12);

  outColor = vec4(enhanced, centerSample.a);
}
`;

const DEFAULT_OPTIONS = {
  sharpenAmount: 0.65,
  blackLevel: 0.055,
  whiteLevel: 0.945,
};

let sharedState = null;
let webglUnavailable = false;

function compileShader(gl, type, source) {
  const shader = gl.createShader(type);
  if (!shader) return null;
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const log = gl.getShaderInfoLog(shader);
    console.warn('[MangaEnhancer] WebGL2 shader compilation failed:', log);
    gl.deleteShader(shader);
    return null;
  }
  return shader;
}

function createProgram(gl, vsSource, fsSource) {
  const vs = compileShader(gl, gl.VERTEX_SHADER, vsSource);
  if (!vs) return null;
  const fs = compileShader(gl, gl.FRAGMENT_SHADER, fsSource);
  if (!fs) {
    gl.deleteShader(vs);
    return null;
  }

  const program = gl.createProgram();
  if (!program) {
    gl.deleteShader(vs);
    gl.deleteShader(fs);
    return null;
  }

  gl.attachShader(program, vs);
  gl.attachShader(program, fs);
  gl.linkProgram(program);

  gl.deleteShader(vs);
  gl.deleteShader(fs);

  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    const log = gl.getProgramInfoLog(program);
    console.warn('[MangaEnhancer] WebGL2 program link failed:', log);
    gl.deleteProgram(program);
    return null;
  }

  return program;
}

function getSharedRenderer() {
  if (webglUnavailable) return null;
  if (sharedState && sharedState.gl && !sharedState.gl.isContextLost()) {
    return sharedState;
  }

  try {
    const canvas = document.createElement('canvas');
    const gl = canvas.getContext('webgl2', {
      alpha: false,
      antialias: false,
      depth: false,
      stencil: false,
      premultipliedAlpha: false,
      preserveDrawingBuffer: true,
      powerPreference: 'high-performance',
    });

    if (!gl) {
      webglUnavailable = true;
      return null;
    }

    const program = createProgram(gl, VERTEX_SHADER_SRC, FRAGMENT_SHADER_SRC);
    if (!program) {
      webglUnavailable = true;
      return null;
    }

    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);

    const posBuffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, posBuffer);
    gl.bufferData(
      gl.ARRAY_BUFFER,
      new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]),
      gl.STATIC_DRAW
    );

    const posLoc = gl.getAttribLocation(program, 'a_position');
    gl.enableVertexAttribArray(posLoc);
    gl.vertexAttribPointer(posLoc, 2, gl.FLOAT, false, 0, 0);

    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);

    const uniforms = {
      u_image: gl.getUniformLocation(program, 'u_image'),
      u_texelSize: gl.getUniformLocation(program, 'u_texelSize'),
      u_sharpenAmount: gl.getUniformLocation(program, 'u_sharpenAmount'),
      u_blackLevel: gl.getUniformLocation(program, 'u_blackLevel'),
      u_whiteLevel: gl.getUniformLocation(program, 'u_whiteLevel'),
    };

    const maxTextureSize = gl.getParameter(gl.MAX_TEXTURE_SIZE) || 4096;

    canvas.addEventListener('webglcontextlost', (e) => {
      e.preventDefault();
      sharedState = null;
    });

    sharedState = {
      canvas,
      gl,
      program,
      vao,
      posBuffer,
      texture,
      uniforms,
      maxTextureSize,
    };
    return sharedState;
  } catch (err) {
    console.warn('[MangaEnhancer] WebGL2 initialization error:', err);
    webglUnavailable = true;
    return null;
  }
}

export function isWebGL2Available() {
  return Boolean(getSharedRenderer());
}

/**
 * Renders an HTMLImageElement through the WebGL2 manga enhancer shader
 * and blits the result onto targetCanvas.
 * Returns true on success, false if WebGL2 is unavailable or rendering failed.
 */
export function renderEnhancedMangaToCanvas(img, targetCanvas, options = DEFAULT_OPTIONS) {
  if (!img || !img.naturalWidth || !img.naturalHeight || !targetCanvas) {
    return false;
  }

  const renderer = getSharedRenderer();
  if (!renderer) return false;

  const { canvas: glCanvas, gl, program, vao, texture, uniforms, maxTextureSize } = renderer;

  try {
    let w = img.naturalWidth;
    let h = img.naturalHeight;

    // Respect GPU MAX_TEXTURE_SIZE for ultra-tall webtoon strips
    if (w > maxTextureSize || h > maxTextureSize) {
      const scale = Math.min(maxTextureSize / w, maxTextureSize / h);
      w = Math.max(1, Math.floor(w * scale));
      h = Math.max(1, Math.floor(h * scale));
    }

    if (glCanvas.width !== w || glCanvas.height !== h) {
      glCanvas.width = w;
      glCanvas.height = h;
    }

    gl.viewport(0, 0, w, h);
    gl.useProgram(program);
    gl.bindVertexArray(vao);

    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, img);

    gl.uniform1i(uniforms.u_image, 0);
    gl.uniform2f(uniforms.u_texelSize, 1.0 / w, 1.0 / h);
    gl.uniform1f(uniforms.u_sharpenAmount, options.sharpenAmount ?? DEFAULT_OPTIONS.sharpenAmount);
    gl.uniform1f(uniforms.u_blackLevel, options.blackLevel ?? DEFAULT_OPTIONS.blackLevel);
    gl.uniform1f(uniforms.u_whiteLevel, options.whiteLevel ?? DEFAULT_OPTIONS.whiteLevel);

    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);

    if (targetCanvas.width !== w || targetCanvas.height !== h) {
      targetCanvas.width = w;
      targetCanvas.height = h;
    }

    const ctx2d = targetCanvas.getContext('2d', { alpha: false });
    if (!ctx2d) return false;
    ctx2d.drawImage(glCanvas, 0, 0, w, h);
    return true;
  } catch (err) {
    console.warn('[MangaEnhancer] WebGL2 render failed, falling back to <img>:', err);
    return false;
  }
}
