// Расширение Visual Studio Code для языка Дан.
// Подсветку даёт грамматика (syntaxes/dan.tmLanguage.json), всё остальное -
// сервер языка, который запускается командой «python dan.py сервер».
const vscode = require("vscode");
const path = require("path");
const fs = require("fs");
const { LanguageClient } = require("vscode-languageclient/node");

let клиент = null;

function командаPython() {
  const задано = vscode.workspace.getConfiguration().get("дан.python");
  if (задано) return задано;
  return process.platform === "win32" ? "python" : "python3";
}

function путьКомпилятора() {
  const задано = vscode.workspace.getConfiguration().get("дан.компилятор");
  if (задано && fs.existsSync(задано)) return задано;
  for (const папка of vscode.workspace.workspaceFolders || []) {
    const путь = path.join(папка.uri.fsPath, "dan.py");
    if (fs.existsSync(путь)) return путь;
  }
  const кандидаты = [];
  if (process.env.DAN_HOME) кандидаты.push(path.join(process.env.DAN_HOME, "dan.py"));
  if (process.platform === "win32") кандидаты.push("D:\\проекты\\Дан\\dan.py");
  for (const путь of кандидаты) if (fs.existsSync(путь)) return путь;
  return null;
}

function выполнитьВТерминале(команда, параметры) {
  const редактор = vscode.window.activeTextEditor;
  if (!редактор || редактор.document.languageId !== "dan") {
    vscode.window.showWarningMessage("Откройте программу на Дане (файл .dan)");
    return;
  }
  const компилятор = путьКомпилятора();
  if (!компилятор) {
    vscode.window.showErrorMessage("Не найден dan.py: укажите путь к нему в настройке «дан.компилятор»");
    return;
  }
  редактор.document.save().then(() => {
    let терминал = vscode.window.terminals.find((т) => т.name === "Дан");
    if (!терминал) терминал = vscode.window.createTerminal("Дан");
    терминал.show(true);
    const кавычки = (с) => '"' + с + '"';
    const вызов = process.platform === "win32" ? "& " : "";
    терминал.sendText(вызов + кавычки(командаPython()) + " " + кавычки(компилятор) + " " + команда + " " +
                      кавычки(редактор.document.fileName) + (параметры ? " " + параметры : ""));
  });
}

function activate(контекст) {
  контекст.subscriptions.push(
    vscode.commands.registerCommand("dan.run", () => выполнитьВТерминале("запустить")),
    vscode.commands.registerCommand("dan.build", () => выполнитьВТерминале("собрать")),
    vscode.commands.registerCommand("dan.check", () => выполнитьВТерминале("проверить")),
    vscode.commands.registerCommand("dan.debug", () => выполнитьВТерминале("отладка"))
  );
  const компилятор = путьКомпилятора();
  if (!компилятор) {
    vscode.window.showWarningMessage(
      "Дан: не найден dan.py, поэтому проверка ошибок и подсказки выключены. Укажите путь в настройке «дан.компилятор».");
    return;
  }
  клиент = new LanguageClient(
    "dan", "Дан",
    { command: командаPython(), args: [компилятор, "сервер"], options: { cwd: path.dirname(компилятор) } },
    { documentSelector: [{ scheme: "file", language: "dan" }] }
  );
  клиент.start();
}

function deactivate() {
  return клиент ? клиент.stop() : undefined;
}

module.exports = { activate, deactivate };
