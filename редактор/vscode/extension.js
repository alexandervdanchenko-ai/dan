// Расширение Visual Studio Code для языка Дан.
// Подсветку даёт грамматика (syntaxes/dan.tmLanguage.json), всё остальное -
// сервер языка компилятора на Дане: «dan сервер» (установленный Дан или semya/dan),
// а если найден только dan.py - «python dan.py сервер» (он передаёт работу компилятору на Дане).
const vscode = require("vscode");
const path = require("path");
const fs = require("fs");
const { LanguageClient } = require("vscode-languageclient/node");

let клиент = null;
const WINDOWS = process.platform === "win32";
const ИМЯ_DAN = WINDOWS ? "dan.exe" : "dan";

function командаPython() {
  const задано = vscode.workspace.getConfiguration().get("дан.python");
  if (задано) return задано;
  return WINDOWS ? "python" : "python3";
}

// Компилятор: { путь, python, дом } - исполняемый файл Дана или dan.py (тогда запускается через Python);
// дом - папка Дана для переменной DAN_HOME (у semya/dan - папка проекта).
function компилятор(путь, дом) {
  return { путь, python: путь.toLowerCase().endsWith(".py"), дом: дом || null };
}

function вПапке(папка) {
  // компилятор на Дане из семени (semya/dan), затем dan.py проекта
  const семя = path.join(папка, "semya", WINDOWS ? "dan.exe" : "dan");
  if (fs.existsSync(семя)) return компилятор(семя, папка);
  const свой = path.join(папка, ИМЯ_DAN);
  if (fs.existsSync(свой) && fs.existsSync(path.join(папка, "sreda"))) return компилятор(свой);
  const py = path.join(папка, "dan.py");
  if (fs.existsSync(py)) return компилятор(py);
  return null;
}

function найтиКомпилятор() {
  const задано = vscode.workspace.getConfiguration().get("дан.компилятор");
  if (задано && fs.existsSync(задано)) return компилятор(задано);
  for (const папка of vscode.workspace.workspaceFolders || []) {
    const найден = вПапке(папка.uri.fsPath);
    if (найден) return найден;
  }
  const папки = [];
  if (process.env.DAN_HOME) папки.push(process.env.DAN_HOME);
  if (WINDOWS) {
    if (process.env.LOCALAPPDATA) папки.push(path.join(process.env.LOCALAPPDATA, "Programs", "Dan"));
    папки.push("D:\\проекты\\Дан");
  }
  for (const папка of папки) {
    const найден = вПапке(папка);
    if (найден) return найден;
  }
  // установленный Дан в PATH
  for (const папка of (process.env.PATH || "").split(path.delimiter)) {
    if (папка && fs.existsSync(path.join(папка, ИМЯ_DAN))) return компилятор(path.join(папка, ИМЯ_DAN));
  }
  return null;
}

function выполнитьВТерминале(команда, параметры) {
  const редактор = vscode.window.activeTextEditor;
  if (!редактор || редактор.document.languageId !== "dan") {
    vscode.window.showWarningMessage("Откройте программу на Дане (файл .dan)");
    return;
  }
  const найден = найтиКомпилятор();
  if (!найден) {
    vscode.window.showErrorMessage("Не найден Дан: укажите путь к dan.exe (или dan.py) в настройке «дан.компилятор»");
    return;
  }
  редактор.document.save().then(() => {
    let терминал = vscode.window.terminals.find((т) => т.name === "Дан");
    // semya/dan берёт папки среды и библиотеки из папки проекта (DAN_HOME)
    if (!терминал) терминал = vscode.window.createTerminal({ name: "Дан", env: найден.дом ? { DAN_HOME: найден.дом } : {} });
    терминал.show(true);
    const кавычки = (с) => '"' + с + '"';
    const вызов = process.platform === "win32" ? "& " : "";
    const запуск = найден.python ? кавычки(командаPython()) + " " + кавычки(найден.путь) : кавычки(найден.путь);
    терминал.sendText(вызов + запуск + " " + команда + " " + кавычки(редактор.document.fileName) +
                      (параметры ? " " + параметры : ""));
  });
}

function activate(контекст) {
  контекст.subscriptions.push(
    vscode.commands.registerCommand("dan.run", () => выполнитьВТерминале("запустить")),
    vscode.commands.registerCommand("dan.build", () => выполнитьВТерминале("собрать")),
    vscode.commands.registerCommand("dan.check", () => выполнитьВТерминале("проверить")),
    vscode.commands.registerCommand("dan.debug", () => выполнитьВТерминале("отладка"))
  );
  const найден = найтиКомпилятор();
  if (!найден) {
    vscode.window.showWarningMessage(
      "Дан: не найден компилятор, поэтому проверка ошибок и подсказки выключены. Укажите путь к dan.exe (или dan.py) " +
      "в настройке «дан.компилятор».");
    return;
  }
  const папка = найден.дом || path.dirname(найден.путь);
  const настройки = { cwd: папка };
  if (найден.дом) настройки.env = Object.assign({}, process.env, { DAN_HOME: найден.дом });
  const сервер = найден.python
    ? { command: командаPython(), args: [найден.путь, "сервер"], options: настройки }
    : { command: найден.путь, args: ["сервер"], options: настройки };
  клиент = new LanguageClient("dan", "Дан", сервер, { documentSelector: [{ scheme: "file", language: "dan" }] });
  клиент.start();
}

function deactivate() {
  return клиент ? клиент.stop() : undefined;
}

module.exports = { activate, deactivate };
