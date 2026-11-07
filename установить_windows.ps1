# Установка языка Дан на компьютер с Windows.
# Создаёт папку D:\проекты\Дан (если диска D нет - папку «проекты\Дан» в папке
# пользователя), устанавливает Python, компилятор C (WinLibs), упаковщик UPX и
# Git, загружает проект из GitHub, библиотеку llama.cpp для модуля «модели» и
# проверяет проект тестами.
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

# Внешняя программа: успех определяется по коду завершения. Сообщения о ходе
# работы, которые git и pip пишут в поток ошибок, Windows PowerShell 5.1 при
# перенаправлении вывода иначе считает ошибкой и останавливает сценарий.
function Выполнить {
    $прежнее = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $args[0] @($args | Select-Object -Skip 1) 2>&1 | ForEach-Object { Write-Host $_ } }
    finally { $ErrorActionPreference = $прежнее }
    return $LASTEXITCODE
}

function Шаг($текст) { Write-Host ""; Write-Host "==> $текст" -ForegroundColor Cyan }

function ОбновитьPath {
    $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
                [Environment]::GetEnvironmentVariable("Path", "User")
}

# Команда есть и работает. В Windows 10 и 11 команда python есть всегда: без
# установленного Python она только предлагает открыть Microsoft Store.
function Работает($команда) {
    if (-not (Get-Command $команда -ErrorAction SilentlyContinue)) { return $false }
    $прежнее = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $null = & $команда --version 2>&1
        return $LASTEXITCODE -eq 0
    } catch { return $false }
    finally { $ErrorActionPreference = $прежнее }
}

function Установить($команда, $пакет, $название) {
    if (Работает $команда) {
        Write-Host "$название уже установлен"
        return
    }
    Write-Host "Устанавливаю $название..."
    $null = Выполнить winget install --id $пакет -e --silent --accept-package-agreements --accept-source-agreements
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
    $код = Выполнить git -C $Папка pull
    if ($код -ne 0) { Write-Host "Не удалось обновить проект (git pull): сообщение выше." -ForegroundColor Red; exit 1 }
} elseif ((Test-Path $Папка) -and (Get-ChildItem $Папка -Force | Select-Object -First 1)) {
    Write-Host "Папка $Папка уже существует и не пуста. Переименуйте или очистите её и запустите сценарий снова." -ForegroundColor Red
    exit 1
} else {
    $код = Выполнить git clone $Репозиторий $Папка
    if ($код -ne 0) { Write-Host "Не удалось загрузить проект (git clone): проверьте подключение к интернету." -ForegroundColor Red; exit 1 }
}

Шаг "Устанавливаю дополнительные инструменты (WebAssembly, сборка для других систем)"
$код = Выполнить python -m pip install --upgrade --quiet ziglang wasmtime
if ($код -ne 0) {
    Write-Host "Не удалось установить zig и wasmtime: сборка для браузера и других систем будет недоступна, остальное работает." -ForegroundColor Yellow
}

Шаг "Загружаю библиотеку llama.cpp для модуля «модели»"
# Архив llama-<метка>-windows-x64.zip из выпуска Дана этой версии, со сверкой SHA-256 по
# SHA256SUMS.txt выпуска, - в папку пользователя %LOCALAPPDATA%\Dan\llama, где её находят
# программы на Дане. Без неё работает всё, кроме модуля «модели».
try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    # версии - из versiya.py и modeli.py без запуска Python: сообщения программ в потоке ошибок Windows
    # PowerShell 5.1 при перенаправлении вывода считает ошибкой и останавливает сценарий
    $текстВерсии = Get-Content (Join-Path $Папка "versiya.py") -Raw -Encoding UTF8
    if ($текстВерсии -notmatch '(?m)^ВЕРСИЯ\s*=\s*"([^"]+)"') { throw "в versiya.py нет номера версии" }
    $версияДана = $Matches[1]
    $текстМоделей = Get-Content (Join-Path $Папка "modeli.py") -Raw -Encoding UTF8
    if ($текстМоделей -notmatch '(?m)^ВЕРСИЯ_LLAMA\s*=\s*"([^"]+)"') { throw "в modeli.py нет версии llama.cpp" }
    $метка = $Matches[1]
    $архив = "llama-$метка-windows-x64.zip"
    $адрес = "https://github.com/alexandervdanchenko-ai/dan/releases/download/v$версияДана"
    $временная = Join-Path $env:TEMP "dan-llama"
    if (Test-Path $временная) { Remove-Item -Recurse -Force $временная }
    New-Item -ItemType Directory -Force -Path $временная | Out-Null
    Invoke-WebRequest -UseBasicParsing -Uri "$адрес/SHA256SUMS.txt" -OutFile (Join-Path $временная "SHA256SUMS.txt")
    $ожидается = ""
    foreach ($строка in Get-Content (Join-Path $временная "SHA256SUMS.txt")) {
        $части = $строка.Trim() -split "\s+"
        if ($части.Count -eq 2 -and $части[1].TrimStart("*") -eq $архив) { $ожидается = $части[0].ToLower() }
    }
    if (-not $ожидается) { throw "в выпуске Дана $версияДана нет архива $архив" }
    Invoke-WebRequest -UseBasicParsing -Uri "$адрес/$архив" -OutFile (Join-Path $временная $архив)
    $получено = (Get-FileHash -Algorithm SHA256 -Path (Join-Path $временная $архив)).Hash.ToLower()
    if ($получено -ne $ожидается) { throw "контрольная сумма $архив не совпадает с SHA256SUMS.txt" }
    $llama = Join-Path $env:LOCALAPPDATA "Dan\llama"
    if (Test-Path $llama) { Remove-Item -Recurse -Force $llama }
    Expand-Archive -Path (Join-Path $временная $архив) -DestinationPath $llama
    Remove-Item -Recurse -Force $временная
    Write-Host "Библиотека llama.cpp $метка установлена в $llama"
} catch {
    Write-Host "Библиотека llama.cpp не загружена: $($_.Exception.Message)." -ForegroundColor Yellow
    Write-Host "Модуль «модели» будет недоступен, остальное работает. Библиотеку можно собрать сценарием поставка\собрать_llama.py." -ForegroundColor Yellow
}

Шаг "Проверяю работу языка"
Set-Location $Папка
$env:PYTHONUTF8 = "1"
foreach ($пример in "примеры\факториал.dan", "примеры\основы_математики.dan") {
    $код = Выполнить python dan.py запустить $пример
    if ($код -ne 0) { Write-Host "Программа $пример не запустилась: сообщение об ошибке выше." -ForegroundColor Red; exit 1 }
}

if ($env:DAN_NO_TESTS -ne "1") {
    Шаг "Запускаю тесты (несколько минут)"
    $код = Выполнить python -m unittest discover -p "test_*.py"
    if ($код -ne 0) { Write-Host "Часть тестов не прошла: подробности выше. Язык при этом установлен." -ForegroundColor Yellow }
}

Шаг "Готово"
Write-Host "Проект находится в $Папка"
Write-Host "Запуск программы:  python dan.py запустить примеры\алгебра.dan"
