import { execFile } from 'child_process';
import * as path from 'path';
import { Graph } from './graphTypes';

export function resolveScannerPath(
  workspaceFolder: string,
  configuredPath: string | undefined
): string {
  if (configuredPath && configuredPath.trim().length > 0) {
    return path.isAbsolute(configuredPath)
      ? configuredPath
      : path.join(workspaceFolder, configuredPath);
  }
  return path.join(
    workspaceFolder,
    'build',
    'native_analysis_engine',
    'project_scanner',
    'codegraph_scanner'
  );
}

export function runScanner(scannerPath: string, projectRoot: string): Promise<Graph> {
  return new Promise((resolve, reject) => {
    execFile(
      scannerPath,
      [projectRoot],
      { maxBuffer: 20 * 1024 * 1024 },
      (error, stdout, stderr) => {
        if (error) {
          const code = (error as NodeJS.ErrnoException).code;
          if (code === 'ENOENT') {
            reject(
              new Error(
                `Scanner executable not found at "${scannerPath}". Build it (cmake -S . -B build && cmake --build build) or set the "codegraph.scannerPath" setting.`
              )
            );
            return;
          }
          reject(
            new Error(
              `Scanner exited with an error.${stderr ? ` stderr: ${stderr.trim()}` : ` ${error.message}`}`
            )
          );
          return;
        }

        try {
          resolve(JSON.parse(stdout) as Graph);
        } catch {
          reject(new Error('Scanner produced invalid JSON output.'));
        }
      }
    );
  });
}
