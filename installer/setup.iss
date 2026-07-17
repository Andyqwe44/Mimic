; Thin MimicClient Setup — downloads payload.zip from CDN at install time.
; Build: ISCC /DMyAppVersion=x.y.z installer\setup.iss

#define MyAppName "Mimic Client"
#ifndef MyAppVersion
#define MyAppVersion "0.0.0"
#endif
#define MyAppPublisher "Mimic"
#define MyAppURL "https://gitee.com/Andyqwe44/mimic"
#define MyAppExeName "mimic_client.exe"
#ifndef MimicCdnBase
#define MimicCdnBase "http://47.107.43.5/mimic"
#endif

[Setup]
AppId={{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
AppMutex=Global\MimicClient_8A3F2D
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\MimicClient
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputDir=..\release
OutputBaseFilename=MimicClient_Setup_v{#MyAppVersion}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UninstallDisplayIcon={app}\bin\mimic_client.exe
UninstallDisplayName={#MyAppName}
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription=Mimic peer client (downloads payload from CDN)
; Thin installer: no [Files] payload — downloaded in CurStepChanged(ssInstall)

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\bin\{#MyAppExeName}"; Check: PayloadReady
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\bin\{#MyAppExeName}"; Tasks: desktopicon; Check: PayloadReady

[Registry]
Root: HKLM; Subkey: "SOFTWARE\MimicClient"; ValueType: string; ValueName: "InstallPath"; ValueData: "{app}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\MimicClient"; ValueType: string; ValueName: "Version"; ValueData: "{#MyAppVersion}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers"; ValueType: none; ValueName: "{app}\bin\{#MyAppExeName}"; Flags: deletevalue

[Dirs]
Name: "{localappdata}\MimicClient"; Flags: uninsalwaysuninstall

[UninstallDelete]
Type: filesandordirs; Name: "{localappdata}\MimicClient"
Type: filesandordirs; Name: "{app}"

[Run]
Filename: "{app}\bin\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent; Check: PayloadReady

[Code]
var
  DownloadOk: Boolean;

function PayloadReady: Boolean;
begin
  Result := DownloadOk and FileExists(ExpandConstant('{app}\bin\{#MyAppExeName}'));
end;

function OnDownloadProgress(const Url, FileName: String; const Progress, ProgressMax: Int64): Boolean;
begin
  if ProgressMax > 0 then
    WizardForm.StatusLabel.Caption :=
      Format('Downloading client payload… %d%%', [(Progress * 100) div ProgressMax])
  else
    WizardForm.StatusLabel.Caption := 'Downloading client payload…';
  Result := True;
end;

function ExpandPayload(const ZipPath, DestDir: String): Boolean;
var
  ResultCode: Integer;
  Ps: String;
begin
  ForceDirectories(DestDir);
  Ps := '-NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -LiteralPath ''' +
        ZipPath + ''' -DestinationPath ''' + DestDir + ''' -Force"';
  Result := Exec('powershell.exe', Ps, '', SW_HIDE, ewWaitUntilTerminated, ResultCode)
            and (ResultCode = 0);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ZipPath: String;
begin
  if CurStep <> ssInstall then Exit;
  DownloadOk := False;
  WizardForm.StatusLabel.Caption := 'Downloading Mimic Client from CDN…';
  try
    DownloadTemporaryFile(
      '{#MimicCdnBase}/client/payload.zip',
      'mimic_client_payload.zip',
      '',
      @OnDownloadProgress);
    ZipPath := ExpandConstant('{tmp}\mimic_client_payload.zip');
    if not FileExists(ZipPath) then
      RaiseException('Download failed: payload.zip missing in {tmp}');
    WizardForm.StatusLabel.Caption := 'Extracting payload…';
    if not ExpandPayload(ZipPath, ExpandConstant('{app}')) then
      RaiseException('Expand-Archive failed — is PowerShell available?');
    if not FileExists(ExpandConstant('{app}\bin\{#MyAppExeName}')) then
      RaiseException('Payload incomplete: bin\mimic_client.exe not found');
    DownloadOk := True;
  except
    RaiseException('CDN install failed: ' + GetExceptionMessage);
  end;
end;

function InitializeSetup: Boolean;
begin
  DownloadOk := False;
  Result := True;
end;
