; setup.iss — InnoSetup script for Game Agent Monitor
; Build: "C:\Program Files\Inno Setup 6\ISCC.exe" setup.iss

#define MyAppName "Game Agent Monitor"
#define MyAppVersion "0.3.4"
#define MyAppPublisher "GameAgentMonitor"
#define MyAppURL "https://gitee.com/Andyqwe44/tictactoe"
#define MyAppExeName "monitor_app.exe"

[Setup]
AppId={{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
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
Source: "..\release\GameAgentMonitor\bin\monitor_app.exe";  DestDir: "{app}\bin"; Components: core
Source: "..\release\GameAgentMonitor\bin\logger.dll";       DestDir: "{app}\bin"; Components: core
Source: "..\release\GameAgentMonitor\bin\updater.exe";      DestDir: "{app}\bin"; Components: core
Source: "..\release\GameAgentMonitor\bin\capture_common.dll"; DestDir: "{app}\bin"; Components: core capture

; Capture modules
Source: "..\release\GameAgentMonitor\bin\capture_wgc.dll";     DestDir: "{app}\bin"; Components: capture
Source: "..\release\GameAgentMonitor\bin\capture_gdi.dll";     DestDir: "{app}\bin"; Components: capture
Source: "..\release\GameAgentMonitor\bin\capture_desktop.dll"; DestDir: "{app}\bin"; Components: capture
Source: "..\release\GameAgentMonitor\bin\capture_pw.dll";      DestDir: "{app}\bin"; Components: capture
Source: "..\release\GameAgentMonitor\bin\capture_screen.dll";  DestDir: "{app}\bin"; Components: capture

; Input modules
Source: "..\release\GameAgentMonitor\bin\input_common.dll";      DestDir: "{app}\bin"; Components: input
Source: "..\release\GameAgentMonitor\bin\input_sendinput.dll";   DestDir: "{app}\bin"; Components: input
Source: "..\release\GameAgentMonitor\bin\input_postmessage.dll"; DestDir: "{app}\bin"; Components: input
Source: "..\release\GameAgentMonitor\bin\input_winapi.dll";      DestDir: "{app}\bin"; Components: input
Source: "..\release\GameAgentMonitor\bin\input_driver.dll";      DestDir: "{app}\bin"; Components: input

; Frontend
Source: "..\release\GameAgentMonitor\frontend\*"; DestDir: "{app}\frontend"; Components: frontend; Flags: recursesubdirs

; Config template
Source: "..\config\settings.default.json"; DestDir: "{app}\config"; Components: core

; Version manifest
Source: "..\release\GameAgentMonitor\version.json"; DestDir: "{app}"; Components: core

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\bin\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\bin\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; Install path for updater
Root: HKLM; Subkey: "SOFTWARE\GameAgentMonitor"; ValueType: string; ValueName: "InstallPath"; ValueData: "{app}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\GameAgentMonitor"; ValueType: string; ValueName: "Version"; ValueData: "{#MyAppVersion}"; Flags: uninsdeletekey

[Run]
Filename: "{app}\bin\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[Code]
function InitializeSetup: Boolean;
begin
  Result := True;
  // Check WebView2 Runtime (Edge Chromium)
  if not RegKeyExists(HKLM, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}') then
  begin
    if MsgBox('Microsoft Edge WebView2 Runtime is required but not detected.' + #13#10 +
              'Please install it from https://go.microsoft.com/fwlink/p/?LinkId=2124703' + #13#10 +
              'Continue installation anyway?', mbConfirmation, MB_YESNO) = IDNO then
    begin
      Result := False;
    end;
  end;
end;
