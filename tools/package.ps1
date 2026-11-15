# tools/package.ps1 -- taşınabilir dağıtım paketini hazırlar.
#
# Önce build_all.bat ile build\Release\MidnightSun.exe üretin, sonra:
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1
#
# Çıktı:
#   dist\MidnightSun\                 (installer.iss bu klasörü paketler)
#   dist\MidnightSun-Portable.zip     (GitHub Release'e yüklenen zip)
#
# Paket içeriği: MidnightSun.exe, varsayılan midnightsun_config.json,
# Visual C++ 2015-2022 çalışma zamanı DLL'leri (uygulama yanında; hedef
# PC'de VC++ Redistributable kurulu olmasa da açılsın diye), LICENSE.txt,
# THIRD_PARTY_NOTICES.txt, OKUBENI.txt.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root 'build\Release\MidnightSun.exe'
if (-not (Test-Path $exe)) { throw "Önce build_all.bat çalıştırın: $exe bulunamadı." }

# VC++ redist klasörü: vswhere ile en yeni VS kurulumunu bul
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe bulunamadı (Visual Studio 2022 gerekli).' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$crt = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC') -Directory |
       Sort-Object Name -Descending |
       ForEach-Object { Join-Path $_.FullName 'x64\Microsoft.VC143.CRT' } |
       Where-Object { Test-Path (Join-Path $_ 'vcruntime140.dll') } |
       Select-Object -First 1
if (-not $crt) { throw 'Microsoft.VC143.CRT redist klasörü bulunamadı.' }

$dist = Join-Path $root 'dist'
$out  = Join-Path $dist 'MidnightSun'
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force $out | Out-Null

Copy-Item $exe $out
Copy-Item (Join-Path $root 'midnightsun_config.json') $out
foreach ($dll in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
    Copy-Item (Join-Path $crt $dll) $out
}
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $out 'LICENSE.txt')
Copy-Item (Join-Path $root 'THIRD_PARTY_NOTICES.txt') $out

$readme = @'
MidnightSun 1.0.0 - tasinabilir paket (Windows 10/11 x64)
===========================================================

Calistirma: MidnightSun.exe'ye cift tiklayin. Ayarlar ayni klasordeki
midnightsun_config.json dosyasina yazilir; klasoru yazilabilir bir yere
(ornegin Belgeler veya Masaustu) cikarin.

Ilk calistirmada Windows Guvenlik Duvari sorarsa "Ozel aglar" icin izin
verin (UDP 47800 kesif + yayin portu, varsayilan 9000).

Uygulama imzasizdir: Windows SmartScreen "Windows bilgisayarinizi korudu"
derse "Ek bilgi" > "Yine de calistir".

Gereksinimler: yayinci PC'de NVENC destekli NVIDIA GPU (GTX 10xx ve
uzeri, surucu >= 522.25). Izleyici PC'de H.264 cozebilen herhangi bir GPU.
Oyun kolu icin yayinci PC'ye ViGEmBus surucusu (istege bagli):
https://github.com/nefarius/ViGEmBus/releases

Kullanim: yayinci PC'de "Bu bilgisayari yayinla"; izleyici PC'de listede
beliren yayina tiklayin ya da eslesme kodunu girin. F1 istatistik,
F2 fare modu, Shift+ESC oturumdan cikis.

Kaynak kod ve belgeler: https://github.com/ozcinax-star/MidnightSun
Lisans: MIT (LICENSE.txt). Ucuncu taraf bilesenler: THIRD_PARTY_NOTICES.txt
'@
Set-Content -Path (Join-Path $out 'OKUBENI.txt') -Value $readme -Encoding ASCII

$zip = Join-Path $dist 'MidnightSun-Portable.zip'
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $out -DestinationPath $zip -CompressionLevel Optimal

Get-ChildItem $out | Format-Table Name, Length -AutoSize
Get-FileHash $zip -Algorithm SHA256 | Format-List Path, Hash
