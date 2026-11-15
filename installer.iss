; installer.iss -- MidnightSun Inno Setup betiği
;
; Derleme: Inno Setup 6 (https://jrsoftware.org/isinfo.php) kurulu bir
; makinede önce build_all.bat ile build\Release\MidnightSun.exe üretin,
; tools\package.ps1 ile taşınabilir klasörü (dist\MidnightSun) hazırlayın,
; sonra bu dosyayı ISCC.exe ile derleyin:
;
;   powershell -ExecutionPolicy Bypass -File tools\package.ps1
;   ISCC.exe installer.iss
;
; Kaynak klasör farklıysa:  ISCC.exe /DSrcDir=yol\MidnightSun installer.iss
; Çıktı: Output\MidnightSun-Kurulum.exe (sabit ad: GitHub Releases
; "latest/download" bağlantıları sürümden bağımsız çalışsın diye)
;
; Kurulum yaptıkları:
;   - MidnightSun.exe + Visual C++ çalışma zamanı DLL'leri (uygulama yanında)
;     + midnightsun_config.json + lisans dosyaları -> {app}
;   - Başlat menüsü + isteğe bağlı masaüstü kısayolu (çalışma klasörü {app};
;     uygulama ayar dosyasını çalışma klasöründe arar)
;   - Ayar dosyasına normal kullanıcı yazma izni (Program Files altında
;     uygulama içi "Kaydet" ve kayıtlı cihazlar yönetici olmadan çalışsın)
;   - Windows Güvenlik Duvarı: uygulamaya gelen UDP izni (keşif 47800 +
;     yayın portu; program bazlı kural olduğundan port değişse de çalışır)
;   - Kaldırırken kural ve kısayollar temizlenir (config dosyasına
;     dokunulmaz; kullanıcı ayarları kalır)

#define AppName    "MidnightSun"
#define AppVersion "1.0.0"
#define AppExe     "MidnightSun.exe"
#ifndef SrcDir
  #define SrcDir   "dist\MidnightSun"
#endif

[Setup]
AppId={{7A1C6F0D-6B2E-4E7B-9C1D-3D5A0B8E4F21}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=ozcinax-star
AppPublisherURL=https://github.com/ozcinax-star/MidnightSun
AppSupportURL=https://github.com/ozcinax-star/MidnightSun/issues
AppUpdatesURL=https://github.com/ozcinax-star/MidnightSun/releases
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
UninstallDisplayIcon={app}\{#AppExe}
SetupIconFile=app.ico
LicenseFile=LICENSE
OutputDir=Output
OutputBaseFilename={#AppName}-Kurulum
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
WizardStyle=modern
PrivilegesRequired=admin

; Türkçe kurulum arayüzü
[Languages]
Name: "turkish"; MessagesFile: "compiler:Languages\Turkish.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; \
    GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SrcDir}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcDir}\msvcp140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcDir}\vcruntime140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcDir}\vcruntime140_1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcDir}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcDir}\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
; Config yalnızca ilk kurulumda kopyalanır; güncellemede kullanıcının
; ayarlarının üzerine yazılmaz. users-modify: uygulama Program Files
; altında yönetici olmadan ayarları kaydedebilsin.
Source: "{#SrcDir}\midnightsun_config.json"; DestDir: "{app}"; \
    Flags: onlyifdoesntexist uninsneveruninstall; Permissions: users-modify

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; \
    WorkingDir: "{app}"; Tasks: desktopicon

[Run]
; Güvenlik duvarı: program bazlı gelen-UDP izni (LAN keşfi 47800 + GSMR
; yayın portu). Program bazlı kural, kullanıcı portu değiştirse de geçerli.
; Önce eski kural silinir ki yeniden kurulumda kopya kural birikmesin.
Filename: "{sys}\netsh.exe"; \
    Parameters: "advfirewall firewall delete rule name=""{#AppName}"""; \
    Flags: runhidden
Filename: "{sys}\netsh.exe"; \
    Parameters: "advfirewall firewall add rule name=""{#AppName}"" dir=in action=allow program=""{app}\{#AppExe}"" protocol=UDP profile=private,domain"; \
    Flags: runhidden
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; \
    WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{sys}\netsh.exe"; \
    Parameters: "advfirewall firewall delete rule name=""{#AppName}"""; \
    Flags: runhidden; RunOnceId: "DelFirewallRule"
