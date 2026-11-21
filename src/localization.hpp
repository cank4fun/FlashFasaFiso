#pragma once
#include <string>
#include <string_view>

namespace fff {
enum class Language { tr, en };
inline constexpr auto defaultLanguage = Language::en;

#define FFF_TEXTS(X) \
    X(empty, L"", L"") \
    X(library, L"Kütüphane", L"Library") \
    X(favorites, L"Favoriler", L"Favorites") \
    X(settings, L"Ayarlar", L"Settings") \
    X(gamesFolder, L"Oyun Klasörü", L"Games Folder") \
    X(openGames, L"Oyun Klasörünü Aç", L"Open Games Folder") \
    X(libraryTitle, L"Oyun Kütüphanesi", L"Game Library") \
    X(search, L"Oyun adı veya tür ara", L"Search by game or genre") \
    X(game, L"Oyun", L"Game") \
    X(scanning, L"Kütüphane taranıyor…", L"Scanning library…") \
    X(noResults, L"Sonuç bulunamadı", L"No results") \
    X(noFavorites, L"Henüz favori yok", L"No favorites yet") \
    X(noGames, L"Henüz oyun yok", L"No games yet") \
    X(searchTip, L"Başka bir ad veya tür arayın.", L"Try another name or genre.") \
    X(favoritesTip, L"Oyun ayrıntılarından favori ekleyin.", L"Add a favorite from the game details.") \
    X(gamesTip, L"Bir oyun klasörünü games dizinine ekleyin.", L"Add a game folder to the games directory.") \
    X(details, L"Oyun ayrıntıları", L"Game details") \
    X(selectGame, L"Listeden bir oyun seçin.", L"Select a game from the list.") \
    X(folderName, L"Klasör  {}", L"Folder  {}") \
    X(entryRequired, L"SWF  Seçim gerekli", L"SWF  Selection required") \
    X(play, L"Oyna", L"Play") \
    X(playing, L"Ruffle Açık", L"Ruffle Running") \
    X(cancelStartup, L"Başlatmayı İptal Et", L"Cancel Startup") \
    X(forceStartup, L"Ruffle İşlemini Sonlandır", L"End Ruffle Process") \
    X(openFolder, L"Klasörü Aç", L"Open Folder") \
    X(removeFavorite, L"Favoriden Çıkar", L"Remove Favorite") \
    X(addFavorite, L"Favoriye Ekle", L"Add Favorite") \
    X(chooseEntry, L"Giriş Dosyasını Seç", L"Choose Entry File") \
    X(choosePrompt, L"Bu oyun için başlatılacak SWF dosyasını seçin.", L"Select the SWF to launch for this game.") \
    X(choose, L"Seç", L"Select") \
    X(cancel, L"Vazgeç", L"Cancel") \
    X(ok, L"Tamam", L"OK") \
    X(close, L"Kapat", L"Close") \
    X(closeTitle, L"FlashFasaFiso kapatılsın mı?", L"Close FlashFasaFiso?") \
    X(closePrompt, L"Açıksa Ruffle çalışmaya devam eder.", L"Any running Ruffle window will stay open.") \
    X(general, L"Genel", L"General") \
    X(language, L"Dil", L"Language") \
    X(turkish, L"Türkçe", L"Türkçe") \
    X(english, L"English", L"English") \
    X(autoScan, L"Açılışta oyunları tara", L"Scan for games on startup") \
    X(rememberTab, L"Son kullanılan sekmeyi hatırla", L"Remember the last tab") \
    X(confirmClose, L"Kapatırken onay iste", L"Confirm on close") \
    X(scanNested, L"Alt klasörlerde SWF ara", L"Look for SWFs in subfolders") \
    X(rescan, L"Yeniden Tara", L"Rescan") \
    X(playback, L"Oynatma", L"Playback") \
    X(ruffleFound, L"Ruffle Desktop bulundu", L"Ruffle Desktop found") \
    X(ruffleMissing, L"Ruffle Desktop eksik", L"Ruffle Desktop missing") \
    X(rememberSwf, L"SWF seçimini hatırla", L"Remember the SWF selection") \
    X(portable, L"Taşınabilir veri", L"Portable data") \
    X(savesFolder, L"Kayıt klasörü", L"Save folder") \
    X(openSaves, L"Kayıtları Aç", L"Open Saves") \
    X(clearCache, L"Önbelleği Temizle", L"Clear Cache") \
    X(cacheCleared, L"Launcher önbelleği temizlendi.", L"Launcher cache cleared.") \
    X(cacheClearFailed, L"Launcher önbelleği temizlenemedi.", L"Could not clear the launcher cache.") \
    X(settingsUnsaved, L"Ayarlar diske yazılamadı. Değişiklikler bu oturumda geçerli.", L"Settings could not be written. Changes apply to this session.") \
    X(settingsSaved, L"Ayarlar kaydedildi.", L"Settings saved.") \
    X(graphicsFailed, L"Grafik yüzeyi oluşturulamadı.", L"Could not create the graphics surface.") \
    X(workerFailed, L"Arka plan kataloğu durdu. Uygulamayı yeniden açın.", L"The background catalog stopped. Restart the application.") \
    X(loading, L"Kütüphane açılıyor…", L"Loading library…") \
    X(found, L"Bulunan oyun: {}", L"Games found: {}") \
    X(scanManually, L" · Tarama için F5", L" · Press F5 to scan") \
    X(cacheFallback, L" · games dizini okunamıyor. Önbellek gösteriliyor.", L" · Cannot read the games folder. Showing the cache.") \
    X(incomplete, L" · Tarama eksik. Klasör erişimini kontrol edin.", L" · Scan incomplete. Check folder access.") \
    X(scanLimited, L" · Tarama oyun veya SWF sınırına ulaştı.", L" · The scan reached its game or SWF limit.") \
    X(badMetadata, L" · Geçersiz game.ini: {}", L" · Invalid game.ini files: {}") \
    X(dataWrite, L" · data yazılamadı", L" · Could not write to data") \
    X(stateDamaged, L" · Geçersiz kişisel veri kayıtları var", L" · Some personal data entries are invalid") \
    X(runtimeAbsent, L" · Ruffle eksik: runtime\\ruffle.exe", L" · Ruffle missing: runtime\\ruffle.exe") \
    X(favoriteAdded, L"Favoriye eklendi.", L"Added to favorites.") \
    X(favoriteRemoved, L"Favoriden çıkarıldı.", L"Removed from favorites.") \
    X(favoriteUnsaved, L"Favori bu oturumda değişti. data\\favorites.ini yazılamadı.", L"Favorite changed for this session. Could not write data\\favorites.ini.") \
    X(dialogFailed, L"Giriş dosyası penceresi açılamadı.", L"Could not open the entry file dialog.") \
    X(entryChanged, L"SWF listesi değişti. Oyunu yeniden seçin.", L"The SWF list changed. Select the game again.") \
    X(entrySaved, L"Giriş dosyası kaydedildi.", L"Entry file saved.") \
    X(entrySession, L"Giriş dosyası bu oturum için seçildi.", L"Entry file selected for this session.") \
    X(entryUnsaved, L"Giriş dosyası oturum için seçildi. data\\library.ini yazılamadı.", L"Entry selected for this session. Could not write data\\library.ini.") \
    X(alreadyPlaying, L"Yeni oyun açmak için Ruffle penceresini kapatın.", L"Close the Ruffle window to start a new game.") \
    X(playerStarted, L"Ruffle başlatıldı · {}", L"Ruffle started · {}") \
    X(playerStarting, L"Ruffle oyunu açıyor · {}", L"Ruffle is opening the game · {}") \
    X(playerNoWindow, L"Ruffle henüz pencere açmadı. Başlatmayı iptal edebilirsiniz.", L"Ruffle has yet to open a window. You can cancel startup.") \
    X(playerCancelPrompt, L"Oyun penceresi henüz açılmadı. Başlatma iptal edilsin mi?", L"The game window has yet to open. Cancel startup?") \
    X(playerCancelClosePrompt, L"Başlatma iptal edilsin ve FlashFasaFiso kapatılsın mı?", L"Cancel startup and close FlashFasaFiso?") \
    X(playerForcePrompt, L"Ruffle yanıt vermedi. Açılmamış oyun işlemi zorla sonlandırılsın mı?", L"Ruffle did not respond. Force the unopened game process to end?") \
    X(playerForceClosePrompt, L"Açılmamış Ruffle işlemi zorla sonlandırılsın ve FlashFasaFiso kapatılsın mı?", L"Force the unopened Ruffle process to end and close FlashFasaFiso?") \
    X(playerCancelPending, L"Ruffle'dan kapanması istendi.", L"Close requested from Ruffle.") \
    X(playerCancelFailed, L"Ruffle kapanmadı. Açılmamış işlemi sonlandırabilirsiniz.", L"Ruffle did not close. You can end the unopened process.") \
    X(playerForcePending, L"Ruffle işleminin sonlanması bekleniyor.", L"Waiting for the Ruffle process to end.") \
    X(playerCancelled, L"Ruffle başlatması iptal edildi · {}", L"Ruffle startup cancelled · {}") \
    X(playerClosed, L"Ruffle kapandı · {}", L"Ruffle closed · {}") \
    X(playerFailed, L"Ruffle hatayla kapandı ({}). Günlük: data\\cache\\ruffle\\log", L"Ruffle closed with an error ({}). Logs: data\\cache\\ruffle\\log") \
    X(folderUnavailable, L"Klasör açılamıyor. Sürücü erişimini kontrol edin.", L"Cannot open the folder. Check drive access.") \
    X(explorerFailed, L"Windows klasörü açamadı ({}).", L"Windows could not open the folder ({}).") \
    X(duplicate, L"FlashFasaFiso bu taşınabilir klasör için zaten açık.", L"FlashFasaFiso is already open for this portable folder.") \
    X(startFailed, L"FlashFasaFiso başlatılamadı.", L"FlashFasaFiso could not start.") \
    X(runtimeNotFound, L"Ruffle bulunamadı. ruffle.exe dosyasını runtime klasörüne koyun.", L"Ruffle was not found. Place ruffle.exe in the runtime folder.") \
    X(invalidSwf, L"Oyunun SWF yolu geçersiz.", L"The game's SWF path is invalid.") \
    X(invalidSwfHeader, L"SWF dosyasının başlığı geçersiz veya eksik.", L"The SWF file header is invalid or truncated.") \
    X(unreadableSwf, L"Oyun dosyası okunamıyor. Klasörü kontrol edip kütüphaneyi yenileyin.", L"Cannot read the game file. Check its folder and rescan.") \
    X(ruffleDirsFailed, L"Ruffle için taşınabilir klasörler açılamıyor. data erişimini kontrol edin.", L"Cannot open Ruffle's portable folders. Check access to data.") \
    X(ruffleConfigWrite, L"Ruffle ayarları yazılamadı. data dizini için yazma izni gerekli.", L"Could not write Ruffle settings. The data folder must be writable.") \
    X(ruffleConfigRead, L"Ruffle ayar dosyası okunamıyor veya bir bağlantı noktası içeriyor.", L"Ruffle's settings file is unreadable or uses a reparse point.") \
    X(commandTooLong, L"Ruffle komut satırı Windows yol sınırını aşıyor.", L"Ruffle's command line exceeds the Windows limit.") \
    X(createProcessFailed, L"Ruffle başlatılamadı (Windows hata kodu {}).", L"Could not start Ruffle (Windows error {}).")

enum class Text {
#define FFF_KEY(key, tr, en) key,
    FFF_TEXTS(FFF_KEY)
#undef FFF_KEY
};

constexpr std::wstring_view text(Language language, Text key) {
    switch (key) {
#define FFF_VALUE(key, turkish, english) case Text::key: return language == Language::tr && !std::wstring_view(turkish).empty() ? turkish : english;
        FFF_TEXTS(FFF_VALUE)
#undef FFF_VALUE
    }
    return {};
}
#undef FFF_TEXTS

inline std::wstring formatted(Language language, Text key, std::wstring_view value) {
    std::wstring result(text(language, key));
    if (const auto token = result.find(L"{}"); token != result.npos) result.replace(token, 2, value);
    return result;
}
}
