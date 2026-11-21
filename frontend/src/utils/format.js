export function formatBytes(bytes) {
  if (!bytes || bytes <= 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  const idx = Math.min(units.length - 1, Math.floor(Math.log(bytes) / Math.log(1024)));
  const value = bytes / Math.pow(1024, idx);
  return `${value >= 10 || idx === 0 ? value.toFixed(0) : value.toFixed(1)} ${units[idx]}`;
}

export function formatDisplayTitle(rawTitle, smartRename = false) {
  if (!rawTitle || !smartRename) return rawTitle || '';
  const cleaned = String(rawTitle)
    .replace(/\.(cbz|cbr|cb7|cbt|epub|pdf|zip|rar|7z)$/i, '')
    .replace(/\[[^\]]*\]/g, ' ')
    .replace(/\(\s*\d{4}(?:\s*-\s*\d{4})?\s*\)/g, ' ')
    .replace(/_/g, ' ')
    .replace(/\s+/g, ' ')
    .trim();
  return cleaned || rawTitle;
}
