# MidnightSun — Tanıtım Metni

*(Web sitesi / mağaza sayfası / sosyal medya için hazır metinler)*

---

## Slogan seçenekleri

- **Oyunun orada. Sen neredeysen orada oyna.**
- Gecikmeyi hissetmeyeceksin: kendi ağında, kendi kuralınla oyun yayını.
- Bir tık yayın, bir tık bağlan. IP yok, kurulum derdi yok.

## Kısa açıklama (mağaza/liste görünümü, ~50 kelime)

MidnightSun, güçlü PC'nizdeki oyunu evdeki başka bir bilgisayardan
milisaniyeler mertebesinde gecikmeyle oynatan bir yayın uygulamasıdır.
GPU'dan çıkmayan NVENC kodlama, özel UDP aktarımı, tam klavye/fare/oyun
kolu desteği ve isteğe bağlı uçtan uca şifreleme — hepsi tek, kurulumu
saniyeler süren uygulamada.

## Uzun açıklama (web sitesi "hakkında" bölümü)

Oyun bilgisayarınız çalışma odasında, siz salondaki koltuktasınız.
MidnightSun tam bu an için var: yayıncı bilgisayarda tek tıkla yayını
başlatın, salondaki cihazda listede beliren yayına tıklayın, oynamaya
başlayın. IP adresi yazmak, yapılandırma dosyası düzenlemek, ağ ayarıyla
boğuşmak yok.

Teknik taraf iddialı: ekran görüntüsü DXGI ile yakalanır, görüntü verisi
GPU belleğinden hiç çıkmadan NVENC ile H.264'e sıkıştırılır ve MidnightSun'ın
oyun yayını için tasarlanmış UDP protokolüyle (GSMR) karşı tarafa akar.
Kayıp paketler anahtar kare istekleri ve intra-refresh ile görüntüyü
kendiliğinden onarır; uyarlanabilir bit hızı denetleyicisi ağ koşuluna göre
kaliteyi saniyeler içinde ayarlar. Ses Opus ile 20 ms'lik dilimler halinde
taşınır; klavye, fare ve oyun kolu girişleriniz saniyede 120 kez yayıncıya
iletilir.

Kontrol sizde: varsayılan olarak yayına kimin bağlanacağına siz karar
verirsiniz ("İzin ver / Reddet"), dilerseniz iki uca gireceğiniz tek bir
şifreyle tüm trafiği AES-256-GCM ile şifrelersiniz.

## Öne çıkan 6 madde (özellik vitrini)

1. **Sıfır kurulum eşleşme** — aynı ağda otomatik keşif; uzaktakiler için
   `XXXX-XXXX-XXXX` kodu.
2. **Milisaniye takıntısı** — zero-copy NVENC, tek karelik VBV, 120 Hz
   giriş aktarımı.
3. **Kapıda onay** — izleyiciyi siz onaylarsınız; şifre (PSK) ile trafik
   uçtan uca şifrelenir.
4. **Tam oyun hissi** — mutlak/göreli fare modları, 256 tuş, XInput oyun
   kolu (sanal Xbox 360 kolu olarak).
5. **Kendini onaran görüntü** — paket kaybında anahtar kare + intra-refresh;
   ağ daralınca bit hızı kendiliğinden iner, düzelince çıkar.
6. **Göz önünde değil, tepside** — yayın penceresi sistem tepsisine küçülür;
   istatistikler tek tuş (F1) uzağınızda.

## SSS (kısa)

**İnternet üzerinden çalışır mı?** Aynı ağ, VPN (Tailscale/WireGuard vb.)
veya port yönlendirmesiyle çalışır; sunucu kiralamaz, veriniz üçüncü
tarafa uğramaz.

**Hangi ekran kartları?** Yayıncıda NVENC'li NVIDIA (GTX 10 serisi ve
üzeri). İzleyici tarafında H.264 çözebilen herhangi bir GPU.

**Ücretli mi?** Hayır. MidnightSun ücretsiz ve açık kaynaklıdır (MIT
lisansı); kaynak kodu GitHub'da.

**Verilerim güvende mi?** Yayın yalnızca sizin seçtiğiniz cihaza akar;
onay istemi ve isteğe bağlı AES-256-GCM şifreleme yerleşiktir. Hesap,
telemetri, bulut yoktur.

---

*Ekran görüntüsü önerisi: (1) launcher — cihaz listesi dolu halde,
(2) durum penceresi — eşleşme kodu görünür, (3) onay istemi,
(4) oyun içi F1 istatistik katmanı.*
