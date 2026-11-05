; Windows installer, built by build.ps1 with Inno Setup 6 from the packaged program folder.
; The same files also ship as a portable zip; the installer adds Start menu and desktop shortcuts and an
; uninstaller, and installs for the current user or, with administrator rights, for everyone.
#ifndef Version
  #error Pass /DVersion=<version> /DSource=<program folder> /DOutput=<folder> /DOutputName=<file name>
#endif

[Setup]
AppId={{6F0C6C5E-2B7A-4B7D-9C4E-3C1D2B5A7E91}
AppName=Compareel
AppVersion={#Version}
AppVerName=Compareel {#Version}
AppPublisher=Ensar Yanar
AppPublisherURL=https://github.com/turxlab/compareel
AppSupportURL=https://github.com/turxlab/compareel/issues
AppUpdatesURL=https://github.com/turxlab/compareel/releases
AppCopyright=© 2026 Ensar Yanar
VersionInfoVersion={#FileVersion}
VersionInfoDescription=Compareel Setup
DefaultDirName={autopf}\Compareel
DefaultGroupName=Compareel
DisableProgramGroupPage=yes
; "Only for me" needs no administrator rights; the dialog offers "for all users" as well.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
LicenseFile={#Source}\LICENSE.txt
SetupIconFile=..\..\src\qt\branding\app.ico
UninstallDisplayIcon={app}\Compareel.exe
UninstallDisplayName=Compareel
WizardStyle=modern
ShowLanguageDialog=auto
CloseApplications=yes
Compression=lzma2/max
SolidCompression=yes
OutputDir={#Output}
OutputBaseFilename={#OutputName}

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ar"; MessagesFile: "compiler:Languages\Arabic.isl"
Name: "bg"; MessagesFile: "compiler:Languages\Bulgarian.isl"
Name: "ca"; MessagesFile: "compiler:Languages\Catalan.isl"
Name: "co"; MessagesFile: "compiler:Languages\Corsican.isl"
Name: "cs"; MessagesFile: "compiler:Languages\Czech.isl"
Name: "da"; MessagesFile: "compiler:Languages\Danish.isl"
Name: "de"; MessagesFile: "compiler:Languages\German.isl"
Name: "es"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "fi"; MessagesFile: "compiler:Languages\Finnish.isl"
Name: "fr"; MessagesFile: "compiler:Languages\French.isl"
Name: "hu"; MessagesFile: "compiler:Languages\Hungarian.isl"
Name: "it"; MessagesFile: "compiler:Languages\Italian.isl"
Name: "ja"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "ko"; MessagesFile: "compiler:Languages\Korean.isl"
Name: "nb"; MessagesFile: "compiler:Languages\Norwegian.isl"
Name: "nl"; MessagesFile: "compiler:Languages\Dutch.isl"
Name: "pl"; MessagesFile: "compiler:Languages\Polish.isl"
Name: "ptbr"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "sk"; MessagesFile: "compiler:Languages\Slovak.isl"
Name: "sl"; MessagesFile: "compiler:Languages\Slovenian.isl"
Name: "sv"; MessagesFile: "compiler:Languages\Swedish.isl"
Name: "th"; MessagesFile: "compiler:Languages\Thai.isl"
Name: "tr"; MessagesFile: "compiler:Languages\Turkish.isl"
Name: "uk"; MessagesFile: "compiler:Languages\Ukrainian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#Source}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Compareel"; Filename: "{app}\Compareel.exe"
Name: "{autodesktop}\Compareel"; Filename: "{app}\Compareel.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Compareel.exe"; Description: "{cm:LaunchProgram,Compareel}"; Flags: nowait postinstall skipifsilent
