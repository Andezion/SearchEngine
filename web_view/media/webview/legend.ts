import { LEGEND_ENTRIES, EDGE_LEGEND_ENTRIES, LegendEntry } from './nodeStyles';

function appendLegendRows(container: HTMLElement, entries: LegendEntry[]): void {
  for (const entry of entries) {
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

export function renderLegend(): void {
  const container = document.getElementById('legend');
  if (!container) {
    return;
  }

  appendLegendRows(container, LEGEND_ENTRIES);

  const divider = document.createElement('div');
  divider.className = 'legend-divider';
  container.appendChild(divider);

  appendLegendRows(container, EDGE_LEGEND_ENTRIES);
}
