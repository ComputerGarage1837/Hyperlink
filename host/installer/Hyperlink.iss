; Hyperlink for Windows installer (Inno Setup 6).
; iscc /DAppVersion=0.1.0 /DSourceDir=<folder with HyperlinkHost.exe and DLLs> /DOutputDir=<out> Hyperlink.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{6B7D3C1E-2F4A-4E8B-9C1D-7A5E3B2F9D10}
AppName=Hyperlink
AppVersion={#AppVersion}
AppVerName=Hyperlink {#AppVersion}
AppPublisher=ComputerGarage1837
AppPublisherURL=https://github.com/ComputerGarage1837/Hyperlink
DefaultDirName={autopf}\Hyperlink
DefaultGroupName=Hyperlink
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=Hyperlink-Setup-{#AppVersion}
SetupIconFile=..\res\hyperlink.ico
UninstallDisplayIcon={app}\HyperlinkHost.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no

[Tasks]
Name: "autostart"; Description: "Start Hyperlink Host when I sign in (runs with admin rights so it can control admin windows)"; Flags: checkedonce
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{group}\Hyperlink Host"; Filename: "{app}\HyperlinkHost.exe"
Name: "{autodesktop}\Hyperlink Host"; Filename: "{app}\HyperlinkHost.exe"; Tasks: desktopicon

[Run]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Hyperlink Host"""; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Hyperlink Host"" dir=in action=allow program=""{app}\HyperlinkHost.exe"" enable=yes profile=any"; Flags: runhidden
Filename: "{sys}\schtasks.exe"; Parameters: "/Delete /TN ""Hyperlink Host"" /F"; Flags: runhidden
Filename: "{sys}\schtasks.exe"; Parameters: "/Create /TN ""Hyperlink Host"" /TR ""\""{app}\HyperlinkHost.exe\"""" /SC ONLOGON /RL HIGHEST /F"; Flags: runhidden; Tasks: autostart
Filename: "{app}\HyperlinkHost.exe"; Description: "Start Hyperlink Host"; Flags: nowait postinstall

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/IM HyperlinkHost.exe /F"; Flags: runhidden; RunOnceId: "KillHost"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Hyperlink Host"""; Flags: runhidden; RunOnceId: "DelFirewall"
Filename: "{sys}\schtasks.exe"; Parameters: "/Delete /TN ""Hyperlink Host"" /F"; Flags: runhidden; RunOnceId: "DelTask"
