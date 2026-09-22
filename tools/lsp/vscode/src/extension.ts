import { workspace, type ExtensionContext } from 'vscode';

import {
  LanguageClient,
  TransportKind,
  type LanguageClientOptions,
  type ServerOptions,
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;

export function activate(context: ExtensionContext): void {
  // The server bundle rides inside the extension (server/server.js, copied
  // from rho-lsp/dist by `npm run build:vscode` at the package root).
  const serverModule = context.asAbsolutePath('server/server.js');

  const config = workspace.getConfiguration('rho');
  const compilerWasm = config.get<string>('compilerWasm', '').trim();

  const serverOptions: ServerOptions = {
    run: {
      module: serverModule,
      transport: TransportKind.stdio,
      options: compilerWasm ? { env: { ...process.env, RHO_LSP_WASM: compilerWasm } } : undefined,
    },
    debug: {
      module: serverModule,
      transport: TransportKind.stdio,
      options: { execArgv: ['--nolazy', '--inspect=6011'] },
    },
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [
      { language: 'rho', scheme: 'file' },
      { language: 'rho', scheme: 'untitled' },
    ],
    outputChannelName: 'rho language server',
    // Default restart policy applies: if the server ever dies, the client
    // restarts it; the server itself contains compiler failures.
  };

  client = new LanguageClient('rho-lsp', 'rho language server', serverOptions, clientOptions);
  void client.start();
}

export async function deactivate(): Promise<void> {
  if (client) {
    await client.stop();
    client = undefined;
  }
}
