#define MyAppName "PS GuardPRO"
#define MyAppVersion "8.10.0"
#define MyAppPublisher "P.Sveshnikov"
#define MyAppExeName "PS_GuardPRO.exe"

[Setup]
AppId={{8C34C71A-7B25-4F31-A77E-9D2D0F9D28B8}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\Programs\PS GuardPRO
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=installer_output
OutputBaseFilename=PS_GuardPRO_8.10.0_Setup
SetupIconFile=Resources\PS_GuardPRO.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
ArchitecturesInstallIn64BitMode=x64
ArchitecturesAllowed=x64
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
UsePreviousAppDir=yes

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "Создать ярлык на рабочем столе"; GroupDescription: "Дополнительные параметры:"; Flags: unchecked

[Files]
Source: "dist\PS_GuardPRO\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\PS GuardPRO"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{autodesktop}\PS GuardPRO"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Запустить PS GuardPRO"; Flags: postinstall nowait skipifsilent

[UninstallDelete]
; Пользовательские данные (Employees, Config, Exports, Backups и т.п.) намеренно сохраняются.
