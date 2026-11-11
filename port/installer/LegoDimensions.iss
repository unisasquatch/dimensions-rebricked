; Inno Setup script for Dimensions Rebricked. Build with make_installer.cmd (stages the files first).
; setup.exe carries only the port (~70 MB). The game data (~50 GB, from the PS4 packages) is copied in at install time
; from an unpacked folder you pick, or left where it is; or, with "I have PKG files", setup unpacks the .pkg files
; themselves into {app}\game with ldpkg.exe (port\tools\ldpkg). PKG files need the package keys file (the two
; fake-package RSA keys, never shipped with setup), kept as {app}\data\pkgkeys.bin so "Add DLC packs" (the launcher's
; --add-dlc) can unpack more add-on packs later. A separate folder of DLC .pkg files is optional.

#define AppName "Dimensions Rebricked"
#define AppVersion GetEnv("LDPORT_VERSION")
#if AppVersion == ""
  #define AppVersion "1.0"
#endif
; LegoToypad (harrysof, MIT licence, https://github.com/harrysof/LegoToypad): downloaded from its GitHub release
; only when the "legotoypad" task is ticked, never bundled. To move to a newer release, change the URL, the
; SHA-256 (the release asset's "digest" in the GitHub API) and the size.
#define LegoToypadUrl "https://github.com/harrysof/LegoToypad/releases/download/v1.7/LegoToypad_app_Version_1.7.exe"
#define LegoToypadSha256 "ed2ff917b0ca5b632389112ef7bec75cfadddec2cc401a579ecefdcc06bc8831"
#define LegoToypadSize "32847872"

[Setup]
AppId={{6B0E8C1F-2D1A-4E57-9C3B-5D1E0A7F4C21}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Dimensions Rebricked
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
; Per-user install: the game writes saves, settings and its shader cache next to itself.
PrivilegesRequired=lowest
OutputDir=output
OutputBaseFilename=Dimensions-Rebricked-Setup
SetupIconFile=icon.ico
UninstallDisplayIcon={app}\{#AppName}.exe
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; The copied game data (counted only when it is copied).
ExtraDiskSpaceRequired=0

; Always a fresh install: setup carries only the port's programs (stage.py), never saves, Toy Pad figures, settings
; or a shader cache from the PC it was built on. The game starts with its default settings and no saves.
[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"
Name: "importshadps4"; Description: "Import LEGO Dimensions saves from the shadPS4 emulator (the saves here are backed up first)"; GroupDescription: "Saves:"; Flags: unchecked
Name: "legotoypad"; Description: "Download the LegoToypad app by harrysof (about 33 MB, from GitHub) and use it as the Toy Pad"; GroupDescription: "Toy Pad (the game's own virtual Toy Pad stays available; Options > Gameplay > Toy Pad switches):"; Flags: unchecked
Name: "legotoypad\start"; Description: "Start LegoToypad with the game"

[Files]
Source: "stage\app\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
; LegoToypad, downloaded (and checked against its SHA-256) only with the "legotoypad" task.
Source: "{#LegoToypadUrl}"; DestName: "LegoToypad.exe"; DestDir: "{app}\LegoToypad"; Hash: "{#LegoToypadSha256}"; ExternalSize: {#LegoToypadSize}; Flags: external download ignoreversion; Tasks: legotoypad
; ldpkg.exe again, for checking the chosen .pkg files before installing ("I have PKG files").
Source: "stage\app\ldpkg.exe"; Flags: dontcopy
; Game data, copied from the chosen folder (an "extracted" folder: elf\, CUSA01176\uroot, CUSA01176-base\uroot, addcont\).
Source: "{code:GameSrc}\elf\eboot.elf"; DestDir: "{app}\game"; Flags: external ignoreversion; Check: CopyGame
Source: "{code:GameSrc}\CUSA01176\uroot\*"; DestDir: "{app}\game\update"; Flags: external ignoreversion recursesubdirs createallsubdirs; Check: CopyGame
Source: "{code:GameSrc}\CUSA01176-base\uroot\*"; DestDir: "{app}\game\base"; Flags: external ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist; Check: CopyGame and HasPart('CUSA01176-base\uroot')
Source: "{code:GameSrc}\addcont\*"; DestDir: "{app}\game\addcont"; Flags: external ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist; Check: CopyGame and HasPart('addcont')

[InstallDelete]
; Switching between copied and left-in-place game data.
Type: files; Name: "{app}\game.txt"
; Version 1.0's launcher and shortcuts, from before the rename to Dimensions Rebricked (same AppId, so an update
; installs over it).
Type: files; Name: "{app}\LEGO Dimensions.exe"
Type: filesandordirs; Name: "{userprograms}\LEGO Dimensions"
Type: files; Name: "{userdesktop}\LEGO Dimensions.lnk"

[UninstallDelete]
Type: files; Name: "{app}\game.txt"
; Game data unpacked from .pkg files by ldpkg (copied game data is removed as installed files anyway).
Type: filesandordirs; Name: "{app}\game"
Type: files; Name: "{app}\data\ldpkg.log"
Type: files; Name: "{app}\data\ldpkg-dlc.log"
Type: files; Name: "{app}\data\pkgkeys.bin"
Type: files; Name: "{app}\data\ldport.log"
Type: files; Name: "{app}\data\ldport.prev.log"
Type: files; Name: "{app}\data\shadps4-import.log"
Type: filesandordirs; Name: "{app}\data\shadercache"
Type: filesandordirs; Name: "{app}\LegoToypad"

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppName}.exe"; WorkingDir: "{app}"
Name: "{group}\Virtual Toy Pad (while the game runs)"; Filename: "http://127.0.0.1:8642/"
Name: "{group}\Add DLC packs"; Filename: "{app}\{#AppName}.exe"; Parameters: "--add-dlc"; WorkingDir: "{app}"
Name: "{group}\Game folder (saves and log)"; Filename: "{app}\data"
Name: "{group}\Transfer saves (shadPS4, Xenia, Cemu, RPCS3)"; Filename: "{app}\ldport.exe"; Parameters: "--save-transfer --data ""{app}\data"""; WorkingDir: "{app}"
Name: "{group}\LegoToypad"; Filename: "{app}\LegoToypad\LegoToypad.exe"; WorkingDir: "{app}\LegoToypad"; Tasks: legotoypad
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppName}.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppName}.exe"; Description: "Play {#AppName} now"; Flags: nowait postinstall skipifsilent

[Code]
var
  GamePage: TInputDirWizardPage;
  ModePage: TInputOptionWizardPage;
  ShadPage: TInputDirWizardPage;
  KeysPage: TInputFileWizardPage;
  DlcPage: TInputDirWizardPage;
  ShadOffered: Boolean;
  DirRadio, PkgRadio: TNewRadioButton;

// shadPS4 keeps its user folder in %APPDATA%\shadPS4, or in "user" next to shadPS4.exe for a portable copy.
// /SHADPS4="<folder>" on the command line picks one (with /TASKS="importshadps4" for silent installs).
function DefaultShadPS4: String;
begin
  Result := ExpandConstant('{param:SHADPS4|}');
  if (Result = '') and DirExists(ExpandConstant('{userappdata}\shadPS4\savedata')) then
    Result := ExpandConstant('{userappdata}\shadPS4');
end;

// True when Dir (or a folder below it) holds a LEGO Dimensions save: the game writes GAME01 / V2GAME01 into each.
function HasLegoSaves(Dir: String; Depth: Integer): Boolean;
var
  F: TFindRec;
begin
  Result := FileExists(Dir + '\GAME01') or FileExists(Dir + '\V2GAME01');
  if Result or (Depth = 0) then Exit;
  if FindFirst(Dir + '\*', F) then begin
    try
      repeat
        if (F.Attributes and FILE_ATTRIBUTE_DIRECTORY <> 0) and (F.Name <> '.') and (F.Name <> '..') and
           (Copy(F.Name, 1, 4) <> 'sce_') then
          Result := HasLegoSaves(Dir + '\' + F.Name, Depth - 1);
      until Result or not FindNext(F);
    finally
      FindClose(F);
    end;
  end;
end;

function ShadSrc: String;
begin
  Result := RemoveBackslashUnlessRoot(ShadPage.Values[0]);
end;

function GameSrc(Param: String): String;
begin
  Result := RemoveBackslashUnlessRoot(GamePage.Values[0]);
end;

// "I have PKG files": GamePage then names a folder of .pkg files, which ldpkg unpacks into {app}\game.
function PkgMode: Boolean;
begin
  Result := PkgRadio.Checked;
end;

function KeysSrc: String;
begin
  Result := Trim(KeysPage.Values[0]);
end;

// The optional folder of DLC (add-on pack) .pkg files; empty when none, or when it is the game's own .pkg folder.
function DlcSrc: String;
begin
  Result := RemoveBackslashUnlessRoot(Trim(DlcPage.Values[0]));
  if CompareText(Result, GameSrc('')) = 0 then Result := '';
end;

function CopyGame: Boolean;
begin
  Result := ModePage.Values[0] and not PkgMode;
end;

procedure GameKindClick(Sender: TObject);
begin
  if PkgMode then GamePage.PromptLabels[0].Caption := 'Folder with the .pkg files:'
  else GamePage.PromptLabels[0].Caption := 'Game data folder:';
end;

function HasPart(Part: String): Boolean;
begin
  Result := DirExists(GameSrc('') + '\' + Part);
end;

procedure InitializeWizard;
begin
  GamePage := CreateInputDirPage(wpSelectDir, 'Game data',
    'Where is the game data?',
    'Pick the "extracted" folder made from your LEGO Dimensions PS4 packages. It must hold elf\eboot.elf and ' +
    'CUSA01176\uroot (the 1.24 update); CUSA01176-base\uroot (base game 1.00) and addcont (add-on packs) are used ' +
    'when present. Or, if you have the .pkg files (fake-signed, as used with shadPS4), pick "I have PKG files" and ' +
    'the folder that holds them.',
    False, '');
  GamePage.Add('Game data folder:');
  // /GAMEDATA="<folder>" and /INPLACE=1 on the command line (for silent installs); /PKGS="<folder>" for .pkg files.
  GamePage.Values[0] := ExpandConstant('{param:GAMEDATA|}');

  DirRadio := TNewRadioButton.Create(GamePage);
  DirRadio.Parent := GamePage.Surface;
  DirRadio.Top := GamePage.Edits[0].Top + GamePage.Edits[0].Height + ScaleY(16);
  DirRadio.Width := GamePage.SurfaceWidth;
  DirRadio.Caption := 'The folder holds the unpacked game data (elf, CUSA01176, ...)';
  DirRadio.Checked := True;
  DirRadio.OnClick := @GameKindClick;
  PkgRadio := TNewRadioButton.Create(GamePage);
  PkgRadio.Parent := GamePage.Surface;
  PkgRadio.Top := DirRadio.Top + DirRadio.Height + ScaleY(6);
  PkgRadio.Width := GamePage.SurfaceWidth;
  PkgRadio.Caption := 'I have PKG files: the folder holds the game''s .pkg files (1.24 update, base game 1.00)';
  PkgRadio.OnClick := @GameKindClick;
  if (ExpandConstant('{param:PKGS|}') <> '') or (ExpandConstant('{param:GAMEDATA|}') = '') then begin
    PkgRadio.Checked := True;  // PKG files are what most people have: the default
    GamePage.Values[0] := ExpandConstant('{param:PKGS|}');
  end;
  if ExpandConstant('{param:PKGS|}') <> '' then begin
    GamePage.Values[0] := ExpandConstant('{param:PKGS}');
    PkgRadio.Checked := True;
  end;
  GameKindClick(nil);

  ModePage := CreateInputOptionPage(GamePage.ID, 'Game data',
    'Copy the game data, or use it where it is?',
    'Copying (about 50 GB) makes the install self-contained. Leaving it in place saves the space, but that folder ' +
    'then has to stay where it is (not on a removable drive).',
    True, False);
  ModePage.Add('Copy the game data into the install folder (recommended)');
  ModePage.Add('Use the game data where it is');
  ModePage.Values[0] := ExpandConstant('{param:INPLACE|0}') <> '1';
  ModePage.Values[1] := not ModePage.Values[0];

  KeysPage := CreateInputFilePage(ModePage.ID, 'Package keys',
    'Where is the package keys file?',
    'Unpacking fake-signed .pkg files needs the two fake-package RSA keys (the fake image-key keyset and the ' +
    'derived-key-3 keyset). They don''t come with this setup: pick a file that holds them, as 1024 raw bytes or as ' +
    'hex text (a keys.inc in the format of keys.example.inc). Setup checks them, then keeps a copy in the game''s ' +
    'data folder for adding DLC later.');
  KeysPage.Add('Keys file:', 'Keys files (*.bin;*.inc;*.txt;*.h)|*.bin;*.inc;*.txt;*.h|All files|*.*', '.bin');
  // /PKGKEYS="<file>" and /DLC="<folder>" on the command line (for silent installs).
  KeysPage.Values[0] := ExpandConstant('{param:PKGKEYS|}');

  DlcPage := CreateInputDirPage(KeysPage.ID, 'DLC (optional)',
    'Add DLC packs now?',
    'If you have DLC (add-on pack) .pkg files for LEGO Dimensions, pick the folder that holds them; leave this empty ' +
    'to skip. Add-on packs in the game''s own .pkg folder are added either way. More can be added after setup with ' +
    'Start > Dimensions Rebricked > Add DLC packs.',
    False, '');
  DlcPage.Add('DLC folder (optional):');
  DlcPage.Values[0] := ExpandConstant('{param:DLC|}');

  ShadPage := CreateInputDirPage(wpSelectTasks, 'Import shadPS4 saves',
    'Where is shadPS4''s user folder?',
    'Pick shadPS4''s user folder (%APPDATA%\shadPS4, or the "user" folder next to shadPS4.exe), its savedata folder, ' +
    'or a single LEGO Dimensions save folder. Each LEGO Dimensions save found there replaces the save of the same ' +
    'slot here; the saves already here are copied to data\savedata-backups first.',
    False, '');
  ShadPage.Add('shadPS4 folder:');
  ShadPage.Values[0] := DefaultShadPS4;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  // Tick the import task once when a shadPS4 folder with LEGO Dimensions saves is found (or /SHADPS4= was given).
  if (CurPageID = wpSelectTasks) and not ShadOffered then begin
    ShadOffered := True;
    if (DefaultShadPS4 <> '') and HasLegoSaves(DefaultShadPS4, 6) then WizardSelectTasks('importshadps4');
  end;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := ((PageID = ShadPage.ID) and not WizardIsTaskSelected('importshadps4')) or
    ((PageID = ModePage.ID) and PkgMode) or
    (((PageID = KeysPage.ID) or (PageID = DlcPage.ID)) and not PkgMode);
end;

// Runs setup's ldpkg.exe (unpacked to {tmp}) with Args; Info gets what it printed. Returns its exit code (-1: no run).
function RunLdpkg(Args: String; var Info: AnsiString): Integer;
var
  Tmp: String;
begin
  ExtractTemporaryFile('ldpkg.exe');
  Tmp := ExpandConstant('{tmp}');
  if not Exec(ExpandConstant('{cmd}'), '/C ""' + Tmp + '\ldpkg.exe" ' + Args + ' > "' + Tmp + '\ldpkg-out.txt" 2>&1"',
      Tmp, SW_HIDE, ewWaitUntilTerminated, Result) then Result := -1;
  if not LoadStringFromFile(Tmp + '\ldpkg-out.txt', Info) then Info := '';
end;

// True when the folder holds at least one .pkg file.
function HasPkgs(Dir: String): Boolean;
var
  F: TFindRec;
begin
  Result := FindFirst(Dir + '\*.pkg', F);
  if Result then FindClose(F);
end;

// The keys page: "ldpkg --keys" checks the file (both key pairs must work), then "ldpkg --info" checks the game's
// .pkg folder with them: it fails unless the 1.24 update is there and can be unpacked.
function CheckKeysAndPkgs: Boolean;
var
  Code: Integer;
  Info: AnsiString;
begin
  Result := False;
  if (KeysSrc = '') or not FileExists(KeysSrc) then begin
    MsgBox('Pick the package keys file. Setup can''t unpack .pkg files without it.', mbError, MB_OK);
    Exit;
  end;
  Code := RunLdpkg('--keys "' + KeysSrc + '"', Info);
  if Code <> 0 then begin
    MsgBox('Those keys don''t work (ldpkg code ' + IntToStr(Code) + '):' + #13#10#13#10 + Info, mbError, MB_OK);
    Exit;
  end;
  Code := RunLdpkg('--keys "' + KeysSrc + '" --info "' + GameSrc('') + '"', Info);
  if Code = 0 then
    Result := True
  else
    MsgBox('These packages can''t be used yet (ldpkg code ' + IntToStr(Code) + '):' + #13#10#13#10 + Info + #13#10 +
      'The folder needs the 1.24 update package (EP1018-CUSA01176_00-LEGODIMENSIONS01-A0124-...), fake-signed; ' +
      'the base game 1.00 package (A0100) is recommended too.', mbError, MB_OK);
end;

// The DLC page: empty is fine; otherwise "ldpkg --dlc --info" must find at least one add-on pack it can unpack.
function CheckDlc: Boolean;
var
  Code: Integer;
  Info: AnsiString;
  Src: String;
begin
  Result := True;
  Src := RemoveBackslashUnlessRoot(Trim(DlcPage.Values[0]));
  if Src = '' then Exit;
  Result := False;
  if not DirExists(Src) then
    MsgBox('That folder does not exist:' + #13#10 + Src + #13#10#13#10 + 'Leave it empty to skip the DLC.', mbError, MB_OK)
  else if not HasPkgs(Src) then
    MsgBox('There are no .pkg files in' + #13#10 + Src + #13#10#13#10 + 'Leave it empty to skip the DLC.', mbError, MB_OK)
  else begin
    Code := RunLdpkg('--keys "' + KeysSrc + '" --dlc --info "' + Src + '"', Info);
    Result := Code = 0;
    if not Result then
      MsgBox('No usable DLC was found there (ldpkg code ' + IntToStr(Code) + '):' + #13#10#13#10 + Info, mbError, MB_OK);
  end;
end;

// "I have PKG files": ldpkg unpacks them (and the DLC folder's add-on packs) into {app}\game, the same layout as copied
// game data. Its console window shows the progress; the log is data\ldpkg.log. Files already unpacked are skipped, so
// running it again resumes. The keys file is kept as data\pkgkeys.bin for "Add DLC packs".
procedure UnpackPkgs;
var
  Code: Integer;
  Log, Args, Keys: String;
begin
  Log := ExpandConstant('{app}\data\ldpkg.log');
  Keys := ExpandConstant('{app}\data\pkgkeys.bin');
  ForceDirectories(ExpandConstant('{app}\data'));
  if not FileCopy(KeysSrc, Keys, False) then Keys := KeysSrc;
  Args := '--layout install --keys "' + Keys + '" --log "' + Log + '" -o "' + ExpandConstant('{app}\game') + '" "' +
    GameSrc('') + '"';
  if DlcSrc <> '' then Args := Args + ' "' + DlcSrc + '"';
  WizardForm.StatusLabel.Caption := 'Unpacking the PKG files (about 50 GB; the console window shows the progress)...';
  if not Exec(ExpandConstant('{app}\ldpkg.exe'), Args, ExpandConstant('{app}'), SW_SHOWNORMAL, ewWaitUntilTerminated,
      Code) or (Code <> 0) then
    SuppressibleMsgBox('The PKG files could not be unpacked (code ' + IntToStr(Code) + '). Details are in' + #13#10 +
      Log + #13#10#13#10 + 'To try again (it carries on where it stopped):' + #13#10 +
      '"' + ExpandConstant('{app}\ldpkg.exe') + '" ' + Args, mbError, MB_OK, IDOK);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Src: String;
begin
  Result := True;
  if (CurPageID = GamePage.ID) and PkgMode then begin
    Src := GameSrc('');
    if not DirExists(Src) then begin
      MsgBox('That folder does not exist:' + #13#10 + Src, mbError, MB_OK);
      Result := False;
    end else if not HasPkgs(Src) then begin
      MsgBox('There are no .pkg files in' + #13#10 + Src + #13#10#13#10 +
        'Pick the folder that holds the game''s .pkg files (the 1.24 update, and the base game 1.00).', mbError, MB_OK);
      Result := False;
    end;
  end else if CurPageID = KeysPage.ID then
    Result := CheckKeysAndPkgs
  else if CurPageID = DlcPage.ID then
    Result := CheckDlc
  else if CurPageID = GamePage.ID then begin
    Src := GameSrc('');
    if not FileExists(Src + '\elf\eboot.elf') then begin
      MsgBox('elf\eboot.elf was not found in' + #13#10 + Src + #13#10#13#10 +
        'Pick the folder that holds elf, CUSA01176 (and CUSA01176-base, addcont).', mbError, MB_OK);
      Result := False;
    end else if not DirExists(Src + '\CUSA01176\uroot') then begin
      MsgBox('CUSA01176\uroot (the unpacked 1.24 update) was not found in' + #13#10 + Src, mbError, MB_OK);
      Result := False;
    end else if not DirExists(Src + '\CUSA01176-base\uroot') then
      MsgBox('Note: CUSA01176-base\uroot (base game 1.00) is not there. The game runs from the update alone ' +
        'only if it holds every file; the base game is recommended.', mbInformation, MB_OK);
  end else if CurPageID = ShadPage.ID then begin
    if not DirExists(ShadSrc) then begin
      MsgBox('That folder does not exist:' + #13#10 + ShadSrc, mbError, MB_OK);
      Result := False;
    end else if not HasLegoSaves(ShadSrc, 6) then begin
      MsgBox('No LEGO Dimensions saves were found in' + #13#10 + ShadSrc + #13#10#13#10 +
        'shadPS4 keeps them in user\savedata\<user id>\CUSA01176\. Pick that folder or one above it.', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

// Runs ldport.exe --import-shadps4, which backs up the saves already installed and then copies shadPS4's in.
procedure ImportShadPS4Saves;
var
  Code: Integer;
  Log: String;
begin
  Log := ExpandConstant('{app}\data\shadps4-import.log');
  WizardForm.StatusLabel.Caption := 'Importing the shadPS4 saves...';
  if not Exec(ExpandConstant('{cmd}'), '/C ""' + ExpandConstant('{app}\ldport.exe') + '" --import-shadps4 "' + ShadSrc +
      '" --data "' + ExpandConstant('{app}\data') + '" > "' + Log + '" 2>&1"', ExpandConstant('{app}'), SW_HIDE,
      ewWaitUntilTerminated, Code) or (Code <> 0) then
    SuppressibleMsgBox('The shadPS4 saves could not be imported (code ' + IntToStr(Code) + '). Details are in' + #13#10 +
      Log + #13#10#13#10 + 'To try again later:' + #13#10 +
      'ldport.exe --import-shadps4 "<shadPS4 folder>" --data "' + ExpandConstant('{app}\data') + '"',
      mbError, MB_OK, IDOK);
end;

// Sets "key=value" in data\settings.ini (a plain key=value file), replacing the key's line or adding one.
procedure SetPortSetting(Key, Value: String);
var
  Path: String;
  Lines: TArrayOfString;
  I, N: Integer;
  Found: Boolean;
begin
  Path := ExpandConstant('{app}\data\settings.ini');
  if not LoadStringsFromFile(Path, Lines) then SetArrayLength(Lines, 0);
  N := GetArrayLength(Lines);
  Found := False;
  for I := 0 to N - 1 do
    if Pos(Key + '=', Lines[I]) = 1 then begin
      Lines[I] := Key + '=' + Value;
      Found := True;
    end;
  if not Found then begin
    SetArrayLength(Lines, N + 1);
    Lines[N] := Key + '=' + Value;
  end;
  SaveStringsToFile(Path, Lines, False);
end;

// The "legotoypad" task: the game takes its figures from LegoToypad (settings.ini toypad=2) once it is there.
procedure UseLegoToypad;
begin
  if not FileExists(ExpandConstant('{app}\LegoToypad\LegoToypad.exe')) then Exit;
  SetPortSetting('toypad', '2');
  if WizardIsTaskSelected('legotoypad\start') then SetPortSetting('legotoypad_start', '1')
  else SetPortSetting('legotoypad_start', '0');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('legotoypad') then
    UseLegoToypad;
  if (CurStep = ssPostInstall) and PkgMode then
    UnpackPkgs
  else if (CurStep = ssPostInstall) and not CopyGame then
    SaveStringToFile(ExpandConstant('{app}\game.txt'), GameSrc('') + #13#10, False);
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('importshadps4') then
    ImportShadPS4Saves;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo,
  MemoGroupInfo, MemoTasksInfo: String): String;
var
  How: String;
begin
  if PkgMode then How := '.pkg files, unpacked into the install folder (about 50 GB; this takes a while)'
  else if CopyGame then How := 'copied into the install folder (about 50 GB; this takes a while)'
  else How := 'used where it is';
  Result := MemoDirInfo + NewLine + NewLine + 'Game data:' + NewLine + Space + GameSrc('') + NewLine + Space + How +
    NewLine + NewLine + MemoTasksInfo;
  if PkgMode then begin
    Result := Result + NewLine + NewLine + 'Package keys:' + NewLine + Space + KeysSrc;
    if DlcSrc <> '' then Result := Result + NewLine + NewLine + 'DLC (add-on packs) from:' + NewLine + Space + DlcSrc
    else Result := Result + NewLine + NewLine + 'DLC: only add-on packs in the game''s .pkg folder (more can be ' +
      'added later with Start > Dimensions Rebricked > Add DLC packs)';
  end;
  if WizardIsTaskSelected('importshadps4') then
    Result := Result + NewLine + NewLine + 'shadPS4 saves imported from:' + NewLine + Space + ShadSrc;
  if WizardIsTaskSelected('legotoypad') then
    Result := Result + NewLine + NewLine + 'LegoToypad (MIT licence) is downloaded from:' + NewLine + Space +
      '{#LegoToypadUrl}';
end;
