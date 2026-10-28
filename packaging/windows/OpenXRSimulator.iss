#ifndef StageDir
  #error StageDir must name the absolute staged CMake install directory
#endif
#ifndef ProductVersion
  #error ProductVersion must be supplied
#endif

[Setup]
AppId={{E638AA20-956C-43A5-83AF-8E6ED5A33472}
AppName=OpenXR Simulator
AppVersion={#ProductVersion}
AppPublisher=OpenXR Simulator contributors
AppPublisherURL=https://github.com/Crementif/OpenXR-Simulator
AppSupportURL=https://github.com/Crementif/OpenXR-Simulator
AppUpdatesURL=https://github.com/Crementif/OpenXR-Simulator/releases
DefaultDirName={localappdata}\Programs\OpenXR Simulator
DefaultGroupName=OpenXR Simulator
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
SetupMutex=OpenXRSimulatorSetup
CloseApplications=no
RestartApplications=no
UninstallDisplayIcon={app}\bin\simxr_companion.exe
SetupIconFile={#StageDir}\share\simxr\branding\openxr-simulator.ico
OutputBaseFilename=openxr-simulator-{#ProductVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\bin\simxr_install_check.exe"; Flags: dontcopy
Source: "install.marker"; DestDir: "{app}"; DestName: ".simxr-install"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\OpenXR Simulator"; Filename: "{app}\bin\simxr_companion.exe"; WorkingDir: "{app}\bin"
Name: "{autodesktop}\OpenXR Simulator"; Filename: "{app}\bin\simxr_companion.exe"; WorkingDir: "{app}\bin"; Tasks: desktopicon

[Run]
Filename: "{app}\bin\simxr_companion.exe"; WorkingDir: "{app}\bin"; Description: "Launch OpenXR Simulator"; Flags: postinstall nowait skipifsilent; BeforeInstall: StopGuard

[Code]
procedure ExitProcess(ExitCode: Cardinal);
  external 'ExitProcess@kernel32.dll stdcall';
function GetCurrentProcessId: Cardinal;
  external 'GetCurrentProcessId@kernel32.dll stdcall';
function OpenProcess(Access: Cardinal; InheritHandle: Boolean; ProcessId: Cardinal): THandle;
  external 'OpenProcess@kernel32.dll stdcall';
function WaitForSingleObject(Handle: THandle; Milliseconds: Cardinal): Cardinal;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';
function GuardLastError: Cardinal;
  external 'GetLastError@kernel32.dll stdcall';

var
  GuardHandle: THandle;
  GuardStatus, GuardStop: String;

function ExistingDirectoryIsManaged(const Directory: String): Boolean;
var
  FindResult: TFindRec;
  Marker: AnsiString;
begin
  Result := True;
  if not DirExists(Directory) then
    Exit;
  if LoadStringFromFile(Directory + '\.simxr-install', Marker) then begin
    Result := Trim(String(Marker)) = 'openxr-simulator-install-v2';
    Exit;
  end;
  if FindFirst(Directory + '\*', FindResult) then begin
    try
      repeat
        if (FindResult.Name <> '.') and (FindResult.Name <> '..') and
           (FindResult.Name <> '.simxr-use-lock') then begin
          Result := False;
          Exit;
        end;
      until not FindNext(FindResult);
    finally
      FindClose(FindResult);
    end;
  end;
end;

function GuardAlive: Boolean;
begin
  Result := (GuardHandle <> 0) and (WaitForSingleObject(GuardHandle, 0) = $102);
end;

function StartGuard(const CheckProgram, Directory: String): Integer;
var
  Arguments: String;
  Status: AnsiString;
  ProcessId, ExitCode, I: Integer;
begin
  Result := 1;
  if FileExists(Directory + '\.simxr-install') and not FileExists(Directory + '\.simxr-use-lock') then
    Exit;
  GuardStatus := ExpandConstant('{tmp}\simxr-guard.status');
  GuardStop := ExpandConstant('{tmp}\simxr-guard.stop');
  DeleteFile(GuardStatus);
  DeleteFile(GuardStop);
  Arguments := '--guard "' + Directory + '" ' + IntToStr(GetCurrentProcessId) +
               ' "' + GuardStatus + '" "' + GuardStop + '"';
  if not Exec(CheckProgram, Arguments, '', SW_HIDE, ewNoWait, ExitCode) then
    Exit;
  for I := 1 to 100 do begin
    if LoadStringFromFile(GuardStatus, Status) then begin
      if String(Status) = 'busy' then begin
        Result := 2;
        Exit;
      end;
      if String(Status) = 'error' then
        Exit;
      if Pos('ready:', String(Status)) = 1 then begin
        ProcessId := StrToIntDef(Copy(String(Status), 7, 20), 0);
        if ProcessId > 0 then
          GuardHandle := OpenProcess($00100000, False, ProcessId);
        if GuardAlive then
          Result := 0;
        Exit;
      end;
    end;
    Sleep(100);
  end;
end;

procedure StopGuard;
var
  I: Integer;
begin
  if GuardHandle = 0 then
    Exit;
  SaveStringToFile(GuardStop, 'stop', False);
  for I := 1 to 100 do begin
    if not GuardAlive then
      Break;
    Sleep(50);
  end;
  CloseHandle(GuardHandle);
  GuardHandle := 0;
end;

function InitializeSetup: Boolean;
var
  I: Integer;
  Parameter: String;
begin
  Result := True;
  for I := 1 to ParamCount do begin
    Parameter := Uppercase(ParamStr(I));
    if (Parameter = '/CLOSEAPPLICATIONS') or (Parameter = '/FORCECLOSEAPPLICATIONS') then begin
      Log('Refusing to close running applications.');
      Result := False;
      Exit;
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  CheckResult: Integer;
begin
  Result := '';
  if not ExistingDirectoryIsManaged(ExpandConstant('{app}')) then begin
    Result := 'Choose an empty directory or an existing OpenXR Simulator installation.';
    Exit;
  end;
  if not ForceDirectories(ExpandConstant('{app}')) then begin
    Result := 'Could not create the installation directory.';
    Exit;
  end;
  ExtractTemporaryFile('simxr_install_check.exe');
  CheckResult := StartGuard(ExpandConstant('{tmp}\simxr_install_check.exe'), ExpandConstant('{app}'));
  if CheckResult = 2 then
    Result := 'Close OpenXR Simulator and applications using its runtime, then retry.'
  else if CheckResult <> 0 then
    Result := 'Could not check whether installation files are in use. No files were changed.';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssInstall) and not GuardAlive then begin
    Log('Installation guard stopped before files could be installed.');
    ExitProcess(1);
  end;
end;

procedure DeinitializeSetup;
begin
  StopGuard;
end;

function InitializeUninstall: Boolean;
var
  TemporaryCheck: String;
  CheckResult: Integer;
  MessageText: String;
begin
  TemporaryCheck := ExpandConstant('{tmp}\simxr_install_check.exe');
  CheckResult := 1;
  if CopyFile(ExpandConstant('{app}\bin\simxr_install_check.exe'), TemporaryCheck, False) then
    CheckResult := StartGuard(TemporaryCheck, ExpandConstant('{app}'));
  Result := CheckResult = 0;
  if Result then
    Exit;
  if CheckResult = 2 then
    MessageText := 'Close OpenXR Simulator and applications using its runtime, then retry.'
  else
    MessageText := 'Could not check whether installation files are in use. No files were changed.';
  Log(MessageText);
  if UninstallSilent then
    ExitProcess(CheckResult)
  else
    MsgBox(MessageText, mbError, MB_OK);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if (CurUninstallStep = usUninstall) and not GuardAlive then begin
    Log('Installation guard stopped before files could be removed.');
    ExitProcess(1);
  end;
  if CurUninstallStep = usPostUninstall then begin
    StopGuard;
    if not DeleteFile(ExpandConstant('{app}\.simxr-use-lock')) then
      Log('Could not remove the installation lock: ' + IntToStr(GuardLastError));
  end;
end;

procedure DeinitializeUninstall;
begin
  StopGuard;
end;
