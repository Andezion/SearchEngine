import cytoscape from 'cytoscape';
import { graphToElements } from './graphToElements';
import { getColorForExtension } from './colors';
import {
  ExtensionToWebviewMessage,
  WebviewToExtensionMessage,
  Graph,
} from '../../src/graphTypes';

const vscodeApi = acquireVsCodeApi();

function renderGraph(graph: Graph): void {
  const container = document.getElementById('cy');
  if (!container) {
    return;
  }

  cytoscape({
    container,
    elements: graphToElements(graph),
    layout: { name: 'cose', animate: false },
    style: [
      {
        selector: 'node[type="project"], node[type="directory"]',
        style: {
          'background-color': '#2b2b2b',
          'background-opacity': 0.15,
          'border-width': 1,
          'border-color': '#888',
          label: 'data(name)',
          'text-valign': 'top',
          'font-size': 11,
          color: '#ccc',
        },
      },
      {
        selector: 'node[type="file"]',
        style: {
          label: 'data(name)',
          'font-size': 9,
          width: 18,
          height: 18,
          color: '#ddd',
          'text-valign': 'bottom',
          'background-color': (ele: cytoscape.NodeSingular) =>
            getColorForExtension(ele.data('extension')),
        },
      },
      {
        selector: 'node[type="struct"], node[type="class"], node[type="union"], node[type="enum"]',
        style: {
          shape: 'round-rectangle',
          'background-color': '#3a3a5c',
          'background-opacity': 0.25,
          'border-width': 1,
          'border-color': '#8888cc',
          label: 'data(name)',
          'font-size': 9,
          color: '#ddd',
          'text-valign': 'top',
        },
      },
      {
        selector: 'node[type="field"], node[type="constant"]',
        style: {
          shape: 'ellipse',
          width: 6,
          height: 6,
          'background-color': '#cccccc',
          label: 'data(name)',
          'font-size': 7,
          color: '#aaa',
          'text-valign': 'bottom',
        },
      },
      {
        selector: 'edge',
        style: {
          width: 1,
          'line-color': '#666',
          'target-arrow-shape': 'none',
          'curve-style': 'bezier',
        },
      },
    ],
  }).on('dblclick', 'node[type="file"]', (event) => {
    const relativePath = event.target.data('file');
    if (relativePath) {
      const message: WebviewToExtensionMessage = { type: 'openFile', relativePath };
      vscodeApi.postMessage(message);
    }
  });
}

window.addEventListener('message', (event: MessageEvent<ExtensionToWebviewMessage>) => {
  if (event.data.type === 'graphData') {
    renderGraph(event.data.graph);
  }
});

const readyMessage: WebviewToExtensionMessage = { type: 'ready' };
vscodeApi.postMessage(readyMessage);
