import { Graph } from '../../src/graphTypes';

const IMPACT_EDGE_TYPES = new Set(['calls', 'reads', 'writes']);

interface ReverseEdgeRef {
  sourceId: string;
  edgeId: string;
}

export type ReverseAdjacency = Map<string, ReverseEdgeRef[]>;

export function buildReverseAdjacency(graph: Graph): ReverseAdjacency {
  const adjacency: ReverseAdjacency = new Map();
  for (const edge of graph.edges) {
    if (!IMPACT_EDGE_TYPES.has(edge.type)) {
      continue;
    }
    const list = adjacency.get(edge.target) ?? [];
    list.push({ sourceId: edge.source, edgeId: edge.id });
    adjacency.set(edge.target, list);
  }
  return adjacency;
}

export interface ImpactResult {
  affectedNodeIds: string[];
  traversedEdgeIds: string[];
}

export function computeImpact(startId: string, adjacency: ReverseAdjacency): ImpactResult {
  const visitedNodes = new Set<string>([startId]);
  const traversedEdges = new Set<string>();
  const queue: string[] = [startId];

  while (queue.length > 0) {
    const current = queue.shift() as string;
    for (const { sourceId, edgeId } of adjacency.get(current) ?? []) {
      traversedEdges.add(edgeId);
      if (!visitedNodes.has(sourceId)) {
        visitedNodes.add(sourceId);
        queue.push(sourceId);
      }
    }
  }

  visitedNodes.delete(startId);
  return { affectedNodeIds: [...visitedNodes], traversedEdgeIds: [...traversedEdges] };
}
