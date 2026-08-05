import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';
import { resolveScannerPath, runScanner } from './scannerRunner';
import { ExtensionToWebviewMessage, WebviewToExtensionMessage } from './graphTypes';

export function activate(context: vscode.ExtensionContext) {
  const disposable = vscode.commands.registerCommand('codegraph.visualizeProject', async () => {
    const workspaceFolder = vscode.workspace.workspaceFolders?.[0];
    if (!workspaceFolder) {
      vscode.window.showErrorMessage('CodeGraph: open a folder or workspace first.');
      return;
    }

    const config = vscode.workspace.getConfiguration('codegraph');
    const scannerPath = resolveScannerPath(
      workspaceFolder.uri.fsPath,
      config.get<string>('scannerPath')
    );

    try {
      const graph = await vscode.window.withProgress(
        {
          location: vscode.ProgressLocation.Notification,
          title: 'CodeGraph: scanning project…',
        },
        () => runScanner(scannerPath, workspaceFolder.uri.fsPath)
      );

      const panel = vscode.window.createWebviewPanel(
        'codegraphView',
        `CodeGraph: ${workspaceFolder.name}`,
        vscode.ViewColumn.One,
        {
          enableScripts: true,
          retainContextWhenHidden: true,
          localResourceRoots: [context.extensionUri],
        }
      );

      panel.webview.html = getWebviewHtml(panel.webview, context.extensionUri);

      panel.webview.onDidReceiveMessage((message: WebviewToExtensionMessage) => {
        if (message.type === 'ready') {
          const payload: ExtensionToWebviewMessage = { type: 'graphData', graph };
          panel.webview.postMessage(payload);
        } else if (message.type === 'openFile') {
          const absolutePath = path.join(workspaceFolder.uri.fsPath, message.relativePath);
          vscode.window.showTextDocument(vscode.Uri.file(absolutePath));
        }
      });
    } catch (error) {
      vscode.window.showErrorMessage(`CodeGraph: ${(error as Error).message}`);
    }
  });

  context.subscriptions.push(disposable);
}

export function deactivate() {}

function getWebviewHtml(webview: vscode.Webview, extensionUri: vscode.Uri): string {
  const htmlPath = vscode.Uri.joinPath(extensionUri, 'media', 'index.html');
  const template = fs.readFileSync(htmlPath.fsPath, 'utf8');

  const scriptUri = webview.asWebviewUri(
    vscode.Uri.joinPath(extensionUri, 'dist', 'webview.js')
  );
  const styleUri = webview.asWebviewUri(
    vscode.Uri.joinPath(extensionUri, 'media', 'style.css')
  );

  return template
    .replaceAll('{{cspSource}}', webview.cspSource)
    .replaceAll('{{scriptUri}}', scriptUri.toString())
    .replaceAll('{{styleUri}}', styleUri.toString());
}
