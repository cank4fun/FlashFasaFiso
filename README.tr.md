# FlashFasaFiso

[English](README.md) | [Türkçe](README.tr.md)

FlashFasaFiso, yerelde sakladığınız Flash oyunları için taşınabilir bir başlatıcıdır. Arşivinizde arama yapabilir, oyunları favorilere ekleyebilir ve ayrı bir Ruffle Desktop penceresinde oynayabilirsiniz.

## İlk kullanım

En güncel Windows x64 taşınabilir ZIP paketini [GitHub Releases sayfasından](https://github.com/cank4fun/FlashFasaFiso/releases/latest) indirin. **Windows 11 x64** üzerinde arşivi çıkarıp `FlashFasaFiso.exe` dosyasını açın. Pakette Ruffle Desktop 0.8.0 bulunur. Başlatıcı, yazılabilir bir klasörde gerektiğinde `games/` ve `data/` dizinlerini oluşturur.

Paket oyun dosyası içermez. Kullanım hakkınız olan SWF dosyalarını her oyun için ayrı bir klasör açarak `games/` altına ekleyin. Yan dosyaları yerinde bırakın ve **F5** tuşuna basın ya da **Yeniden Tara** düğmesine tıklayın.

```text
FlashFasaFiso/
  FlashFasaFiso.exe
  runtime/ruffle.exe
  games/Örnek Oyun/oyun.swf
  data/
```

## Oyun kütüphanesi

**Ctrl+F** aramayı açar, **Ctrl+D** favori durumunu değiştirir, **Enter** oyunu başlatır. Birden fazla SWF varsa **Giriş Dosyasını Seç** düğmesini kullanın.

Başlatıcı önce oyun klasörünün kökündeki SWF’lere bakar. Kökte SWF yoksa ve alt klasör taraması açıksa iki alt seviyeye kadar arar. İsteğe bağlı UTF-8 `game.ini` dosyası `title`, `genre`, `entry` ve `description` alanlarını kabul eder. `entry` yolunu oyun klasörüne göreli olarak `/` ayırıcıyla yazın. Kapak adları `cover.png`, `cover.jpg` veya `cover.jpeg` olabilir. WIC WebP çözümleyebiliyorsa `cover.webp` de kullanılabilir.

Başlatıcı kapansa da oyun açık kalır. Aynı klasörden başka oyun açmadan önce Ruffle’ı kapatın. Ruffle pencere açmazsa **Başlatmayı İptal Et** normal kapatma isteği gönderir. Zorla sonlandırma ikinci bir onay gerektirir.

## Ayarlar ve kayıtlar

Yeni profiller İngilizce açılır. **Ayarlar** bölümünde İngilizce veya Türkçe seçebilirsiniz. Dil hemen değişir ve sonraki açılışta korunur. Windows menüleri, Ruffle ve oyun içeriğinin dil ayarları ayrıdır.

| Yol | İçerik |
| --- | --- |
| `data/settings.ini` | Tercihler ve son seçim |
| `data/favorites.ini` | Favoriler |
| `data/library.ini` | Kayıtlı SWF seçimleri |
| `data/saves/` | Ruffle oyun kayıtları |
| `data/ruffle-config/` | Ruffle tercihleri |
| `data/cache/ruffle/log/` | Ruffle günlükleri |

**Önbelleği Temizle**, katalog önbelleğini ve bellekteki kapakları temizler. Ayar ve kayıtlarınızın da taşınması için FlashFasaFiso klasörünü bütün olarak kopyalayın. Oyun klasörünü veya giriş SWF’sini yeniden adlandırmak oyun kimliğini ya da kayıt anahtarını değiştirebilir.

## Derleme ve test

MSVC x64 araçları ve Windows SDK ile Visual Studio 2022 17.14, ayrıca CMake 3.25 veya üzeri gerekir. Komutları kaynak dizininde PowerShell ile çalıştırın.

```powershell
.\build-windows.ps1
.\build-windows.ps1 -Configuration Debug
.\build-windows.ps1 -Package
```

Betik derleme yapar ve CTest’i çalıştırır. `-Package`, gerekirse sabit Ruffle sürümünü indirir, arşivin SHA-256 değerini doğrular ve `out/FlashFasaFiso/` klasörünü hazırlar. Paketlemeden sonra etkileşimli bir Windows oturumunda `powershell.exe -NoProfile -File tests/verify-windows.ps1` komutunu çalıştırın. Regresyon testleri örnek SWF ve katalog üretmek için Python 3 kullanır.

Başlatıcı C++23, Win32, Direct2D ve DirectWrite kullanır. Kütüphane taraması ve Windows Imaging Component (WIC) ile kapak çözümleme arka planda çalışır. Her yeni arayüz dili için eksiksiz bir `README.<dil-kodu>.md` ekleyin.

## Uyumluluk ve lisans

Uyumluluk oyuna göre değişir. Başlatıcı ağ isteği yapmaz. Ruffle, SWF’nin TCP soketlerini ve tarayıcı yönlendirmelerini engelleyen ayarlarla açılır. Haricî HTTP(S) kaynaklarına bağlı oyunlar ağ bağlantısı gerektirebilir. Ruffle’ın etkileşimsiz dosya erişimi giriş SWF’sinin diziniyle sınırlıdır. Giriş dosyası alt klasörde olan oyunlar bundan etkilenebilir. Taşınabilir kayıt URL’si de özgün mutlak URL’ye bağlı oyunları etkileyebilir.

Ruffle 0.8.0 açılış günlüğü için yazılabilir bir `data/` dizini ister. Sürücü harfi değişikliği `SUBST` ile denendi. Windows 10 ve fiziksel USB bellek test edilmedi.

FlashFasaFiso’nun kodu [MIT lisansıyla](LICENSE) sunulur. MIT veya Apache-2.0 ile kullanılabilen Ruffle’ın [lisans ve üçüncü taraf bildirimleri](LICENSES/Ruffle-LICENSE.md) ile [kaynak bağlantıları](LICENSES/Ruffle-SOURCE.md) ayrı dosyalardadır. Oyunların ve varlıklarının telif hakları ayrıdır.
