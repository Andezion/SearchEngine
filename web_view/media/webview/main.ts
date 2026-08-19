import cytoscape from 'cytoscape';
import fcose from 'cytoscape-fcose';
import { graphToElements } from './graphToElements';
import { getColorForExtension, getColorForLanguage, getColorForType } from './colors';
import { renderLegend } from './legend';
import { buildReverseAdjacency, computeImpact } from './impactAnalysis';
import { renderImpactPanel, showImpact, clearImpactPanel } from './impactPanel';
import {
  ExtensionToWebviewMessage,
  WebviewToExtensionMessage,
  Graph,
  GraphNode,
} from '../../src/graphTypes';

cytoscape.use(fcose);

const vscodeApi = acquireVsCodeApi();

function renderGraph(graph: Graph): void {
  const container = document.getElementById('cy');
  if (!container) {
    return;
  }

  const nodeById = new Map<string, GraphNode>(graph.nodes.map((node) => [node.id, node]));
  const reverseAdjacency = buildReverseAdjacency(graph);

  const cy = cytoscape({
    container,
    elements: graphToElements(graph),
    layout: {
      name: 'fcose',
      quality: 'default',
      randomize: true,
      animate: false,
      fit: true,
      padding: 30,
      nodeSeparation: 90,
      idealEdgeLength: () => 60,
      nodeRepulsion: () => 4500,
      packComponents: false,
    } as cytoscape.LayoutOptions,
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
        selector: 'node[type="file"]:childless',
        style: {
          label: 'data(name)',
          'font-size': 10,
          width: 18,
          height: 18,
          color: '#ddd',
          'text-valign': 'bottom',
          'background-color': (ele: cytoscape.NodeSingular) =>
            getColorForExtension(ele.data('extension')),
        },
      },
      {
        selector: 'node[type="file"]:parent',
        style: {
          label: 'data(name)',
          'font-size': 10,
          'text-valign': 'top',
          'text-margin-y': -4,
          color: '#ddd',
          'background-color': (ele: cytoscape.NodeSingular) =>
            getColorForExtension(ele.data('extension')),
          'background-opacity': 0.15,
          'border-width': 1,
          'border-color': '#888',
        },
      },
      {
        selector: 'node[type="namespace"]',
        style: {
          shape: 'rectangle',
          'background-color': '#3a5c46',
          'background-opacity': 0.2,
          'border-width': 1,
          'border-style': 'dashed',
          'border-color': '#6cbf8c',
          label: 'data(name)',
          'font-size': 9,
          color: '#ddd',
          'text-valign': 'top',
        },
      },
      {
        selector:
          'node[type="struct"], node[type="class"], node[type="union"], node[type="enum"], node[type="interface"]',
        style: {
          shape: 'round-rectangle',
          'background-opacity': 0.25,
          'border-width': 1.5,
          label: 'data(name)',
          'font-size': 10,
          color: '#ddd',
          'text-valign': 'top',
          'background-color': (ele: cytoscape.NodeSingular) => getColorForType(ele.data('type')),
          'border-color': (ele: cytoscape.NodeSingular) => getColorForLanguage(ele.data('language')),
        },
      },
      {
        selector: 'node[type="class"]',
        style: { 'border-width': 2.5 },
      },
      {
        selector: 'node[type="union"]',
        style: { 'border-style': 'dashed' },
      },
      {
        selector: 'node[type="enum"]',
        style: { 'border-style': 'dotted' },
      },
      {
        selector: 'node[type="interface"]',
        style: { 'border-style': 'double', 'border-width': 4 },
      },
      {
        selector: 'node[type="function"], node[type="method"]',
        style: {
          shape: 'diamond',
          width: 10,
          height: 10,
          label: 'data(name)',
          'font-size': 7,
          color: '#ddd',
          'text-valign': 'bottom',
          'background-color': (ele: cytoscape.NodeSingular) => getColorForType(ele.data('type')),
          'border-width': 1.5,
          'border-color': (ele: cytoscape.NodeSingular) => getColorForLanguage(ele.data('language')),
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
      {
        selector: 'edge[type="calls"]',
        style: {
          'line-color': '#c97b4a',
          'target-arrow-color': '#c97b4a',
          'target-arrow-shape': 'triangle',
          'line-style': 'solid',
          width: 1.2,
          'curve-style': 'bezier',
        },
      },
      {
        selector: 'edge[type="imports"]',
        style: {
          'line-color': '#5a8fbf',
          'target-arrow-color': '#5a8fbf',
          'target-arrow-shape': 'triangle',
          'line-style': 'dashed',
          width: 1,
          'curve-style': 'bezier',
        },
      },
      {
        selector: 'edge[type="writes"]',
        style: {
          'line-color': '#c94a6b',
          'target-arrow-color': '#c94a6b',
          'target-arrow-shape': 'triangle',
          'line-style': 'solid',
          width: 1.2,
          'curve-style': 'bezier',
        },
      },
      {
        selector: 'edge[type="reads"]',
        style: {
          'line-color': '#4ac9a0',
          'target-arrow-color': '#4ac9a0',
          'target-arrow-shape': 'triangle',
          'line-style': 'dotted',
          width: 1,
          'curve-style': 'bezier',
        },
      },
      {
        selector: 'node.impact-selected',
        style: {
          'border-width': 3,
          'border-color': '#e0c341',
          'overlay-color': '#e0c341',
          'overlay-opacity': 0.25,
          'overlay-padding': 4,
        },
      },
      {
        selector: 'node.impact-affected',
        style: {
          'border-width': 2,
          'border-color': '#e0724a',
          'overlay-color': '#e0724a',
          'overlay-opacity': 0.15,
        },
      },
      {
        selector: 'edge.impact-affected-edge',
        style: {
          'line-color': '#e0724a',
          'target-arrow-color': '#e0724a',
          width: 2.5,
        },
      },
    ],
  });

  function clearImpact(): void {
    cy.elements().removeClass('impact-selected impact-affected impact-affected-edge');
    clearImpactPanel();
  }

  renderImpactPanel(clearImpact);

  cy.on('dblclick', 'node[type="file"]', (event) => {
    const relativePath = event.target.data('file');
    if (relativePath) {
      const message: WebviewToExtensionMessage = { type: 'openFile', relativePath };
      vscodeApi.postMessage(message);
    }
  });

  cy.on('tap', 'node', (event) => {
    const startId = event.target.id();
    const startNode = nodeById.get(startId);
    if (!startNode) {
      return;
    }

    const { affectedNodeIds, traversedEdgeIds } = computeImpact(startId, reverseAdjacency);
    const affectedNodes = affectedNodeIds
      .map((id) => nodeById.get(id))
      .filter((node): node is GraphNode => node !== undefined);

    clearImpact();
    cy.getElementById(startId).addClass('impact-selected');
    for (const id of affectedNodeIds) {
      cy.getElementById(id).addClass('impact-affected');
    }
    for (const id of traversedEdgeIds) {
      cy.getElementById(id).addClass('impact-affected-edge');
    }

    showImpact(startNode, affectedNodes);
  });

  cy.on('tap', (event) => {
    if (event.target === cy) {
      clearImpact();
    }
  });
}

window.addEventListener('message', (event: MessageEvent<ExtensionToWebviewMessage>) => {
  if (event.data.type === 'graphData') {
    renderGraph(event.data.graph);
  }
});

renderLegend();

const readyMessage: WebviewToExtensionMessage = { type: 'ready' };
vscodeApi.postMessage(readyMessage);
