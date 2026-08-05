export interface GraphNode {
  id: string;
  type:
    | 'project'
    | 'directory'
    | 'file'
    | 'struct'
    | 'class'
    | 'union'
    | 'enum'
    | 'field'
    | 'constant';
  name: string;
  file: string | null;
  language: string | null;
  line_start: number | null;
  line_end: number | null;
  metadata: Record<string, unknown>;
}

export interface GraphEdge {
  id: string;
  type: 'contains' | 'defines';
  source: string;
  target: string;
}

export interface Graph {
  version: string;
  root: string;
  nodes: GraphNode[];
  edges: GraphEdge[];
}

export type ExtensionToWebviewMessage = { type: 'graphData'; graph: Graph };

export type WebviewToExtensionMessage =
  | { type: 'ready' }
  | { type: 'openFile'; relativePath: string };
