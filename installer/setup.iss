; setup.iss — InnoSetup script for Game Agent Monitor
; Build: "C:\Program Files\Inno Setup 6\ISCC.exe" setup.iss

#define MyAppName "Game Agent Monitor"
#ifndef MyAppVersion
#define MyAppVersion "0.0.0"
#endif
#define MyAppPublisher "GameAgentMonitor"
#define MyAppURL "https://gitee.com/Andyqwe44/tictactoe"
#define MyAppExeName "monitor_app.exe"

[Setup]
AppId={{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
; Refuse to install/uninstall while the app is running — Inno built-in, reuses the
; app's prod single-instance mutex. Inno prompts the user to close it first.
AppMutex=Global\GameAgentMonitor_8A3F2D
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\GameAgentMonitor
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputDir=..\release
OutputBaseFilename=GameAgentMonitor_Setup_v{#MyAppVersion}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UninstallDisplayIcon={app}\bin\monitor_app.exe
UninstallDisplayName={#MyAppName}
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription=Desktop monitor for visual game AI

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Components]
Name: "core";     Description: "Core Application (required)";                Types: full compact custom; Flags: fixed
Name: "frontend"; Description: "Web Frontend (React UI)";                    Types: full compact
Name: "capture";  Description: "Capture Modules (WGC / GDI / DesktopBlt)";  Types: full compact
Name: "input";    Description: "Input Forwarding Modules";                  Types: full compact

[Files]
; Core binaries
Source: "..\release\GameAgentMonitor\bin\monitor_app.exe";  DestDir: "{app}\bin"; Components: core; Flags: ignoreversion
Source: "..\release\GameAgentMonitor\bin\logger.dll";       DestDir: "{app}\bin"; Components: core; Flags: ignoreversion
Source: "..\release\GameAgentMonitor\bin\updater.exe";      DestDir: "{app}\bin"; Components: core; Flags: ignoreversion
Source: "..\release\GameAgentMonitor\bin\updater.new";      DestDir: "{app}\bin"; Components: core; Flags: ignoreversion
Source: "..\release\GameAgentMonitor\bin\capture_common.dll"; DestDir: "{app}\bin"; Components: core; Flags: ignoreversion capture; Flags: ignoreversion

; Capture modules
Source: "..\release\GameAgentMonitor\bin\capture_wgc.dll";     DestDir: "{app}\bin"; Components: capture; Flags: ignoreversion
Source: "..\release\GameAgentMonitor\bin\capture_gdi.dll";     DestDir: "{app}\bin"; Components: capture; Flags: ignoreversion
Source: "..\release\GameAgentMonitor\bin\capture_desktop.dll"; DestDir: "{app}\bin"; Components: capture; Flags: ignoreversion
Source: "..\release\GameAgentMonitor\bin\capture_pw.dll";      DestDir: "{app}\bin"; Components: capture; Flags: ignoreversion
Source: "..\release\GameAgentMonitor\bin\capture_screen.dll";  DestDir: "{app}\bin"; Components: capture; Flags: ignoreversion

; Input modules
Source: "..\release\GameAgentMonitor\bin\input_common.dll";      DestDir: "{app}\bin"; Components: input; Flags: ignoreversion
Source: "..\release\GameAgentMonitor\bin\input_sendinput.dll";   DestDir: "{app}\bin"; Components: input; Flags: ignoreversion
Source: "..\release\GameAgentMonitor\bin\input_postmessage.dll"; DestDir: "{app}\bin"; Components: input; Flags: ignoreversion
Source: "..\release\GameAgentMonitor\bin\input_winapi.dll";      DestDir: "{app}\bin"; Components: input; Flags: ignoreversion
Source: "..\release\GameAgentMonitor\bin\input_driver.dll";      DestDir: "{app}\bin"; Components: input; Flags: ignoreversion

; Frontend
Source: "..\release\GameAgentMonitor\frontend\*"; DestDir: "{app}\frontend"; Components: frontend; Flags: ignoreversion recursesubdirs

; Config template
Source: "..\config\settings.default.json"; DestDir: "{app}\config"; Components: core; Flags: ignoreversion

; Version manifest
Source: "..\release\GameAgentMonitor\version.json"; DestDir: "{app}"; Components: core; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\bin\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\bin\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; Install path for updater
Root: HKLM; Subkey: "SOFTWARE\GameAgentMonitor"; ValueType: string; ValueName: "InstallPath"; ValueData: "{app}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\GameAgentMonitor"; ValueType: string; ValueName: "Version"; ValueData: "{#MyAppVersion}"; Flags: uninsdeletekey

[Dirs]
; Runtime data root — created at install so Inno owns it; monitor_app also
; ensure_dir's it at runtime. All dynamic data (logs, settings, WebView2, staging)
; lives under here, writable regardless of the install drive.
Name: "{localappdata}\GameAgentMonitor"; Flags: uninsalwaysuninstall

[UninstallDelete]
; Runtime data — Inno never "installed" these files (app/updater wrote them at
; runtime), so this recursive delete is what actually wipes the folder on uninstall.
Type: filesandordirs; Name: "{localappdata}\GameAgentMonitor"
; Install tree — updater increments additively drop new DLLs / updater.exe.old into
; {app}\bin that aren't in Inno's install manifest; nuke the whole tree to leave nothing.
Type: filesandordirs; Name: "{app}"

; Wipe the "always run as admin" Layers flag on every install/upgrade. A prior
; run-as-admin choice sticks a RUNASADMIN value into HKCU\...\Layers that forces
; the exe to always elevate — then ShellExecute from the installer's [Run] fails
; with error 740 (elevation required vs the already-dropped installer token).
; Deleting it here resets the user to a clean normal-user launch every install.
[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers"; ValueType: none; ValueName: "{app}\bin\{#MyAppExeName}"; Flags: deletevalue

[Run]
Filename: "{app}\bin\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[Code]
function InitializeSetup: Boolean;
begin
  Result := True;
end;
