import { LEGEND_ENTRIES } from './nodeStyles';

export function renderLegend(): void {
  const container = document.getElementById('legend');
  if (!container) {
    return;
  }

  for (const entry of LEGEND_ENTRIES) {
    const row = document.createElement('div');
    row.className = 'legend-row';

    const swatch = document.createElement('span');
    swatch.className = 'legend-swatch';
    swatch.style.backgroundColor = entry.color;

    const label = document.createElement('span');
    label.textContent = entry.note ? `${entry.label} (${entry.note})` : entry.label;

    row.appendChild(swatch);
    row.appendChild(label);
    container.appendChild(row);
  }
}
