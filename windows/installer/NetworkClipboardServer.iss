#define MyAppName "Network Clipboard Server"
#define MyAppVersion "0.1.0"
#define MyAppPublisher "LocalTools"
#define MyAppExeName "NetworkClipboardService.exe"
#define MyTrayExeName "NetworkClipboardWindows.exe"
#define MyServiceName "NetworkClipboardServer"
#define MySourceDir "..\deploy"

[Setup]
AppId={{6BB9C6A0-1487-4D4D-9D2B-80953C2C860E}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\NetworkClipboard
DefaultGroupName=Network Clipboard
DisableProgramGroupPage=yes
PrivilegesRequired=admin
OutputDir=..\installer-output
OutputBaseFilename=NetworkClipboardServerSetup
Compression=lzma
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64

[Files]
Source: "{#MySourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Run]
Filename: "{sys}\sc.exe"; Parameters: "stop {#MyServiceName}"; Flags: runhidden waituntilterminated
Filename: "{sys}\sc.exe"; Parameters: "delete {#MyServiceName}"; Flags: runhidden waituntilterminated
Filename: "{sys}\sc.exe"; Parameters: "create {#MyServiceName} binPath= ""{app}\{#MyAppExeName}"" start= auto DisplayName= ""{#MyAppName}"""; Flags: runhidden waituntilterminated
Filename: "{sys}\sc.exe"; Parameters: "description {#MyServiceName} ""Local LAN clipboard HTTP server on port 8787."""; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Network Clipboard Server"""; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Network Clipboard Server"" dir=in action=allow protocol=TCP localport=8787"; Flags: runhidden waituntilterminated
Filename: "{sys}\sc.exe"; Parameters: "start {#MyServiceName}"; Flags: runhidden waituntilterminated
Filename: "{app}\{#MyTrayExeName}"; Description: "Start Network Clipboard tray agent"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{sys}\sc.exe"; Parameters: "stop {#MyServiceName}"; Flags: runhidden waituntilterminated
Filename: "{sys}\sc.exe"; Parameters: "delete {#MyServiceName}"; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Network Clipboard Server"""; Flags: runhidden waituntilterminated

[Icons]
Name: "{group}\Service configuration"; Filename: "{app}\NetworkClipboardService.ini"
Name: "{group}\Network Clipboard Agent"; Filename: "{app}\{#MyTrayExeName}"
Name: "{autostartup}\Network Clipboard Agent"; Filename: "{app}\{#MyTrayExeName}"
