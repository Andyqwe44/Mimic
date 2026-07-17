; Thin MimicServer Setup — downloads signaling payload.zip from CDN at install time.
; Build: ISCC /DMyAppVersion=x.y.z installer\setup_server.iss

#define MyAppName "Mimic Server"
#ifndef MyAppVersion
#define MyAppVersion "0.0.0"
#endif
#define MyAppPublisher "Mimic"
#define MyAppURL "https://gitee.com/Andyqwe44/mimic"
#ifndef MimicCdnBase
#define MimicCdnBase "http://47.107.43.5/mimic"
#endif

[Setup]
AppId={{B2C3D4E5-F6A7-8901-BCDE-F12345678901}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
DefaultDirName={autopf}\MimicServer
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputDir=..\release
OutputBaseFilename=MimicServer_Setup_v{#MyAppVersion}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UninstallDisplayName={#MyAppName}
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription=Mimic signaling server (downloads payload from CDN)

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Icons]
Name: "{group}\{#MyAppName} (README)"; Filename: "{app}\README.md"; Check: PayloadReady
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Run]
Filename: "{cmd}"; Parameters: "/K cd /d ""{app}"" && echo MimicServer — run: node server.js --host 0.0.0.0 --port 8443 && node server.js --host 0.0.0.0 --port 8443"; Description: "Start MimicServer (node)"; Flags: nowait postinstall skipifsilent; Check: PayloadReady

[Code]
var
  DownloadOk: Boolean;

function PayloadReady: Boolean;
begin
  Result := DownloadOk and FileExists(ExpandConstant('{app}\server.js'));
end;

function OnDownloadProgress(const Url, FileName: String; const Progress, ProgressMax: Int64): Boolean;
begin
  if ProgressMax > 0 then
    WizardForm.StatusLabel.Caption :=
      Format('Downloading server payload… %d%%', [(Progress * 100) div ProgressMax])
  else
    WizardForm.StatusLabel.Caption := 'Downloading server payload…';
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
  WizardForm.StatusLabel.Caption := 'Downloading Mimic Server from CDN…';
  try
    DownloadTemporaryFile(
      '{#MimicCdnBase}/server/payload.zip',
      'mimic_server_payload.zip',
      '',
      @OnDownloadProgress);
    ZipPath := ExpandConstant('{tmp}\mimic_server_payload.zip');
    if not FileExists(ZipPath) then
      RaiseException('Download failed: payload.zip missing in {tmp}');
    WizardForm.StatusLabel.Caption := 'Extracting payload…';
    if not ExpandPayload(ZipPath, ExpandConstant('{app}')) then
      RaiseException('Expand-Archive failed — is PowerShell available?');
    if not FileExists(ExpandConstant('{app}\server.js')) then
      RaiseException('Payload incomplete: server.js not found');
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
