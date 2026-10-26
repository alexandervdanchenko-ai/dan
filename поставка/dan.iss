; Установщик языка Дан для Windows (Inno Setup 6).
; Собирается сценарием собрать.py: он готовит папку «готово\dan» и передаёт
; номер версии параметром /DAppVer=...
; Установка не требует прав администратора: Дан ставится для текущего
; пользователя, команда dan добавляется в его PATH.

#ifndef AppVer
  #define AppVer "0.0"
#endif

[Setup]
AppId={{6E7A3F1C-9C2B-4D7E-9A51-DA0DA0DA0001}
AppName=Дан
AppVersion={#AppVer}
AppVerName=Дан {#AppVer}
AppPublisher=Язык программирования Дан
AppPublisherURL=https://github.com/alexandervdanchenko-ai/dan
AppSupportURL=https://github.com/alexandervdanchenko-ai/dan/issues
DefaultDirName={autopf}\Dan
DefaultGroupName=Дан
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ChangesEnvironment=yes
ChangesAssociations=yes
OutputDir=готово
OutputBaseFilename=dan-{#AppVer}-setup
Compression=lzma2/ultra64
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
UninstallDisplayName=Дан {#AppVer}
LicenseFile=..\LICENSE
SetupIconFile=значки\dan.ico
UninstallDisplayIcon={app}\dan.ico

[Languages]
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "path"; Description: "Добавить команду dan в командную строку (PATH)"; GroupDescription: "Дополнительно:"
Name: "assoc"; Description: "Запускать программы .dan двойным щелчком"; GroupDescription: "Дополнительно:"

[Files]
Source: "готово\dan\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
; примеры - в папку «Документы\Дан», где их можно менять
Source: "готово\dan\примеры\*.dan"; DestDir: "{userdocs}\Дан\примеры"; Flags: onlyifdoesntexist

[Icons]
Name: "{group}\Редактор Дан"; Filename: "{app}\редактор.exe"; Parameters: "--дан=""{app}\dan.exe"""; WorkingDir: "{userdocs}\Дан"; IconFilename: "{app}\dan.ico"
Name: "{group}\Учебник «Дан с нуля»"; Filename: "{app}\учебник\index.html"
Name: "{group}\Дан - командная строка"; Filename: "{cmd}"; Parameters: "/k chcp 65001 >nul && echo Дан {#AppVer}. Команды: dan"; WorkingDir: "{userdocs}\Дан"; IconFilename: "{app}\dan.ico"
Name: "{group}\Примеры программ"; Filename: "{userdocs}\Дан\примеры"
Name: "{group}\Справочник языка"; Filename: "{app}\справочник.html"
Name: "{group}\Библиотека Дана"; Filename: "{app}\документация_библиотеки\index.html"
Name: "{group}\Удалить Дан"; Filename: "{uninstallexe}"

[Registry]
; команда dan в PATH пользователя
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; \
  Tasks: path; Check: NeedsAddPath(ExpandConstant('{app}'))
; файлы .dan: двойной щелчок - запуск в окне консоли, «Изменить» - Блокнот
Root: HKCU; Subkey: "Software\Classes\.dan"; ValueType: string; ValueData: "Dan.Program"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Dan.Program"; ValueType: string; ValueData: "Программа на языке Дан"; Flags: uninsdeletekey; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Dan.Program\DefaultIcon"; ValueType: string; ValueData: """{app}\dan-fail.ico"",0"; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Dan.Program\shell\open\command"; ValueType: string; \
  ValueData: """{cmd}"" /k chcp 65001 >nul && ""{app}\dan.exe"" запустить ""%1"""; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Dan.Program\shell\edit\command"; ValueType: string; \
  ValueData: """{app}\редактор.exe"" --дан=""{app}\dan.exe"" ""%1"""; Tasks: assoc

[Run]
Filename: "{app}\редактор.exe"; Parameters: "--дан=""{app}\dan.exe"" ""{userdocs}\Дан\примеры\факториал.dan"""; \
  WorkingDir: "{userdocs}\Дан"; Description: "Открыть редактор Дан"; Flags: postinstall nowait skipifsilent

[Code]
// в коде Pascal Script имена - только латиницей
function NeedsAddPath(Dir: string): Boolean;
var
  Current: string;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Current) then
    Result := True
  else
    Result := Pos(';' + Uppercase(Dir) + ';', ';' + Uppercase(Current) + ';') = 0;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Current, Dir: string;
  P: Integer;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    Dir := ExpandConstant('{app}');
    if RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Current) then
    begin
      P := Pos(';' + Uppercase(Dir), Uppercase(Current));
      if P > 0 then
      begin
        Delete(Current, P, Length(Dir) + 1);
        RegWriteExpandStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Current);
      end;
    end;
  end;
end;
