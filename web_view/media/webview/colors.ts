const EXTENSION_COLORS: Record<string, string> = {
  '.c': '#5590d9',
  '.h': '#7aa6d9',
  '.cpp': '#e0723c',
  '.cc': '#e0723c',
  '.hpp': '#e69a6c',
  '.rs': '#dd8e46',
  '.go': '#4dc0b5',
  '.ts': '#3178c6',
  '.tsx': '#3178c6',
  '.js': '#e0c341',
  '.jsx': '#e0c341',
  '.py': '#3f7fb3',
  '.json': '#9a9a9a',
  '.md': '#8fb35e',
  '': '#8a8a8a',
};


function hashColor(key: string): string {
  let hash = 0;
  for (let i = 0; i < key.length; i++) {
    hash = (hash << 5) - hash + key.charCodeAt(i);
    hash |= 0;
  }
  const hue = Math.abs(hash) % 360;
  return `hsl(${hue}, 55%, 55%)`;
}

export function getColorForExtension(extension: string | undefined): string {
  const ext = extension ?? '';
  return EXTENSION_COLORS[ext] ?? hashColor(ext);
}
