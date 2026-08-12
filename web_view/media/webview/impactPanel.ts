import { GraphNode } from '../../src/graphTypes';

let clearCallback: (() => void) | null = null;

function renderPlaceholder(container: HTMLElement): void {
  container.innerHTML = '';
  const placeholder = document.createElement('div');
  placeholder.className = 'impact-placeholder';
  placeholder.textContent = 'Click a node to see what depends on it (via calls/reads/writes).';
  container.appendChild(placeholder);
}

export function renderImpactPanel(onClear: () => void): void {
  clearCallback = onClear;
  const container = document.getElementById('impact-panel');
  if (!container) {
    return;
  }
  renderPlaceholder(container);
}

export function showImpact(startNode: GraphNode, affectedNodes: GraphNode[]): void {
  const container = document.getElementById('impact-panel');
  if (!container) {
    return;
  }
  container.innerHTML = '';

  const header = document.createElement('div');
  header.className = 'impact-header';
  header.textContent = `Impact of "${startNode.name}" (${startNode.type})`;
  container.appendChild(header);

  const clearButton = document.createElement('button');
  clearButton.className = 'impact-clear-button';
  clearButton.textContent = 'Clear';
  clearButton.addEventListener('click', () => clearCallback?.());
  container.appendChild(clearButton);

  const summary = document.createElement('div');
  summary.className = 'impact-summary';
  if (affectedNodes.length === 0) {
    summary.textContent = 'Nothing depends on this (via calls/reads/writes).';
  } else {
    const counts = new Map<string, number>();
    for (const node of affectedNodes) {
      counts.set(node.type, (counts.get(node.type) ?? 0) + 1);
    }
    const parts = [...counts.entries()].map(
      ([type, count]) => `${count} ${type}${count === 1 ? '' : 's'}`
    );
    summary.textContent = `${affectedNodes.length} affected: ${parts.join(', ')}`;
  }
  container.appendChild(summary);

  const list = document.createElement('div');
  list.className = 'impact-list';
  for (const node of affectedNodes) {
    const row = document.createElement('div');
    row.className = 'impact-row';
    row.textContent = `[${node.type}] ${node.name}`;
    list.appendChild(row);
  }
  container.appendChild(list);
}

export function clearImpactPanel(): void {
  const container = document.getElementById('impact-panel');
  if (!container) {
    return;
  }
  renderPlaceholder(container);
}
