# Установка языка Дан на компьютер с Windows.
# Создаёт папку D:\проекты\Дан, устанавливает Python, компилятор C (WinLibs),
# упаковщик UPX и Git, загружает проект из GitHub и проверяет его тестами.
#
# Запуск (в обычном окне PowerShell):
#   powershell -ExecutionPolicy Bypass -File установить_windows.ps1

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$Папка = "D:\проекты\Дан"
$Репозиторий = "https://github.com/alexandervdanchenko-ai/dan.git"

function Шаг($текст) { Write-Host ""; Write-Host "==> $текст" -ForegroundColor Cyan }

function ОбновитьPath {
    $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
                [Environment]::GetEnvironmentVariable("Path", "User")
}

function Установить($команда, $пакет, $название) {
    if (Get-Command $команда -ErrorAction SilentlyContinue) {
        Write-Host "$название уже установлен"
        return
    }
    Write-Host "Устанавливаю $название..."
    winget install --id $пакет -e --silent --accept-package-agreements --accept-source-agreements
    ОбновитьPath
    if (-not (Get-Command $команда -ErrorAction SilentlyContinue)) {
        Write-Host "$название установлен, но пока не виден. Закройте окно PowerShell, откройте новое и запустите сценарий ещё раз." -ForegroundColor Yellow
        exit 1
    }
}

Шаг "Проверяю диск D"
if (-not (Test-Path "D:\")) {
    Write-Host "Диск D не найден. Подключите диск или измените путь в строке `$Папка в начале сценария." -ForegroundColor Red
    exit 1
}

Шаг "Проверяю winget"
if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Write-Host "Не найден winget. Установите «Установщик приложений» (App Installer) из Microsoft Store и запустите сценарий снова." -ForegroundColor Red
    exit 1
}

Шаг "Устанавливаю необходимые программы"
Установить "git" "Git.Git" "Git"
Установить "python" "Python.Python.3.12" "Python"
Установить "gcc" "BrechtSanders.WinLibs.POSIX.UCRT" "компилятор C (WinLibs)"
Установить "upx" "UPX.UPX" "упаковщик UPX"

Шаг "Создаю папку $Папка"
New-Item -ItemType Directory -Force -Path "D:\проекты" | Out-Null

Шаг "Загружаю проект"
if (Test-Path (Join-Path $Папка ".git")) {
    git -C $Папка pull
} elseif ((Test-Path $Папка) -and (Get-ChildItem $Папка -Force | Select-Object -First 1)) {
    Write-Host "Папка $Папка уже существует и не пуста. Переименуйте или очистите её и запустите сценарий снова." -ForegroundColor Red
    exit 1
} else {
    git clone $Репозиторий $Папка
}

Шаг "Устанавливаю дополнительные инструменты (WebAssembly, сборка для других систем)"
python -m pip install --upgrade --quiet ziglang wasmtime
if ($LASTEXITCODE -ne 0) {
    Write-Host "Не удалось установить zig и wasmtime: сборка для браузера и других систем будет недоступна, остальное работает." -ForegroundColor Yellow
}

Шаг "Проверяю работу языка"
Set-Location $Папка
$env:PYTHONUTF8 = "1"
python dan.py запустить "примеры\факториал.dan"
python dan.py запустить "примеры\основы_математики.dan"

Шаг "Запускаю тесты (около минуты)"
python -m unittest

Шаг "Готово"
Write-Host "Проект находится в $Папка"
Write-Host "Запуск программы:  python dan.py запустить примеры\алгебра.dan"
