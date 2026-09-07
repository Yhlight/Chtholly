const vscode = require("vscode");
const { LanguageClient, TransportKind } = require("vscode-languageclient/node");

let client;

function workspaceFolder() {
  const document = vscode.window.activeTextEditor?.document;
  if (document) {
    const folder = vscode.workspace.getWorkspaceFolder(document.uri);
    if (folder) {
      return folder;
    }
  }
  return vscode.workspace.workspaceFolders?.[0];
}

async function executeCompiler(action) {
  const folder = workspaceFolder();
  if (!folder && action !== "doctor") {
    vscode.window.showErrorMessage(
      `Chtholly ${action} requires an open project or workspace.`
    );
    return;
  }
  const command = vscode.workspace
    .getConfiguration("chtholly")
    .get("compiler.path", "chthollyc");
  const options = folder ? { cwd: folder.uri.fsPath } : undefined;
  const execution = new vscode.ProcessExecution(command, [action], options);
  const scope = folder || vscode.TaskScope.Workspace;
  const task = new vscode.Task(
    { type: "chtholly", action },
    scope,
    `Chtholly: ${action}`,
    "chtholly",
    execution,
    ["$chtholly"]
  );
  if (action === "build" || action === "check") {
    task.group = vscode.TaskGroup.Build;
  }
  task.presentationOptions = {
    reveal: vscode.TaskRevealKind.Always,
    panel: vscode.TaskPanelKind.Shared,
    clear: true
  };
  await vscode.tasks.executeTask(task);
}

function activate(context) {
  const command = vscode.workspace
    .getConfiguration("chtholly")
    .get("server.path", "chtholly-lsp");
  const server = { command, transport: TransportKind.stdio };
  client = new LanguageClient(
    "chtholly",
    "Chtholly Language Server",
    { run: server, debug: server },
    {
      documentSelector: [
        { scheme: "file", language: "chtholly" },
        { scheme: "file", language: "cfdl" }
      ],
      synchronize: {
        fileEvents: vscode.workspace.createFileSystemWatcher(
          "**/{chtholly.toml,chtholly.workspace.toml}"
        )
      }
    }
  );
  client.start();
  for (const action of ["check", "build", "run", "doctor"]) {
    context.subscriptions.push(
      vscode.commands.registerCommand(
        `chtholly.${action}`,
        () => executeCompiler(action)
      )
    );
  }
  context.subscriptions.push({
    dispose: () => {
      if (client) {
        client.stop();
      }
    }
  });
}

async function deactivate() {
  if (client) {
    await client.stop();
  }
}

module.exports = { activate, deactivate };
