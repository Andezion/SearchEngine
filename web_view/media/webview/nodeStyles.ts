import { getColorForExtension, getColorForLanguage } from './colors';

export interface LegendEntry {
  type: string;
  label: string;
  color: string;
  shape: string;
  note?: string;
}


export const LEGEND_ENTRIES: LegendEntry[] = [
  { type: 'project/directory', label: 'Project / Directory', color: '#2b2b2b', shape: 'rectangle' },
  { type: 'file', label: 'File', color: getColorForExtension('.cpp'), shape: 'ellipse', note: 'colored by extension' },
  { type: 'namespace', label: 'Namespace', color: '#3a5c46', shape: 'rectangle' },
  { type: 'struct', label: 'Struct', color: getColorForLanguage('C++'), shape: 'round-rectangle' },
  { type: 'class', label: 'Class', color: getColorForLanguage('C++'), shape: 'round-rectangle' },
  { type: 'union', label: 'Union', color: getColorForLanguage('C++'), shape: 'round-rectangle (dashed)' },
  { type: 'enum', label: 'Enum', color: getColorForLanguage('C++'), shape: 'round-rectangle (dotted)' },
  { type: 'function/method', label: 'Function / Method', color: getColorForLanguage('C++'), shape: 'diamond' },
  { type: 'field/constant', label: 'Field / Constant', color: '#cccccc', shape: 'ellipse (small)' },
];

export const EDGE_LEGEND_ENTRIES: LegendEntry[] = [
  { type: 'calls', label: 'Calls', color: '#c97b4a', shape: 'solid line' },
  { type: 'imports', label: 'Imports', color: '#5a8fbf', shape: 'dashed line' },
  { type: 'writes', label: 'Writes', color: '#c94a6b', shape: 'solid line' },
  { type: 'reads', label: 'Reads', color: '#4ac9a0', shape: 'dotted line' },
];
