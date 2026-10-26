# Установка языка Дан на компьютер с Windows.
# Создаёт папку D:\проекты\Дан (если диска D нет - папку «проекты\Дан» в папке
# пользователя), устанавливает Python, компилятор C (WinLibs), упаковщик UPX и
# Git, загружает проект из GitHub и проверяет его тестами.
# Другую папку можно задать переменной окружения DAN_FOLDER.
#
# Запуск (в обычном окне PowerShell):
#   powershell -ExecutionPolicy Bypass -File установить_windows.ps1

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

if ($env:DAN_FOLDER) { $Папка = $env:DAN_FOLDER }
elseif (Test-Path "D:\") { $Папка = "D:\проекты\Дан" }
else { $Папка = Join-Path $env:USERPROFILE "проекты\Дан" }
$Репозиторий = "https://github.com/alexandervdanchenko-ai/dan.git"

function Шаг($текст) { Write-Host ""; Write-Host "==> $текст" -ForegroundColor Cyan }

function ОбновитьPath {
    $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
                [Environment]::GetEnvironmentVariable("Path", "User")
}

# Команда есть и работает. В Windows 10 и 11 команда python есть всегда: без
# установленного Python она только предлагает открыть Microsoft Store.
function Работает($команда) {
    if (-not (Get-Command $команда -ErrorAction SilentlyContinue)) { return $false }
    try {
        $null = & $команда --version 2>&1
        return $LASTEXITCODE -eq 0
    } catch { return $false }
}

function Установить($команда, $пакет, $название) {
    if (Работает $команда) {
        Write-Host "$название уже установлен"
        return
    }
    Write-Host "Устанавливаю $название..."
    winget install --id $пакет -e --silent --accept-package-agreements --accept-source-agreements
    ОбновитьPath
    if (-not (Работает $команда)) {
        Write-Host "$название установлен, но пока не виден. Закройте окно PowerShell, откройте новое и запустите сценарий ещё раз." -ForegroundColor Yellow
        exit 1
    }
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
New-Item -ItemType Directory -Force -Path (Split-Path $Папка -Parent) | Out-Null

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
if ($LASTEXITCODE -ne 0) { Write-Host "Программа не запустилась: сообщение об ошибке выше." -ForegroundColor Red; exit 1 }
python dan.py запустить "примеры\основы_математики.dan"
if ($LASTEXITCODE -ne 0) { Write-Host "Программа не запустилась: сообщение об ошибке выше." -ForegroundColor Red; exit 1 }

if ($env:DAN_NO_TESTS -ne "1") {
    Шаг "Запускаю тесты (несколько минут)"
    python -m unittest discover -p "test_*.py"
}

Шаг "Готово"
Write-Host "Проект находится в $Папка"
Write-Host "Запуск программы:  python dan.py запустить примеры\алгебра.dan"
