import type { ElementDefinition } from 'cytoscape';
import { Graph } from '../../src/graphTypes';

export function graphToElements(graph: Graph): ElementDefinition[] {
  const parentByChildId = new Map<string, string>();
  for (const edge of graph.edges) {
    if (edge.type === 'contains' || edge.type === 'defines') {
      parentByChildId.set(edge.target, edge.source);
    }
  }

  const nodeElements: ElementDefinition[] = graph.nodes.map((node) => ({
    data: {
      id: node.id,
      type: node.type,
      name: node.name,
      file: node.file,
      parent: parentByChildId.get(node.id),
      ...node.metadata,
    },
  }));

  const edgeElements: ElementDefinition[] = graph.edges.map((edge) => ({
    data: {
      id: edge.id,
      type: edge.type,
      source: edge.source,
      target: edge.target,
    },
  }));

  return [...nodeElements, ...edgeElements];
}
