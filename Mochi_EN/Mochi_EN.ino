/*
 * MOCHI - Animasyonlu OLED yuzu
 * 
 * Ozellikler:
 *  - LittleFS'te .bin dosyalarindan animasyon
 *  - WiFi AP modu (arayuzden ayarlanabilir hizli dokunma sayisiyla acilir)
 *  - WiFi STA modu (ev WiFi'sina baglanma)
 *  - Web arayuzu: yuz galerisi, atamalar, ayarlar
 *  - Animasyon test butonlari, kategoriler, storage bar, touch kalibrasyonu
 *  - Opsiyonel MOC3 RLE+delta sikistirilmis .bin destegi (MOC2 uyumlu)
 *  - Dokunma davranislari: poke, petting
 *  - Checkbox ile secilebilir idle, tepki, opening event ve random event animasyonlari
 *  - Rastgele secimlerde son 4 animasyonu mumkun oldugunca tekrar etmeme
 *  - Arayuzden ayarlanabilir WiFi/pil dokunma kombinasyonlari; WiFi gesture ac/kapat toggle
 *  - Ayarlanabilir sure sonra WiFi otomatik kapanir
 *  - GPIO0 uzerinden pil/voltaj yuzdesi ve dusuk pil bildirimi
 *  - Arayuzden yuzdelik OLED parlaklik/kontrast ayari
 *  - Ayarlanabilir uyku modu: light/deep/hybrid; tepkisizlikte OLED/WiFi kapanir, dokunma disinda is yapmaz
 * 
 * Donanim:
 *  - ESP32-C3 SuperMini
 *  - 1.3" SH1106 SPI OLED (SCK=4, MOSI=6, RES=3, DC=5, CS=7)
 *  - TTP223 dokunma sensoru (OUT=GPIO1)
 *  - MMA8452Q ivmeolcer (SDA=GPIO20, SCL=GPIO21, INT1=GPIO2, INT2=GPIO10)
 *  - Normal kullanimda LiPo + TP4056 sarj karti
 *  - Pil olcumu: OUT+/BAT+ -> 330k -> GPIO0 -> 330k -> GND
 */

#include <SPI.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>
#include <esp_idf_version.h>
#include <driver/gpio.h>
// Web UI ayri header dosyasinda gzip-compressed PROGMEM olarak tutulur.
#include "Mochi_html.h"
// MMA8452Q ivmeolcer entegrasyonu
#include <Wire.h>
#include <SparkFun_MMA8452Q.h>  // SparkFun MMA8452Q by Jim Lindblom

// ============================================================
// PIN TANIMLARI
// ============================================================
#define PIN_SCK   4
#define PIN_MOSI  6
#define PIN_RES   3
#define PIN_DC    5
#define PIN_CS    7
#define PIN_TOUCH 1
#define PIN_BATTERY_ADC 0

// MMA8452Q pinleri
#define PIN_ACCEL_SDA  20
#define PIN_ACCEL_SCL  21
#define PIN_ACCEL_INT1  2   // Hard shake / free-fall; deep sleep wake kaynagi
#define PIN_ACCEL_INT2 10   // Soft shake, orientation, motion

// MMA8452Q register adresleri. Motion/transient/PL ayarlari Wire ile yapiliyor.
#define ACCEL_I2C_ADDR        0x1C
#define ACCEL_REG_INT_SOURCE  0x0C
#define ACCEL_REG_PL_STATUS   0x10
#define ACCEL_REG_PL_CFG      0x11
#define ACCEL_REG_PL_COUNT    0x12
#define ACCEL_REG_TRANS_CFG   0x1D
#define ACCEL_REG_TRANS_SRC   0x1E
#define ACCEL_REG_TRANS_THS   0x1F
#define ACCEL_REG_TRANS_COUNT 0x20
#define ACCEL_REG_CTRL1       0x2A
#define ACCEL_REG_CTRL3       0x2C
#define ACCEL_REG_CTRL4       0x2D
#define ACCEL_REG_CTRL5       0x2E

// ============================================================
// WIFI AYARLARI
// ============================================================
// Cihazin adi
const char* DEVICE_NAME = "R2D2";

// AP bilgileri
const char* AP_SSID = "R2D2";
const char* AP_PASSWORD = "example1";

// Varsayilan: 10 hizli dokunma (7 saniye icinde) ile WiFi acilir
const int DEFAULT_TAP_TRIGGER_COUNT = 10;  // WiFi acma/kapama tap sayisi
const int DEFAULT_WIFI_TAP_WINDOW_SEC = 7;
const int DEFAULT_BATTERY_TAP_COUNT = 5;
const int DEFAULT_BATTERY_TAP_WINDOW_SEC = 5;
const int MIN_TAP_TRIGGER_COUNT = 2;
const int MAX_TAP_TRIGGER_COUNT = 25;
const int MIN_TAP_WINDOW_SEC = 1;
const int MAX_TAP_WINDOW_SEC = 30;
const int MAX_TAP_HISTORY = 30;
const unsigned long BATTERY_GESTURE_SETTLE_MS = 750UL;  // Pil/WiFi gesture ayrimi icin bekleme

// ============================================================
// DAVRANIS PARAMETRELERI
// ============================================================
const int DEFAULT_LONG_PRESS_MS = 200;
const unsigned long PETTING_AFTER_DURATION_MS = 2000;
const int DEFAULT_DEEP_PETTING_MS = 3000;  // bu kadar tutarsa "cok sevilme" yuzu
const int DEFAULT_TOUCH_DEBOUNCE_MS = 35;

// .5: UI/config clamp limitleri tek merkezde. loadConfig + handleApiConfigImport
// + handleApiConfig (UI POST) eskiden bu literal'lari her yerde tekrar ediyordu.
const int LONG_PRESS_MIN_MS = 80;
const int LONG_PRESS_MAX_MS = 1500;
const int DEEP_PETTING_MIN_MS = 500;
const int DEEP_PETTING_MAX_MS = 10000;
const int TOUCH_DEBOUNCE_MIN_MS = 0;
const int TOUCH_DEBOUNCE_MAX_MS = 300;
const int IDLE_LOOPS_MIN = 1;
const int IDLE_LOOPS_MAX = 5;
const int IDLE_SECONDS_MIN = 0;
const int IDLE_SECONDS_MAX = 60;
// Idle ara animasyon listesi max kapasitesi (idleAnims[20].name dizisiyle uyumlu).
const int MAX_IDLE_LIST = 20;
// Tepki/event listeleri kapasitesi (pokeReactions[50] vs ile uyumlu).
const int MAX_REACTION_LIST = 50;

// ============================================================
// PIL OLCUMU
// ============================================================
// Normal pil baglantisi: LiPo/TP4056 OUT+ -> 330k -> GPIO0 -> 330k -> GND.
// USB/direct ESP besleme testlerinde okunan voltaj LiPo durumu olarak yorumlanmaz.
const float BATTERY_DIVIDER_RATIO = 2.372f;
const float BATTERY_CALIBRATION = 1.000f;
const float BATTERY_MIN_VOLTAGE = 3.50f;
const float BATTERY_MAX_VOLTAGE = 4.08f;
const unsigned long BATTERY_READ_INTERVAL_MS = 2000UL;
const unsigned long BATTERY_NOTIFICATION_DURATION_MS = 2500UL;  // Pil overlay suresi
// CHRG/STDBY pini bagli olmadigi icin sarj durumu voltaj trendinden tahmin edilir.
const unsigned long BATTERY_CHARGE_DETECT_INTERVAL_MS = 10000UL;
const unsigned long BATTERY_CHARGING_HOLD_MS = 600000UL;  // son yukselmeden sonra 10 dk sarj kabul et
const float BATTERY_CHARGE_RISE_THRESHOLD_V = 0.004f;     // 10 sn icinde 4mV yukselme = sarj sinyali
const float BATTERY_CHARGE_DROP_OFF_V = 0.035f;           // tepe voltajdan 35mV dusme = muhtemelen sarjdan cikti
const int BATTERY_WARN_COUNT = 5;
const int BATTERY_WARN_THRESHOLDS[BATTERY_WARN_COUNT] = {50, 30, 20, 10, 5};

// ============================================================
// EKRAN
// ============================================================
// SH1106/U8g2 kontrasti arayuzde yuzde olarak ayarlanir.
const int DEFAULT_DISPLAY_BRIGHTNESS_PERCENT = 50;
const int MIN_DISPLAY_BRIGHTNESS_PERCENT = 5;
const int MAX_DISPLAY_BRIGHTNESS_PERCENT = 100;

// ============================================================
// UYKU MODU
// ============================================================
// Dokunma yoksa ekran kapanir, idle/random/event animasyonlari durur.
// Uyku sadece dokunma ile bozulur; web ping/random event uykuyu bolmez.
const bool DEFAULT_SLEEP_ENABLED = true;
const int DEFAULT_SLEEP_TIMEOUT_MINUTES = 60;
const int MIN_SLEEP_TIMEOUT_MINUTES = 1;
const int MAX_SLEEP_TIMEOUT_MINUTES = 720;
// Blocking uyku giris/cikis animasyonlarinda asiri uzun sureleri sinirlar.
const int MAX_SLEEP_ANIM_MS = 15000;
// Uyku tipi:
// 0 = Light sleep: RAM/state korunur, dokununca hizli devam eder
// 1 = Deep sleep: en az enerji, dokununca cihaz yeniden baslar ama opening/hello atlanir
// 2 = Hybrid: once light sleep, belirlenen sure dokunma gelmezse deep sleep
const int SLEEP_MODE_LIGHT = 0;
const int SLEEP_MODE_DEEP = 1;
const int SLEEP_MODE_HYBRID = 2;
const int DEFAULT_SLEEP_MODE = SLEEP_MODE_LIGHT;
const int DEFAULT_SLEEP_DEEP_AFTER_MINUTES = 15;
const int MIN_SLEEP_DEEP_AFTER_MINUTES = 1;
const int MAX_SLEEP_DEEP_AFTER_MINUTES = 720;
const uint32_t MOCHI_DEEP_SLEEP_MAGIC = 0xC0FFEE42;
RTC_DATA_ATTR uint32_t mochiDeepSleepMagic = 0;

// Arduino/C++ derleyicisi global degiskeni kullanmadan once en azindan bildirim ister.
// Asil tanim WiFi bildirim bolumunde duruyor.
extern bool showingWifiNotification;

U8G2_SH1106_128X64_VCOMH0_F_4W_HW_SPI u8g2(
  U8G2_R0, PIN_CS, PIN_DC, PIN_RES
);

// ============================================================
// BIN FORMAT
// ============================================================
#define BIN_MAGIC "MOC2"
#define BIN_MAGIC_V3 "MOC3"
#define BIN_HEADER_SIZE 8
// .bin dosya formati versiyonlari ve format byte tanimlari.
// Header layout (BIN_HEADER_SIZE = 8 byte):
//   [0..3] magic ('MOC2' veya 'MOC3')
//   [4]    frame_count
//   [5]    bytes_per_row (Mochi'de daima 16 = 128/8)
//   [6]    height (daima 64)
//   [7]    format byte:
//             - MOC2: anlamsiz (genelde 0); ham frame data izler
//             - MOC3: 3 = RLE+delta sıkıştırma (BIN_FORMAT_RLE_DELTA)
// Eskiden MOC2'de de header[7] okunup format kontrolune sokuluyordu;
// artik isMoc3 dali altinda kontrol ediliyor, MOC2 yolunda format okunmuyor.
#define BIN_FORMAT_RAW 0           // MOC2 placeholder (kullanim disi, doc icin)
#define BIN_FORMAT_RLE_DELTA 3     // MOC3 sıkıştırma

struct BinHeader {
  char magic[4];          // "MOC2"
  uint8_t frame_count;
  uint8_t bytes_per_row;  // genelde 16 (128/8)
  uint8_t height;         // genelde 64
  uint8_t reserved;
};

// ============================================================
// ANIMASYON STATE
// ============================================================
struct Animation {
  String name;            // dosya adi (uzantisiz): "happy"
  uint8_t frame_count;
  uint8_t bytes_per_row;
  uint8_t height;
  uint8_t* frames_data;   // tum frame'ler dahil delay'leri
  bool loaded;
};

Animation currentAnim;
// Eskiden 7 ayri Animation slot'u vardi (poke/petting/deep/opening/random/
// test/sleepTransition). Hicbiri ayni anda kullanilmiyordu (transient: yukle,
// oynat, free), ama her biri Animation struct'i frames_data pointer'iyla heap
// fragmentation'a katki sagliyordu. Hepsini tek 'eventAnim' slot'una indirdik.
// loadAnimation() icindeki freeAnimation cagrisı zaten onceki sahibin frames_data'sini
// otomatik free ediyor, yani gecisler atomik.
Animation eventAnim;
Animation* activeAnim = nullptr;

// Display dirty-skip state. drawCurrentFrame her loop tick'inde sendBuffer
// odemesin diye son cizilen state'i takip ediyoruz. Frame/mood/icon degismedikce
// drawing skip ediliyor.
bool displayDirty = true;
int lastDrawnFrame = -1;
Animation* lastDrawnAnim = nullptr;
bool lastDrawnCharging = false;
bool lastDrawnWifi = false;
inline void markDisplayDirty() { displayDirty = true; }
// Event/tepki animasyonu bittikten sonra SH1106/OLED controller bazen eski
// frame kolonlarini tutabiliyor. Bir sonraki default yuz yuklemesinde tam
// temiz ekran + default frame zorlamak icin tek seferlik bayrak.
bool forceHardIdleDisplayRecovery = false;

// En son animasyon yukleme/parse hatasini tutar; UI debug icin /api/sys'e
// dahil edilir. Bos string = son acilistan beri hata yok.
String lastAnimError = "";
unsigned long lastAnimErrorAt = 0;

// Config yukleme durumu UI'da gosterilebilsin. Olasi degerler:
//   "ok"            - /config.json bulundu, parse basarili
//   "fresh"         - /config.json yoktu, default'larla baslandı, ilk save yapildi
//   "restored-bak"  - /config.json bozuktu, /config.bak'tan geri yuklendi (M-17)
//   "fallback-defaults" - /config.json bozuk + .bak da yok, default'larla calisiyor
//                         (kullanici save yapmadan elindeki ayarlari kaybetmemek icin
//                          bilinmeli)
String configStatus = "ok";

// Yuz dosyasi varligi cache'i. faceFileExists() her cagrida LittleFS.exists()
// syscall'i yapiyordu; getRandomReaction* her tetiklemede 5-10 yuz icin top-up
// 5-10ms blocking olabiliyor. Bu cache /faces/ dizinindeki .bin'leri tutuyor;
// dirty flag upload/delete'lerde set ediliyor.
const int MAX_FACE_CACHE = 64;
String faceCache[MAX_FACE_CACHE];
int faceCacheCount = 0;
bool faceCacheDirty = true;
int activeFrame = 0;
unsigned long lastFrameTime = 0;
float speedMultiplier = 1.0;  // global hiz carpani

// ============================================================
// MOOD STATE
// ============================================================
enum MoodState {
  MOOD_IDLE,
  MOOD_POKED,
  MOOD_PETTING,
  MOOD_DEEP_PETTING,
  MOOD_PETTING_AFTER,
  MOOD_RANDOM_EVENT,
  MOOD_TEST_EVENT,
  MOOD_ACCEL_EVENT  // ivmeolcer tetikleyici animasyonu
};

// Gecici/event animasyonlarini tek merkezden guvenli oynatmak icin kaynak etiketi.
// Bu turda touch, petting/deep petting ve accel event yuklemeleri bu yoldan geciyor.
enum EventSource {
  EVENT_TOUCH,
  EVENT_ACCEL,
  EVENT_PETTING,
  EVENT_DEEP_PETTING,
  EVENT_IDLE,
  EVENT_RANDOM,
  EVENT_TEST,
  EVENT_OPENING,
  EVENT_WEB,
  EVENT_UNKNOWN
};

struct EventPlayOptions {
  bool allowDuringWifi;
  uint16_t maxFramesDuringWifi;
  bool resetFrame;
  bool logResult;
};

const EventPlayOptions TOUCH_EVENT_OPTIONS = {true, 30, true, true};
const EventPlayOptions ACCEL_EVENT_OPTIONS = {true, 30, true, true};
const EventPlayOptions PETTING_EVENT_OPTIONS = {true, 45, true, true};
const EventPlayOptions DEEP_PETTING_EVENT_OPTIONS = {true, 45, true, true};
const EventPlayOptions TEST_EVENT_OPTIONS = {true, 45, true, true};
const EventPlayOptions IDLE_EVENT_OPTIONS = {false, 0, true, true};
const EventPlayOptions RANDOM_EVENT_OPTIONS = {false, 0, true, true};

MoodState mood = MOOD_IDLE;
unsigned long moodStartTime = 0;

// Idle animasyon yonetimi
// Mantik: Yuzler sekmesindeki aktif yuz ana/default yuzdur ve normalde surekli o oynar.
// Idle listesi ise ana yuzun arasina kisa, rastgele "bakis/tepki" animasyonlari sokar.
// 3 paralel array (idleAnimList/Loops/Seconds) tek struct array'a indirildi.
// Avantajlar:
//  - Kavramsal birim tek yerde: ara idle bir liste elemani = isim + loops + seconds.
//  - removeName / prune fonksiyonlari paralel array'lere ozel branch tutmuyor;
//    struct'tan eleman silinince loops/seconds otomatik geliyor/gidiyor.
//  - Cache locality biraz daha iyi (kucuk fark, ama bedavadan).
struct IdleAnim {
  String name;
  int loops;     // kac tur oynasin (1..5)
  int seconds;   // 0=otomatik (animasyon * loops), >0=sabit saniye
};
IdleAnim idleAnims[MAX_IDLE_LIST];
int idleAnimCount = 0;
int lastIdleEventIndex = -1;
bool idleEventPlaying = false;
unsigned long nextIdleEventAt = 0;
unsigned long currentIdleStartTime = 0;
unsigned long currentIdleDurationMs = 0;
const int DEFAULT_IDLE_EVENT_MIN_SEC = 25;  // default yuz ana durumdur; idle sadece arada mikro hareket verir
const int DEFAULT_IDLE_EVENT_MAX_SEC = 45;
unsigned long idleEventMinMs = (unsigned long)DEFAULT_IDLE_EVENT_MIN_SEC * 1000UL;
unsigned long idleEventMaxMs = (unsigned long)DEFAULT_IDLE_EVENT_MAX_SEC * 1000UL;

// Tepki listeleri
String pokeReactions[50];
int pokeReactionCount = 0;
String pettingReactions[50];
int pettingReactionCount = 0;
String deepPettingReactions[50];
int deepPettingReactionCount = 0;

// Opening event listesi: cihaz acilista secili animasyonlardan birini oynatir
String openingEvents[50];
int openingEventCount = 0;

// Random event listesi: idle'dan daha seyrek ve daha belirgin otomatik davranislar icindir.
String randomEvents[50];
int randomEventCount = 0;

// Uyku animasyon listeleri: uykuya giris ve uyanista secilenlerden rastgele oynar.
String sleepEnterEvents[50];
int sleepEnterEventCount = 0;
String sleepWakeEvents[50];
int sleepWakeEventCount = 0;
const int DEFAULT_RANDOM_EVENT_MIN_SEC = 60;
const int DEFAULT_RANDOM_EVENT_MAX_SEC = 120;
unsigned long randomEventMinMs = (unsigned long)DEFAULT_RANDOM_EVENT_MIN_SEC * 1000UL;
unsigned long randomEventMaxMs = (unsigned long)DEFAULT_RANDOM_EVENT_MAX_SEC * 1000UL;
unsigned long nextRandomEventAt = 0;
unsigned long randomEventStartTime = 0;
unsigned long randomEventDurationMs = 1200;
unsigned long testEventStartTime = 0;
unsigned long testEventDurationMs = 1200;
unsigned long pokeReactionDurationMs = 800;
unsigned long pettingAfterDurationMs = PETTING_AFTER_DURATION_MS;

// Otomatik davranislar kuyruk gibi akmasin: default yuz once belirli sure sakin kalsin,
// bir otomatik davranistan sonra da cooldown olsun. Dokunma tepkileri bu kurali bozabilir.
const unsigned long AUTO_DEFAULT_DWELL_MS = 8000UL;
const unsigned long AUTO_EVENT_COOLDOWN_MS = 15000UL;
const unsigned long AUTO_RANDOM_PRIORITY_WINDOW_MS = 10000UL;
unsigned long lastAutoEventEndedAt = 0;

// Web UI gibi uzun isler sirasinda OLED frame ilerlemesini beslemek icin kullanilir.
unsigned long lastBlockingDisplayServiceAt = 0;

// Rastgele secim gecmisi: ayni kategori icinde son 4 secimi mumkun oldugunca tekrar etme.
// Not: Listede 4 veya daha az gecerli animasyon varsa matematiksel olarak son 4'u tamamen dislamak
// mumkun degil; bu durumda en azindan bir onceki secimi tekrar etmemeye calisir.
const int RANDOM_HISTORY_SIZE = 4;
String recentIdleHistory[RANDOM_HISTORY_SIZE];
int recentIdleHistoryCount = 0;
String recentPokeHistory[RANDOM_HISTORY_SIZE];
int recentPokeHistoryCount = 0;
String recentPettingHistory[RANDOM_HISTORY_SIZE];
int recentPettingHistoryCount = 0;
String recentDeepHistory[RANDOM_HISTORY_SIZE];
int recentDeepHistoryCount = 0;
String recentRandomEventHistory[RANDOM_HISTORY_SIZE];
int recentRandomEventHistoryCount = 0;
String recentSleepEnterHistory[RANDOM_HISTORY_SIZE];
int recentSleepEnterHistoryCount = 0;
String recentSleepWakeHistory[RANDOM_HISTORY_SIZE];
int recentSleepWakeHistoryCount = 0;

// ============================================================
// IVMEOLCER (MMA8452Q) STATE
// ============================================================
MMA8452Q accel;
bool accelReady = false;           // baslatma basarili mi

// Tetikleyici reaction listeleri (config icinde saklanir)
String accelSoftShakeReactions[MAX_REACTION_LIST];
int accelSoftShakeCount = 0;
String accelHardShakeReactions[MAX_REACTION_LIST];
int accelHardShakeCount = 0;
String accelAccelReactions[MAX_REACTION_LIST];
int accelAccelCount = 0;
String accelFreefallReactions[MAX_REACTION_LIST];
int accelFreefallCount = 0;
String accelOrientationReactions[MAX_REACTION_LIST];
int accelOrientationCount = 0;
String accelPickupReactions[MAX_REACTION_LIST];
int accelPickupCount = 0;

// Rastgele secim gecmisi — her kategori icin ayri history
String recentAccelSoftHistory[RANDOM_HISTORY_SIZE];
int recentAccelSoftHistoryCount = 0;
String recentAccelHardHistory[RANDOM_HISTORY_SIZE];
int recentAccelHardHistoryCount = 0;
String recentAccelAccelHistory[RANDOM_HISTORY_SIZE];
int recentAccelAccelHistoryCount = 0;
String recentAccelFreefallHistory[RANDOM_HISTORY_SIZE];
int recentAccelFreefallHistoryCount = 0;
String recentAccelOrientHistory[RANDOM_HISTORY_SIZE];
int recentAccelOrientHistoryCount = 0;
String recentAccelPickupHistory[RANDOM_HISTORY_SIZE];
int recentAccelPickupHistoryCount = 0;

// Kullanici ayarlanabilir hassasiyet parametreleri (config icinde saklanir)
float accelSoftThreshold   = 0.5f;   // g; yumusak sallanma esigi
float accelHardThreshold   = 1.5f;   // g; sert sallanma esigi
float accelAccelThreshold  = 0.8f;   // g; arac/asansor hizlanma esigi
unsigned long accelFreefallDurMs = 200UL; // ms; serbest dusme min suresi
unsigned long accelPickupInactivityMs = 30000UL; // ms; pickup oncesi hareketsizlik
bool accelEnabled = true;             // tum ivmeolcer mantigi acik/kapali

// Cooldown sureleri (ms) — her kategori icin
const unsigned long ACCEL_COOLDOWN_SOFT_MS       =  3000UL;
const unsigned long ACCEL_COOLDOWN_HARD_MS       =  5000UL;
const unsigned long ACCEL_COOLDOWN_ACCEL_MS      =  8000UL;
const unsigned long ACCEL_COOLDOWN_FREEFALL_MS   =  8000UL;
const unsigned long ACCEL_COOLDOWN_ORIENT_MS     =  4000UL;
const unsigned long ACCEL_COOLDOWN_PICKUP_MS     = 10000UL;

unsigned long accelCooldownSoftEnd       = 0;
unsigned long accelCooldownHardEnd       = 0;
unsigned long accelCooldownAccelEnd      = 0;
unsigned long accelCooldownFreefallEnd   = 0;
unsigned long accelCooldownOrientEnd     = 0;
unsigned long accelCooldownPickupEnd     = 0;

// Mevcut MOOD_ACCEL_EVENT suresi ve bitis zamani
unsigned long accelEventDurationMs = 0;
unsigned long accelEventStartTime  = 0;

// Interrupt bayraklari — ISR'dan main loop'a gecis icin volatile
volatile bool accelInt1Fired = false;  // INT1: hard shake / freefall
volatile bool accelInt2Fired = false;  // INT2: soft shake, orientation, motion
bool accelHardWakePending = false;     // light sleep INT1 wake -> hard reaction oynat
uint32_t touchRawTransitions = 0;      // TESHIS: ham dokunma pini kac kez degisti (RF gurultu tespiti)
int touchAdcLast = 0;                  // readTouchActive'in son ADC okumasi (DIAG icin)

// INT pinleri firtina yaptiginda ivmeolceri tamamen kapatmak yerine
// I2C polling fallback'e geceriz. Boylece aktif modda sallama yuzleri
// yazilimsal olarak calismaya devam eder; sadece sleep wake INT'siz kalir.
bool accelInterruptsSuspended = false;
const unsigned long ACCEL_POLL_FALLBACK_INTERVAL_MS = 50UL;

// DIAG BUILD: Config'te ivmeolcer kapali kaldiysa (DIAG accel=0) sallama
// tepkileri hic tetiklenmez. Donanim/yazilim testi icin boot'ta runtime olarak
// zorla aciyoruz; config dosyasina kaydetmiyoruz.
const bool ACCEL_FORCE_ENABLE_FOR_DIAG = true;

uint8_t accelLastPlStatus = 0;         // handleAccelEvents'in son okudugu PL_STATUS (live UI icin)

// Pickup pattern detection state
unsigned long accelLastMotionTime = 0;       // son motion interrupt zamani
bool accelPickupMotionSeen       = false;    // inactivity sonrasi motion geldi mi
bool accelPickupOrientSeen       = false;    // ardından orientation degisi mi

// Serbest dusme tespiti: kac milisaniye alti-esik kaldı
unsigned long accelFreefallStartMs = 0;
bool accelInFreefall = false;

// Yuz kategorileri (config icinde saklanir, sadece arayuz filtreleme icin)
String faceCategoryNames[80];
String faceCategoryValues[80];
int faceCategoryCount = 0;

// ============================================================
// DOKUNMA STATE
// ============================================================
bool prevTouch = false;
unsigned long touchStartTime = 0;
unsigned long lastInteractionTime = 0;
// Deep-petting gecisini mevcut sevme animasyonunun ortasinda kesmek yerine
// bir sonraki dongu sinirinda yapmak icin bekleyen gecis bayragi.
bool deepPettingPending = false;

// hizli tap detector
unsigned long tapTimes[MAX_TAP_HISTORY];
int tapCount = 0;

// ============================================================
// WIFI / WEB SERVER
// ============================================================
WebServer server(80);
bool wifiActive = false;
unsigned long wifiActivatedTime = 0;
unsigned long lastWebActivityTime = 0;
unsigned long lastAPClientSeenTime = 0;

// WiFi acikken default animasyon RAM'de kalir; eventler merkezi loader ile
// sadece guvenli/kucuk secimlerden denenir. Eski tek-kare snapshot modu kaldirildi.
uint32_t heapBeforeWiFiStart = 0;
uint32_t heapAfterWiFiStart = 0;

// STA bagli olmayinca radyo acik kalip pil yiyor. Auto-connect ile baslayan
// STA icin bir baslangic zamani takibi tutuyoruz. 90 saniyede WL_CONNECTED olmazsa
// STA'i kapatiyoruz; AP acilinca zaten yeniden moda donulur.
unsigned long staConnectAttemptStart = 0;
bool staAutoConnectInProgress = false;
const unsigned long STA_AUTO_CONNECT_TIMEOUT_MS = 90000UL;

// Yuz dosyasi yoksa ayni dosyayi dongude tekrar tekrar denemeyelim.
bool waitingForFaceSelection = false;
String waitingMissingFaceName = "";
bool suppressAnimationLoadLog = false;
String lastUploadFaceName = "";
// Eskiden tek 'uploadHadValidBin' flag'i vardi ama sadece dosya
// uzantisinin .bin olup olmadigini kontrol ediyordu. Artik ikisi ayri:
//   uploadHadBinExtension - upload baslangicinda .bin uzantisi varsa true
//   uploadHadValidContent - upload bittikten sonra validateFaceFile() gecerse true
bool uploadHadBinExtension = false;
bool uploadHadValidContent = false;

// LittleFS durum takibi
bool littlefsReady = false;
bool littlefsFormattedThisBoot = false;
String littlefsStatus = "not started";

// Pil durumu
float batteryVoltage = 0.0f;
int batteryPercent = -1;
int previousBatteryPercent = -1;
unsigned long lastBatteryReadTime = 0;
bool batteryWarnShown[BATTERY_WARN_COUNT] = {false, false, false, false, false};
bool showingBatteryNotification = false;
unsigned long batteryNotificationStartTime = 0;
int batteryNotificationPercent = -1;
float batteryNotificationVoltage = 0.0f;
bool batteryCharging = false;
float batteryChargeTrendVoltage = 0.0f;
unsigned long lastBatteryChargeTrendTime = 0;
unsigned long lastBatteryChargingSeenTime = 0;
float batteryChargePeakVoltage = 0.0f;

// Uyku modu durumu
bool systemSleeping = false;
unsigned long sleepEnteredAt = 0;
bool sleepTransitionRunning = false;
bool wokeFromMochiDeepSleep = false;
bool ignoreTouchUntilRelease = false;

// ============================================================
// FALLBACK BLINK - LittleFS'te yuz yoksa procedural cizim yapilir
// (drawFallbackFace fonksiyonu asagida)
// ============================================================

// ============================================================
// CONFIG
// ============================================================
struct MochiConfig {
  String defaultFace;
  String currentFace;
  String wifiSSID;        // ev WiFi SSID
  String wifiPassword;    // ev WiFi parola
  bool wifiAutoConnect;
  int wifiTimeoutMinutes; // AP acikken web etkinligi yoksa kac dakika sonra kapansin
  int wifiTapCount;       // WiFi AP acmak icin gereken hizli dokunma sayisi
  int wifiTapWindowSec;   // WiFi icin kac saniyelik hizli dokunma penceresi
  int batteryTapCount;    // Pil yuzdesini ekranda gostermek icin gereken dokunma sayisi
  int batteryTapWindowSec;// Pil yuzdesi icin kac saniyelik hizli dokunma penceresi
  int longPressMs;        // sevilme esigi
  int deepPettingMs;      // cok sevilme esigi
  int touchDebounceMs;    // TTP223 debounce
  int displayBrightnessPercent; // OLED parlakligi/kontrasti: 5-100%, varsayilan 50% (~128)
  bool sleepEnabled;      // tepkisizlikte uykuya gecsin mi
  int sleepTimeoutMinutes;// kac dakika dokunma olmazsa uyusun
  String sleepEnterFace;  // eski config uyumlulugu: ilk uykuya giris animasyonu
  String sleepWakeFace;   // eski config uyumlulugu: ilk uyanma animasyonu
  // sleepEnterMs / sleepWakeMs alanlari kaldirildi.
  // playNamedAnimationBlocking her cagriya durationMs=0 ile geliyordu
  // (animasyon kendi total suresi kadar oynar), bu config degerleri
  // hicbir zaman okunmadi ve UI'da gosterilmiyor. Eski config.json'larda
  // bu alanlar varsa loadConfig onlari ihmal eder, ilk saveConfig'te dusurulur.
  int sleepMode;          // 0 light, 1 deep, 2 hybrid
  int sleepDeepAfterMinutes; // hybrid modda kac dk light sleep sonra deep sleep
};

MochiConfig config;

// ============================================================
// FORWARD DECLARATIONS
// ============================================================
void loadConfig();
void saveConfig();
bool loadAnimation(const String& name, Animation& anim);
bool faceFileExists(const String& name);
String findFirstUsableFaceName();
// Face cache helpers. faceFileExists() bunlari transparent kullanir.
void rebuildFaceCache();
inline void invalidateFaceCache();
// validateFaceFile: dosyayi acip header'i parse ederek 128x64 MOC2/MOC3 olup
// olmadigini dogrular. Yalniz uzantiya degil icerige bakar (C-4).
bool validateFaceFile(const String& name);
// Web'den gelen yuz adlarini path traversal ve garip karakterlerden
// sanitize eder. Sadece [A-Za-z0-9_-] kabul eder, max 32 char. Bos string
// dondurmesi = isim gecersiz, cagiran tarafta reddedilmeli.
String sanitizeFaceName(const String& raw);
int countFaceFiles();
void enterFallbackWaitMode(const String& missingName, const char* reason);
void freeAnimation(Animation& anim);
void freeTransientAnimations();
// NOT: Asagidaki File& alan fonksiyonlar Arduino'nun otomatik prototip
// uretiminde sorun cikariyor (LittleFS.h'deki File tipi auto-prototype'tan
// once tanimlanmamis sayilabiliyor). Bu yuzden manuel forward decl SARTTIR;
// bu blogu silme/tasima.
bool decodeRLEIntoFrame(File& f, uint16_t compLen, uint8_t* out, const uint8_t* prev, size_t frameDataSize, bool delta);
bool loadRawMoc2(File& f, Animation& anim, size_t fileSize, uint8_t* header);
bool loadCompressedMoc3(File& f, Animation& anim, size_t fileSize, uint8_t* header);
void drawWifiStatusIcon();
bool startTestAnimation(const String& kind);
String getFaceCategory(const String& name);
void setFaceCategory(const String& name, const String& category);
void removeFaceCategory(const String& name);
bool removeNameFromArray(String* list, int& count, const String& name);
bool pruneMissingFacesFromArray(String* list, int& count);
// Idle struct array icin dedicated yardimcilar (paralel array yerine).
bool removeIdleAnimByName(const String& name);
bool pruneMissingIdleAnims();
bool listAlreadyHasName(String* list, int count, const String& name);
void resetRandomHistories();
bool sanitizeConfigAgainstFiles(bool saveAfter);

// GPIO1 dijital giris kati hasarli — dokunma ADC ile okunur (tanim handleTouch ustunde)
bool readTouchActive();

// ivmeolcer forward declarations
uint8_t accelReadReg(uint8_t reg);
void accelWriteReg(uint8_t reg, uint8_t val);
void applyAccelHardwareConfig(float transientThresholdG);
void setupAccelerometer();
void IRAM_ATTR accelISR_INT1();
void IRAM_ATTR accelISR_INT2();
void handleAccelEvents();
void handleAccelPollingFallback();
bool triggerAccelReaction(String* list, int count, String* history, int& histCnt, unsigned long* cooldownEnd, unsigned long cooldownMs);
bool accelEventAllowed();

void updateAnimation();
void serviceDisplayDuringBlockingWork();
bool defaultFaceIsStable(unsigned long now);
bool autoCooldownReady(unsigned long now);
bool randomEventDue(unsigned long now);
bool randomEventSoon(unsigned long now);
void handleTouch();
int countRecentTaps(unsigned long now, unsigned long windowMs);
void enterMood(MoodState newMood);
const char* eventSourceName(int source);
EventSource eventSourceFromMood(MoodState state);
bool playEventFaceSafe(const String& name, MoodState targetMood, EventSource source, const EventPlayOptions& options);
bool returnToDefaultFaceSafe(int source, bool scheduleIdleEventNow = true, bool scheduleRandomEventNow = true);
void selectIdleNext();
String getRandomReaction(String* list, int count);
unsigned long getAnimationTotalDurationMs(Animation& anim);
String getRandomReactionWithHistory(String* list, int count, String* history, int& historyCount);
bool isNameInHistory(const String& name, String* history, int historyCount);
void pushRandomHistory(String* history, int& historyCount, const String& name);
unsigned long getAdjustedAnimationDurationMs(Animation& anim, unsigned long minMs, unsigned long maxMs);
unsigned long getRemainingAnimationDurationMs(Animation& anim, int frameIdx, unsigned long now);
void scheduleNextRandomEvent();
void scheduleNextIdleEvent();                      // 1059'da loadDefaultIdleFace cagriliyor, ondan once decl gerekli
bool loadDefaultIdleFace(bool scheduleIdleEventNow); // 1059 ve sonrasinda 1590'dan once cagriliyor
void startWiFiAP();
void stopWiFiAP();
void restoreDefaultFaceAfterWiFi();
void wakeFromSleepMode();
void enterMinimalLightSleepUntilTouch();
void enterDeepSleepUntilTouch();
bool enableDeepSleepWakeOnTouch();
void stopAllWirelessForSleep();

void setupWebServer();
void drawFallbackFace();
void playHelloThereAnimation();
bool playStartupOpeningAnimation();
bool initLittleFS();
void setupBatteryMonitor();
void updateBatteryMonitor(bool force, bool resetTrend = false);
float readBatteryVoltage();
int batteryVoltageToPercent(float voltage);
void checkBatteryWarnings();
void showBatteryNotification(int percent, float voltage);
void drawBatteryNotification();
void recoverDisplayAfterNotification(const char* source);
void drawBatterySmallIcon(int x, int y, int percent);
void updateBatteryChargingEstimate(bool resetTrend);
void drawChargingBatteryOverlay();
int displayBrightnessPercentToContrast(int percent);
void applyDisplayBrightness();

// ============================================================
// OLED PARLAKLIK / KONTRAST
// ============================================================
int displayBrightnessPercentToContrast(int percent) {
  if(percent < MIN_DISPLAY_BRIGHTNESS_PERCENT) percent = MIN_DISPLAY_BRIGHTNESS_PERCENT;
  if(percent > MAX_DISPLAY_BRIGHTNESS_PERCENT) percent = MAX_DISPLAY_BRIGHTNESS_PERCENT;
  // 50% -> 128, 100% -> 255
  return (percent * 255 + 50) / 100;
}

void applyDisplayBrightness() {
  int pct = config.displayBrightnessPercent;
  if(pct < MIN_DISPLAY_BRIGHTNESS_PERCENT) pct = DEFAULT_DISPLAY_BRIGHTNESS_PERCENT;
  if(pct > MAX_DISPLAY_BRIGHTNESS_PERCENT) pct = MAX_DISPLAY_BRIGHTNESS_PERCENT;
  config.displayBrightnessPercent = pct;
  u8g2.setContrast(displayBrightnessPercentToContrast(pct));
}

// ============================================================
// FALLBACK YUZ CIZIMI (LittleFS'te dosya yoksa)
// ============================================================
void drawFallbackFace() {
  // Fallback sadece dosya yokken/aktif yüz bozukken görünür.
  // Kapalı/kısık göz animasyonu burada bilinçli olarak kullanılmıyor; aksi halde
  // fallback tek frame'de kalırsa kullanıcı bunu "kısık gözde donma" sanıyor.
  u8g2.clearBuffer();

  u8g2.drawRBox(29, 14, 18, 32, 7);
  u8g2.drawRBox(81, 14, 18, 32, 7);

  for(int i = -10; i <= 10; i++) {
    int yo = (i * i) / 45;
    u8g2.drawPixel(64 + i, 50 + yo);
    u8g2.drawPixel(64 + i, 50 + yo + 1);
  }

  drawChargingBatteryOverlay();
  if(showingBatteryNotification) drawBatteryNotification();
  drawWifiStatusIcon();
  u8g2.sendBuffer();
}

// ============================================================
// LITTLEFS / DOSYA YONETIMI
// ============================================================
// ============================================================
// YUZ DOSYASI VALIDASYON SEVIYELERI
// ============================================================
// Uc seviye var, hizdan dogruluga dogru:
//
//   1. faceFileExists(name)
//      - O(1), sadece LittleFS.exists() cagiriyor.
//      - Kullanim: hizli liste/UI kontrolu, "var mi acaba" sorusu.
//      - DIKKAT: bos/bozuk dosya icin de true dondurebilir.
//
//   2. validateFaceFile(name) / validateFaceFileAtPath(path)
//      - Dosyayi acip header'i okur, magic + 128x64 + minimum boyut kontrolu.
//      - Tum frame data'yi okumaz, yine hizli (~tek seek + 8 byte read).
//      - Kullanim: upload sonrasi (M-9), UI'da "yuklenebilir mi" testi,
//        sanitize-against-files turlari.
//
//   3. loadAnimation(name, anim)
//      - Tum dosyayi alloc'lu olarak heap'e yukler. En agir, en katı.
//      - Kullanim: Animasyon oynayacaksa zaten bu cagriliyor.
//      - Fail durumunda lastAnimError set ediliyor (P-5).
//
// Genel kural: UI/lookup yerlerinde faceFileExists, upload/import
// noktalarında validateFaceFile, oynatma yapilacak yerde loadAnimation.
// ============================================================
// Face cache rebuild. /faces/ dizinini bir kez tarayip .bin'lerin name'lerini
// tutar; dirty olmadan tekrar cagrilmaz. faceFileExists() bunu transparent kullanir.
void rebuildFaceCache() {
  faceCacheCount = 0;
  if(!littlefsReady) { faceCacheDirty = false; return; }
  File root = LittleFS.open("/faces");
  if(root && root.isDirectory()) {
    File f = root.openNextFile();
    while(f && faceCacheCount < MAX_FACE_CACHE) {
      if(!f.isDirectory()) {
        String fname = f.name();
        // Tmp upload dosyasini cache'e alma (M-9 ile uyumlu).
        if(fname.endsWith(".bin") && !fname.startsWith("_upload.tmp") && fname.indexOf("/_upload.tmp") < 0) {
          // ESP32 core'a gore name "/faces/X.bin" veya "X.bin" donebilir, normalize et.
          int slashIdx = fname.lastIndexOf('/');
          if(slashIdx >= 0) fname = fname.substring(slashIdx + 1);
          if(fname.endsWith(".bin")) fname = fname.substring(0, fname.length() - 4);
          if(fname.length() > 0) faceCache[faceCacheCount++] = fname;
        }
      }
      f = root.openNextFile();
    }
  }
  faceCacheDirty = false;
}

// Upload/delete sonrasi mutlaka cagrilmali; ileride file rename/replace varsa o da.
inline void invalidateFaceCache() { faceCacheDirty = true; }

bool faceFileExists(const String& name) {
  if(!littlefsReady) return false;
  if(name.length() == 0) return false;
  // Once cache'e bak. Dirty ise rebuild. Eskiden her cagri LittleFS.exists()
  // syscall'i yapiyordu; getRandomReaction* gibi sicak yollarda kumulatif maliyetti.
  if(faceCacheDirty) rebuildFaceCache();
  for(int i = 0; i < faceCacheCount; i++) {
    if(faceCache[i] == name) return true;
  }
  return false;
}

// Asil validasyon mantigi path uzerinden calisir; bu sayede hem
// /faces/{name}.bin hem de /_upload.tmp gibi gecici path'ler ayni mantikla
// dogrulanir. Yuksek seviyede validateFaceFile(name) bu helper'i sarmalar.
bool validateFaceFileAtPath(const String& path) {
  if(!littlefsReady) return false;
  if(path.length() == 0) return false;
  File f = LittleFS.open(path, "r");
  if(!f) return false;

  size_t fileSize = f.size();
  if(fileSize < BIN_HEADER_SIZE) { f.close(); return false; }

  uint8_t header[BIN_HEADER_SIZE];
  if(f.read(header, BIN_HEADER_SIZE) != BIN_HEADER_SIZE) {
    f.close();
    return false;
  }
  f.close();

  bool isV2 = (memcmp(header, BIN_MAGIC, 4) == 0);
  bool isV3 = (memcmp(header, BIN_MAGIC_V3, 4) == 0);
  if(!isV2 && !isV3) return false;

  uint8_t frame_count = header[4];
  uint8_t bytes_per_row = header[5];
  uint8_t height = header[6];
  if(frame_count == 0) return false;
  if(bytes_per_row != 16) return false;
  if(height != 64) return false;

  if(isV3) {
    uint8_t format = header[7];
    if(format != BIN_FORMAT_RLE_DELTA) return false;
    // MOC3 sikistirilmis: minimum boyut header + her frame icin en az 3 byte
    // (delay + 2 byte compressed length). Cok daha buyuk olabilir, sadece alt sinir.
    size_t minSize = (size_t)BIN_HEADER_SIZE + (size_t)frame_count * 3;
    if(fileSize < minSize) return false;
  } else {
    // MOC2 ham: tam frame_count * (1 + 16*64) byte gelmeli.
    size_t expected = (size_t)frame_count * (1 + (size_t)bytes_per_row * height);
    if(fileSize < BIN_HEADER_SIZE + expected) return false;
  }
  return true;
}

// Yuz dosyasini gercekten acip header'in 128x64 MOC2/MOC3 kurallarina
// uydugunu dogrular. faceFileExists() yalnizca dosyanin varligina bakar; bu
// fonksiyon icerik validasyonu yapar. Upload sonrasi ve riskli yuklemelerden
// once cagrilir. Tum frame'leri okumaz; sadece header sane mi diye bakar +
// dosya boyutunun beklenen minimum'u tutturup tutturmadigini kontrol eder.
bool validateFaceFile(const String& name) {
  if(name.length() == 0) return false;
  String path = "/faces/" + name + ".bin";
  return validateFaceFileAtPath(path);
}

String findFirstUsableFaceName() {
  if(!littlefsReady) return "";

  if(faceCacheDirty) rebuildFaceCache();
  for(int i = 0; i < faceCacheCount; i++) {
    String name = faceCache[i];
    if(name.length() > 0 && validateFaceFile(name)) return name;
  }

  // Cache doluysa veya beklenmedik şekilde boş kaldıysa dizini doğrudan tara.
  File root = LittleFS.open("/faces");
  if(root && root.isDirectory()) {
    File f = root.openNextFile();
    while(f) {
      if(!f.isDirectory()) {
        String fname = f.name();
        int slashIdx = fname.lastIndexOf('/');
        if(slashIdx >= 0) fname = fname.substring(slashIdx + 1);
        if(fname.endsWith(".bin") && !fname.startsWith("_upload.tmp")) {
          String name = fname.substring(0, fname.length() - 4);
          f.close();
          if(name.length() > 0 && validateFaceFile(name)) return name;
        }
      }
      f = root.openNextFile();
    }
  }

  return "";
}


// Path traversal koruyucu sanitizer. Web'den gelen "name" parametreleri
// dogrudan "/faces/" + name + ".bin" path'ine yapistirilmadan once buradan gecer.
// Kabul edilen karakterler: A-Z, a-z, 0-9, '_' ve '-'. Max 32 char.
// Donus bos ise isim red edilmeli (404 / 400). Turkce/uzantili adlar burada yok;
// kullanici dosyasini Turkce isimlendirmek isterse client tarafinda ASCII slug
// olusturmali. ESP32'de kucuk LittleFS uzerinde sade ASCII slug en guvenlisi.
String sanitizeFaceName(const String& raw) {
  if(raw.length() == 0) return "";
  if(raw.length() > 32) return "";
  for(size_t i = 0; i < raw.length(); i++) {
    char c = raw.charAt(i);
    bool ok = (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-';
    if(!ok) return "";
  }
  return raw;
}

int countFaceFiles() {
  if(!littlefsReady) return 0;
  int count = 0;
  File root = LittleFS.open("/faces");
  if(root && root.isDirectory()) {
    File f = root.openNextFile();
    while(f) {
      if(!f.isDirectory()) {
        String fname = f.name();
        // tmp upload dosyasi face sayisina dahil edilmemeli.
        if(fname.endsWith(".bin") && !fname.startsWith("_upload.tmp") && fname.indexOf("/_upload.tmp") < 0) count++;
      }
      f = root.openNextFile();
    }
  }
  return count;
}

void enterFallbackWaitMode(const String& missingName, const char* reason) {
  freeAnimation(currentAnim);
  activeAnim = nullptr;
  activeFrame = 0;
  lastFrameTime = millis();
  currentIdleStartTime = millis();
  // Yuz yuklenene/secene kadar tekrar dosya okumayi deneme.
  currentIdleDurationMs = 24UL * 60UL * 60UL * 1000UL;
  markDisplayDirty();

  if(!waitingForFaceSelection || waitingMissingFaceName != missingName) {
    waitingForFaceSelection = true;
    waitingMissingFaceName = missingName;
    if(missingName.length() > 0) {
      Serial.printf("Yuz dosyasi yok: /faces/%s.bin - fallback yuz ciziliyor. %s\n",
                    missingName.c_str(), reason);
    } else {
      Serial.printf("Yuz secili degil - fallback yuz ciziliyor. %s\n", reason);
    }
  }
}

bool decodeRLEIntoFrame(File& f, uint16_t compLen, uint8_t* out, const uint8_t* prev, size_t frameDataSize, bool delta) {
  size_t written = 0;
  size_t consumed = 0;
  while(consumed + 2 <= compLen && written < frameDataSize) {
    int count = f.read();
    int value = f.read();
    if(count < 0 || value < 0) return false;
    consumed += 2;
    for(int i = 0; i < count && written < frameDataSize; i++) {
      uint8_t b = (uint8_t)value;
      out[written] = delta ? (prev[written] ^ b) : b;
      written++;
    }
  }
  // Fazladan byte varsa atla (bozuk/gelecek format toleransi)
  while(consumed < compLen) { if(f.read() < 0) return false; consumed++; }
  return written == frameDataSize;
}

bool loadRawMoc2(File& f, Animation& anim, size_t fileSize, uint8_t* header) {
  anim.frame_count = header[4];
  anim.bytes_per_row = header[5];
  anim.height = header[6];

  // 128x64 OLED icin sabit raster boyutu zorunlu. Bozuk/yabanci dosyada
  // header degerleri buyuk olursa malloc dev miktar isteyip basarisiz olur,
  // kucukse drawing 16x64 varsaydigi icin dizi disi yazma olur.
  if(anim.frame_count == 0) return false;
  if(anim.bytes_per_row != 16) return false;
  if(anim.height != 64) return false;
  size_t frameDataSize = (size_t)anim.bytes_per_row * anim.height;
  size_t expected = (size_t)anim.frame_count * (1 + frameDataSize);
  if(fileSize < BIN_HEADER_SIZE + expected) return false;

  anim.frames_data = (uint8_t*)malloc(expected);
  if(!anim.frames_data) return false;
  if(f.read(anim.frames_data, expected) != expected) {
    free(anim.frames_data);
    anim.frames_data = nullptr;
    return false;
  }
  return true;
}

bool loadCompressedMoc3(File& f, Animation& anim, size_t fileSize, uint8_t* header) {
  (void)fileSize;
  anim.frame_count = header[4];
  anim.bytes_per_row = header[5];
  anim.height = header[6];
  uint8_t format = header[7];

  // RawMoc2 ile ayni gerekceyle 128x64 zorunlu.
  if(anim.frame_count == 0) return false;
  if(anim.bytes_per_row != 16) return false;
  if(anim.height != 64) return false;
  if(format != BIN_FORMAT_RLE_DELTA) return false;

  size_t frameDataSize = (size_t)anim.bytes_per_row * anim.height;
  size_t rawSize = (size_t)anim.frame_count * (1 + frameDataSize);
  anim.frames_data = (uint8_t*)malloc(rawSize);
  if(!anim.frames_data) return false;

  uint8_t* prev = (uint8_t*)calloc(frameDataSize, 1);
  uint8_t* frame = (uint8_t*)malloc(frameDataSize);
  if(!prev || !frame) {
    if(prev) free(prev);
    if(frame) free(frame);
    free(anim.frames_data);
    anim.frames_data = nullptr;
    return false;
  }

  for(int i = 0; i < anim.frame_count; i++) {
    int delay = f.read();
    int lo = f.read();
    int hi = f.read();
    if(delay < 0 || lo < 0 || hi < 0) {
      free(prev); free(frame); free(anim.frames_data); anim.frames_data = nullptr; return false;
    }
    uint16_t compLen = (uint16_t)lo | ((uint16_t)hi << 8);
    if(!decodeRLEIntoFrame(f, compLen, frame, prev, frameDataSize, true)) {
      free(prev); free(frame); free(anim.frames_data); anim.frames_data = nullptr; return false;
    }
    size_t offset = (size_t)i * (1 + frameDataSize);
    anim.frames_data[offset] = (uint8_t)delay;
    memcpy(anim.frames_data + offset + 1, frame, frameDataSize);
    memcpy(prev, frame, frameDataSize);
  }

  free(prev);
  free(frame);
  return true;
}

bool loadAnimation(const String& name, Animation& anim) {
  if(!littlefsReady) return false;
  if(name.length() == 0) return false;
  // WiFi acikken default/current animasyonu onceden serbest birakma.
  // AP basladiktan sonra max alloc heap dusuyor; currentAnim'i burada bosaltirsak
  // event bitince /faces/<default>.bin yeniden yuklenemeyebiliyor ve yuz fallback'e
  // dusuyor. Once eventAnim'i currentAnim resident halde yuklemeyi deniyoruz.
  // Heap yetmezse sadece o event atlanir; default yuz bozulmadan akmaya devam eder.
  freeAnimation(anim);
  anim.name = name;

  String path = "/faces/" + name + ".bin";
  File f = LittleFS.open(path, "r");
  if(!f) {
    Serial.printf("[ANIM] missing path=%s\n", path.c_str());
    lastAnimError = "Bulunamadi: " + name; lastAnimErrorAt = millis();
    return false;
  }

  uint8_t header[BIN_HEADER_SIZE];
  if(f.read(header, BIN_HEADER_SIZE) != BIN_HEADER_SIZE) {
    Serial.printf("[ANIM] bad_header path=%s\n", path.c_str());
    lastAnimError = "Eksik header: " + name; lastAnimErrorAt = millis();
    f.close();
    return false;
  }

  bool ok = false;
  size_t fileSize = f.size();
  if(memcmp(header, BIN_MAGIC, 4) == 0) {
    ok = loadRawMoc2(f, anim, fileSize, header);
  } else if(memcmp(header, BIN_MAGIC_V3, 4) == 0) {
    ok = loadCompressedMoc3(f, anim, fileSize, header);
  } else {
    Serial.printf("[ANIM] bad_magic path=%s\n", path.c_str());
    lastAnimError = "Gecersiz magic: " + name; lastAnimErrorAt = millis();
  }
  f.close();

  if(!ok) {
    Serial.printf("[ANIM] bad_file path=%s\n", path.c_str());
    if(lastAnimError.length() == 0) {
      // Magic ok'di ama parse fail - daha spesifik hata yoksa generic mesaj.
      lastAnimError = "Parse fail: " + name;
      lastAnimErrorAt = millis();
    }
    freeAnimation(anim);
    return false;
  }

  anim.name = name;
  anim.loaded = true;
  waitingForFaceSelection = false;
  waitingMissingFaceName = "";

  // Aynı Animation slot'una farklı dosya yüklendiğinde pointer adresi değişmeyebilir.
  // Bu yüzden yeni dosya yüklendiği anda ekranı zorla yeniden çizdiriyoruz.
  markDisplayDirty();

  Serial.printf("[ANIM] loaded face=%s frames=%d\n", name.c_str(), anim.frame_count);
  return true;
}

void freeAnimation(Animation& anim) {
  if(anim.frames_data) {
    free(anim.frames_data);
    anim.frames_data = nullptr;
  }
  anim.loaded = false;
  anim.frame_count = 0;
  // Esikilan slot'un name'i bos olmazsa "anim.name == X" testleri eski
  // sahibine takiliyor. ornek: deleted face name'i hala currentAnim.name'de
  // kaliyor → handleApiDelete yanlis dalları takip ediyor.
  anim.name = "";
}

void freeTransientAnimations() {
  // Tek slot'a indirildigi icin tek free yetiyor.
  freeAnimation(eventAnim);
}

bool removeNameFromArray(String* list, int& count, const String& name) {
  bool changed = false;
  int writeIdx = 0;
  for(int i = 0; i < count; i++) {
    if(list[i] == name) {
      changed = true;
      continue;
    }
    list[writeIdx] = list[i];
    writeIdx++;
  }
  for(int i = writeIdx; i < count; i++) {
    list[i] = "";
  }
  count = writeIdx;
  return changed;
}

// idleAnims paralel array yerine struct olunca, removeNameFromArray
// generic kaldi; idle icin dedicated fonksiyon, struct alanlarinin tutarliligini
// garanti ediyor.
bool removeIdleAnimByName(const String& name) {
  bool changed = false;
  int writeIdx = 0;
  for(int i = 0; i < idleAnimCount; i++) {
    if(idleAnims[i].name == name) {
      changed = true;
      continue;
    }
    if(writeIdx != i) {
      idleAnims[writeIdx] = idleAnims[i];
    }
    writeIdx++;
  }
  for(int i = writeIdx; i < idleAnimCount; i++) {
    idleAnims[i].name = "";
    idleAnims[i].loops = 1;
    idleAnims[i].seconds = 0;
  }
  idleAnimCount = writeIdx;
  return changed;
}

bool listAlreadyHasName(String* list, int count, const String& name) {
  if(name.length() == 0) return false;
  for(int i = 0; i < count; i++) {
    if(list[i] == name) return true;
  }
  return false;
}

bool pruneMissingFacesFromArray(String* list, int& count) {
  if(!littlefsReady) return false;
  bool changed = false;
  int writeIdx = 0;
  for(int i = 0; i < count; i++) {
    String name = list[i];
    if(name.length() > 0 && faceFileExists(name) && !listAlreadyHasName(list, writeIdx, name)) {
      list[writeIdx] = name;
      writeIdx++;
    } else {
      changed = true;
    }
  }
  for(int i = writeIdx; i < count; i++) {
    list[i] = "";
  }
  if(count != writeIdx) changed = true;
  count = writeIdx;
  return changed;
}

// Idle struct array icin dedicated prune. Yuz dosyasi yoksa veya bos isim
// varsa eleman atilir; loops/seconds otomatik gelir cunku struct'in parcasi.
bool pruneMissingIdleAnims() {
  if(!littlefsReady) return false;
  bool changed = false;
  int writeIdx = 0;
  for(int i = 0; i < idleAnimCount; i++) {
    String name = idleAnims[i].name;
    if(name.length() == 0 || !faceFileExists(name)) {
      changed = true;
      continue;
    }
    // Duplicate kontrolu: writeIdx oncesindeki struct'larda ayni isim var mi
    bool dup = false;
    for(int j = 0; j < writeIdx; j++) {
      if(idleAnims[j].name == name) { dup = true; break; }
    }
    if(dup) {
      changed = true;
      continue;
    }
    if(writeIdx != i) idleAnims[writeIdx] = idleAnims[i];
    writeIdx++;
  }
  for(int i = writeIdx; i < idleAnimCount; i++) {
    idleAnims[i].name = "";
    idleAnims[i].loops = 1;
    idleAnims[i].seconds = 0;
  }
  idleAnimCount = writeIdx;
  return changed;
}

bool sanitizeConfigAgainstFiles(bool saveAfter) {
  if(!littlefsReady) return false;
  bool changed = false;

  // Default yuz artik Yuzler sekmesindeki aktif yuzle ayni kavram.
  if(config.currentFace.length() == 0 && config.defaultFace.length() > 0) {
    config.currentFace = config.defaultFace;
    changed = true;
  }

  if(config.currentFace.length() > 0 && !validateFaceFile(config.currentFace)) {
    Serial.printf("Config aktif yuz dosyasi yok/bozuk: %s\n", config.currentFace.c_str());
    config.currentFace = "";
    changed = true;
  }

  if(config.currentFace.length() == 0) {
    String fallbackFace = findFirstUsableFaceName();
    if(fallbackFace.length() > 0) {
      config.currentFace = fallbackFace;
      Serial.printf("Config aktif yuz otomatik secildi: %s\n", fallbackFace.c_str());
      changed = true;
    }
  }

  if(config.defaultFace != config.currentFace) {
    config.defaultFace = config.currentFace;
    changed = true;
  }

  if(config.sleepEnterFace.length() > 0 && !faceFileExists(config.sleepEnterFace)) {
    Serial.printf("Config uykuya giris animasyonu yok, temizlendi: %s\n", config.sleepEnterFace.c_str());
    config.sleepEnterFace = "";
    changed = true;
  }
  if(config.sleepWakeFace.length() > 0 && !faceFileExists(config.sleepWakeFace)) {
    Serial.printf("Config uyanma animasyonu yok, temizlendi: %s\n", config.sleepWakeFace.c_str());
    config.sleepWakeFace = "";
    changed = true;
  }

  changed |= pruneMissingIdleAnims();
  changed |= pruneMissingFacesFromArray(pokeReactions, pokeReactionCount);
  changed |= pruneMissingFacesFromArray(pettingReactions, pettingReactionCount);
  changed |= pruneMissingFacesFromArray(deepPettingReactions, deepPettingReactionCount);
  changed |= pruneMissingFacesFromArray(openingEvents, openingEventCount);
  changed |= pruneMissingFacesFromArray(randomEvents, randomEventCount);
  changed |= pruneMissingFacesFromArray(sleepEnterEvents, sleepEnterEventCount);
  changed |= pruneMissingFacesFromArray(sleepWakeEvents, sleepWakeEventCount);
  // ivmeolcer reaction listelerini de budala
  changed |= pruneMissingFacesFromArray(accelSoftShakeReactions,   accelSoftShakeCount);
  changed |= pruneMissingFacesFromArray(accelHardShakeReactions,   accelHardShakeCount);
  changed |= pruneMissingFacesFromArray(accelAccelReactions,       accelAccelCount);
  changed |= pruneMissingFacesFromArray(accelFreefallReactions,    accelFreefallCount);
  changed |= pruneMissingFacesFromArray(accelOrientationReactions, accelOrientationCount);
  changed |= pruneMissingFacesFromArray(accelPickupReactions,      accelPickupCount);

  // Listelerden silinen yuzler recent*History dizilerinde tutulu kalabiliyor.
  // Bu, "son 4 secimi tekrar etme" filtresinin var olmayan isimlere takilip
  // gercek listeden kacinmasina yol acabiliyor (ozellikle kucuk listelerde).
  // Bir sey budandiysa tum recent history'leri sifirla.
  if(changed) resetRandomHistories();

  if(changed && saveAfter) saveConfig();
  return changed;
}


String getFaceCategory(const String& name) {
  for(int i = 0; i < faceCategoryCount; i++) {
    if(faceCategoryNames[i] == name) return faceCategoryValues[i];
  }
  return "uncategorized";
}

void setFaceCategory(const String& name, const String& category) {
  if(name.length() == 0) return;
  String cat = category;
  if(cat.length() == 0) cat = "uncategorized";
  for(int i = 0; i < faceCategoryCount; i++) {
    if(faceCategoryNames[i] == name) {
      faceCategoryValues[i] = cat;
      return;
    }
  }
  if(faceCategoryCount < 80) {
    faceCategoryNames[faceCategoryCount] = name;
    faceCategoryValues[faceCategoryCount] = cat;
    faceCategoryCount++;
  }
}

void removeFaceCategory(const String& name) {
  int writeIdx = 0;
  for(int i = 0; i < faceCategoryCount; i++) {
    if(faceCategoryNames[i] == name) continue;
    faceCategoryNames[writeIdx] = faceCategoryNames[i];
    faceCategoryValues[writeIdx] = faceCategoryValues[i];
    writeIdx++;
  }
  for(int i = writeIdx; i < faceCategoryCount; i++) {
    faceCategoryNames[i] = "";
    faceCategoryValues[i] = "";
  }
  faceCategoryCount = writeIdx;
}

// Frame i'inci frame'in delay'ini dondur (ms)
uint16_t getFrameDelay(Animation& anim, int frameIdx) {
  if(!anim.loaded || frameIdx >= anim.frame_count) return 100;
  
  int frameDataSize = anim.bytes_per_row * anim.height;
  int offset = frameIdx * (1 + frameDataSize);  // her frame: 1 byte delay + data
  
  uint8_t delay_units = anim.frames_data[offset];
  return (uint16_t)delay_units * 10;
}

// Frame i'inci frame'in pixel data'sini dondur
uint8_t* getFrameData(Animation& anim, int frameIdx) {
  if(!anim.loaded || frameIdx >= anim.frame_count) return nullptr;
  
  int frameDataSize = anim.bytes_per_row * anim.height;
  int offset = frameIdx * (1 + frameDataSize) + 1;  // delay byte'i atla
  
  return anim.frames_data + offset;
}

// ============================================================
// ACILIS ANIMASYONU
// ============================================================
void drawHelloThereFrame(int x, int underlineWidth) {
  const char* text = "Hello there!";

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB10_tr);

  // Yaziyi dikeyde ortala; SH1106 128x64 icin guvenli alan.
  int y = 36;
  u8g2.drawStr(x, y, text);

  // Kucuk bir alt cizgi efekti: girerken/genislerken, cikarken daralir.
  int centerX = 64;
  int half = underlineWidth / 2;
  if(underlineWidth > 0) {
    u8g2.drawHLine(centerX - half, 43, underlineWidth);
  }

  u8g2.sendBuffer();
}

void playHelloThereAnimation() {
  const char* text = "Hello there!";
  u8g2.setFont(u8g2_font_ncenB10_tr);
  int textWidth = u8g2.getStrWidth(text);
  int targetX = (128 - textWidth) / 2;
  if(targetX < 0) targetX = 0;

  // Soldan giris
  for(int x = -textWidth; x <= targetX; x += 6) {
    int progress = map(x, -textWidth, targetX, 0, 52);
    if(progress < 0) progress = 0;
    if(progress > 52) progress = 52;
    drawHelloThereFrame(x, progress);
    delay(28);
  }

  // Kisa bekleme + ufak nefes efekti
  unsigned long holdStart = millis();
  while(millis() - holdStart < 650) {
    int pulse = ((millis() / 120) % 2) ? 56 : 44;
    drawHelloThereFrame(targetX, pulse);
    delay(60);
  }

  // Saga cikis
  for(int x = targetX; x <= 128; x += 6) {
    int remaining = map(x, targetX, 128, 52, 0);
    if(remaining < 0) remaining = 0;
    if(remaining > 52) remaining = 52;
    drawHelloThereFrame(x, remaining);
    delay(24);
  }

  u8g2.clearBuffer();
  u8g2.sendBuffer();
}

bool playStartupOpeningAnimation() {
  if(openingEventCount <= 0) {
    Serial.println("[OPENING] skip reason=no_opening_face fallback=hello");
    return false;
  }

  String name = getRandomReaction(openingEvents, openingEventCount);
  if(name.length() == 0) {
    Serial.println("[OPENING] skip reason=missing_selected fallback=hello");
    return false;
  }

  if(!loadAnimation(name, eventAnim)) {
    Serial.printf("[OPENING] fail face=%s reason=load_failed\n", name.c_str());
    return false;
  }

  Serial.printf("[OPENING] start face=%s\n", name.c_str());

  int frame = 0;
  unsigned long start = millis();
  unsigned long total = getAnimationTotalDurationMs(eventAnim);
  // Acilista bir tur oynat; cok uzun dosyalarda boot'u kilitlememek icin 6 sn ile sinirla.
  unsigned long playMs = max((unsigned long)800, min((unsigned long)6000, total));

  while(millis() - start < playMs) {
    uint8_t* frameData = getFrameData(eventAnim, frame);
    if(frameData) {
      u8g2.clearBuffer();
      u8g2.drawXBM(0, 0, 128, 64, frameData);
      u8g2.sendBuffer();
    }

    uint16_t delayMs = getFrameDelay(eventAnim, frame);
    delayMs = (uint16_t)(delayMs / speedMultiplier);
    if(delayMs < 20) delayMs = 20;

    unsigned long frameStart = millis();
    while(millis() - frameStart < delayMs && millis() - start < playMs) {
      delay(2);
    }

    frame = (frame + 1) % eventAnim.frame_count;
  }

  freeAnimation(eventAnim);
  activeFrame = 0;
  lastFrameTime = millis();
  return true;
}

bool playNamedAnimationBlocking(const String& name, unsigned long durationMs, bool abortOnTouch) {
  if(name.length() == 0 || !faceFileExists(name)) return false;
  if(!loadAnimation(name, eventAnim)) return false;

  u8g2.setPowerSave(0);
  applyDisplayBrightness();

  unsigned long total = getAnimationTotalDurationMs(eventAnim);
  // durationMs = 0 ise animasyon kendi toplam suresi kadar oynar.
  // Global hiz carpani yine uygulanir; kullanici uyku/uyanma icin ekstra ms belirlemez.
  unsigned long playMs = durationMs > 0 ? durationMs : (unsigned long)(total / speedMultiplier);
  if(playMs < 100UL) playMs = 100UL;
  // Sadece disaridan ozel sure verilirse eski guvenlik clamp'i uygula.
  if(durationMs > 0 && playMs > (unsigned long)MAX_SLEEP_ANIM_MS) playMs = MAX_SLEEP_ANIM_MS;

  int frame = 0;
  unsigned long start = millis();
  sleepTransitionRunning = true;

  while(millis() - start < playMs) {
    if(abortOnTouch && readTouchActive()) {
      sleepTransitionRunning = false;
      freeAnimation(eventAnim);
      activeFrame = 0;
      lastFrameTime = millis();
      return false;
    }

    uint8_t* frameData = getFrameData(eventAnim, frame);
    if(frameData) {
      u8g2.clearBuffer();
      u8g2.drawXBM(0, 0, 128, 64, frameData);
      u8g2.sendBuffer();
    }

    uint16_t delayMs = getFrameDelay(eventAnim, frame);
    delayMs = (uint16_t)(delayMs / speedMultiplier);
    if(delayMs < 20) delayMs = 20;

    unsigned long frameStart = millis();
    while(millis() - frameStart < delayMs && millis() - start < playMs) {
      if(abortOnTouch && readTouchActive()) {
        sleepTransitionRunning = false;
        freeAnimation(eventAnim);
        activeFrame = 0;
        lastFrameTime = millis();
        return false;
      }
      delay(2);
    }

    frame = (frame + 1) % eventAnim.frame_count;
  }

  sleepTransitionRunning = false;
  freeAnimation(eventAnim);
  activeFrame = 0;
  lastFrameTime = millis();
  return true;
}

void enterSleepMode() {
  if(systemSleeping || !config.sleepEnabled) return;
  if(prevTouch) return;

  Serial.println("Uyku moduna geciliyor");
  showingBatteryNotification = false;
  showingWifiNotification = false;

  // Uyku modunda web server/AP/STA tamamen kapansin;
  // dokunma disinda hicbir is pili yemesin.
  stopAllWirelessForSleep();

  mood = MOOD_IDLE;
  idleEventPlaying = false;
  nextIdleEventAt = 0;
  nextRandomEventAt = 0;
  freeTransientAnimations();

  // Uykuya giris animasyonlari seciliyse iclerinden biri rastgele secilir
  // ve kendi dogal toplam suresi kadar oynar. Liste bos ise direkt kapanir.
  String sleepInName = getRandomReactionWithHistory(sleepEnterEvents, sleepEnterEventCount, recentSleepEnterHistory, recentSleepEnterHistoryCount);
  if(sleepInName.length() > 0) {
    bool completed = playNamedAnimationBlocking(sleepInName, 0, true);
    if(!completed) {
      Serial.println("Uykuya giris dokunmayla iptal edildi");
      lastInteractionTime = millis();
      loadDefaultIdleFace(true);
      scheduleNextRandomEvent();
      return;
    }
  }

  u8g2.clearBuffer();
  u8g2.sendBuffer();
  u8g2.setPowerSave(1);

  // Bu noktadan sonra cihaz uyku modunda kabul edilir. Bunu deep sleep'ten once
  // set ediyoruz ki son anda dokunma gelirse wakeFromSleepMode() guvenli calissin.
  systemSleeping = true;
  sleepEnteredAt = millis();
  // Uyku boyunca sadece SERT sallanma INT1 uretsin; yumusak hareket uyandirmasin.
  if(accelReady) applyAccelHardwareConfig(accelHardThreshold);
  tapCount = 0;
  prevTouch = false;
  ignoreTouchUntilRelease = false;

  if(config.sleepMode == SLEEP_MODE_DEEP) {
    enterDeepSleepUntilTouch();
    return;
  }
}


void stopAllWirelessForSleep() {
  // AP aciksa normal kapatma rutinini kullan; server.stop() da burada yapiliyor.
  if(wifiActive) {
    stopWiFiAP();
  }

  // STA auto-connect aciksa AP kapali olsa bile WiFi radyosu acik kalabilir.
  // Uyku modunda bunu da kapatiyoruz; uyaninca kendiliginden acilmiyor.
  server.stop();
  WiFi.disconnect(true, true);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiActive = false;
  showingWifiNotification = false;
}

void enterMinimalLightSleepUntilTouch() {
  if(!systemSleeping) return;

  if(readTouchActive()) {
    wakeFromSleepMode();
    return;
  }

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  gpio_wakeup_enable((gpio_num_t)PIN_TOUCH, GPIO_INTR_HIGH_LEVEL);
  // Sert sallanma light sleep'i de uyandirsin (esik uykuda hard'a yukseltildi;
  // transient latch pini SRC okunana kadar HIGH tutar, level-wake garantili).
  if(accelReady) gpio_wakeup_enable((gpio_num_t)PIN_ACCEL_INT1, GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  bool hybridMayDeepen = (config.sleepMode == SLEEP_MODE_HYBRID);
  if(hybridMayDeepen) {
    unsigned long lightToDeepMs = (unsigned long)config.sleepDeepAfterMinutes * 60UL * 1000UL;
    if(lightToDeepMs < 60000UL) lightToDeepMs = 60000UL;
    unsigned long elapsed = millis() - sleepEnteredAt;
    if(elapsed >= lightToDeepMs) {
      gpio_wakeup_disable((gpio_num_t)PIN_TOUCH);
      enterDeepSleepUntilTouch();
      return;
    }
    unsigned long remainMs = lightToDeepMs - elapsed;
    esp_sleep_enable_timer_wakeup((uint64_t)remainMs * 1000ULL);
  }

  esp_err_t err = esp_light_sleep_start();
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  gpio_wakeup_disable((gpio_num_t)PIN_TOUCH);
  if(accelReady) gpio_wakeup_disable((gpio_num_t)PIN_ACCEL_INT1);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

  if(err != ESP_OK) {
    delay(20);
  }

  // INT1 (sert sallanma) ile uyandiysa hard reaction'i kuyrukla —
  // wakeFromSleepMode uyanma animasyonunu oynattiktan sonra handleAccelEvents isler.
  if(accelReady && digitalRead(PIN_ACCEL_INT1)) {
    accelHardWakePending = true;
  }

  if(readTouchActive() || cause == ESP_SLEEP_WAKEUP_GPIO) {
    wakeFromSleepMode();
    return;
  }

  if(hybridMayDeepen && cause == ESP_SLEEP_WAKEUP_TIMER) {
    enterDeepSleepUntilTouch();
  }
}

bool enableDeepSleepWakeOnTouch() {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

  // ESP32-C3 Arduino core tarafinda deep sleep GPIO wake icin kullanilan standart API.
  // PIN_TOUCH=GPIO1; TTP223 dokununca HIGH verdigi icin HIGH level ile uyandiriyoruz.
  // PIN_ACCEL_INT1=GPIO2 de wake kaynagi — sert sallanma deep sleep'ten uyandirabilmeli.
  // NOT: GPIO1 dijital giris kati hasarli oldugundan dokunmanin deep sleep'i
  // uyandirmasi guvenilmez olabilir (deep sleep GPIO wake dijital esik ister,
  // ADC uykuda calismaz). Guvenilir deep-sleep wake kaynagi: sert sallanma (INT1).
  // Deep sleep'te SADECE sert sallanma uyandirsin diye transient esigi hard'a yukselt;
  // uyanista setupAccelerometer -> applyAccelHardwareConfig soft esige geri doner.
  if(accelReady) applyAccelHardwareConfig(accelHardThreshold);
  uint64_t wakeMask = (1ULL << PIN_TOUCH);
  if(accelReady) wakeMask |= (1ULL << PIN_ACCEL_INT1);
  esp_err_t err = esp_deep_sleep_enable_gpio_wakeup(wakeMask, ESP_GPIO_WAKEUP_GPIO_HIGH);
  if(err != ESP_OK) {
    Serial.printf("Deep sleep GPIO wake kurulamadı: %d\n", (int)err);
    return false;
  }
  return true;
}

void enterDeepSleepUntilTouch() {
  Serial.println("Deep sleep moduna geciliyor - sadece TTP223 dokunma uyandirir");
  Serial.flush();

  stopAllWirelessForSleep();
  u8g2.clearBuffer();
  u8g2.sendBuffer();
  u8g2.setPowerSave(1);
  delay(20);

  if(readTouchActive()) {
    // Parmak hala uzerindeyse deep sleep'e girip aninda uyanma dongusune dusmeyelim.
    wakeFromSleepMode();
    return;
  }

  if(!enableDeepSleepWakeOnTouch()) {
    // Wake kaynagi kurulamazsa cihaz kilitli kalmasin; light sleep'e geri don.
    systemSleeping = true;
    sleepEnteredAt = millis();
    return;
  }

  mochiDeepSleepMagic = MOCHI_DEEP_SLEEP_MAGIC;
  esp_deep_sleep_start();
}

void wakeFromSleepMode() {
  if(!systemSleeping) return;

  Serial.println("Uyku modundan cikiliyor");
  systemSleeping = false;

  // Uykuda hard'a yukseltilen transient esigini normale dondur,
  // uyku sirasinda birikmis latch'leri temizle.
  if(accelReady) {
    applyAccelHardwareConfig(accelSoftThreshold);
    accelInt1Fired = false;
    accelInt2Fired = false;
  }

  // Light sleep sirasinda SH1106'nin segment remap (A0/A1) ve COM scan
  // direction (C0/C8) registerlari bazi kart kombinasyonlarinda reset varsayilanina
  // donebiliyor; bu durumda ekran ters/aynalanmis goruluyordu. initDisplay()
  // tum init sequence'ini yeniden gondererek bu registerlari U8g2 R0 yonelimine
  // geri ayarlar. Buffer'a dokunmaz.
  u8g2.initDisplay();
  u8g2.setPowerSave(0);
  applyDisplayBrightness();
  u8g2.clearBuffer();
  u8g2.sendBuffer();
  // Wake'te initDisplay registerlari resetledi, drawCurrentFrame zorla redraw etmeli.
  markDisplayDirty();
  lastDrawnAnim = nullptr;
  lastDrawnFrame = -1;
  lastInteractionTime = millis();
  tapCount = 0;
  ignoreTouchUntilRelease = true;
  touchStartTime = millis();
  showingBatteryNotification = false;
  showingWifiNotification = false;

  // Uyanma animasyonlari seciliyse iclerinden biri rastgele secilir
  // ve kendi dogal toplam suresi kadar oynar. Liste bos ise direkt default yuze doner.
  String wakeName = getRandomReactionWithHistory(sleepWakeEvents, sleepWakeEventCount, recentSleepWakeHistory, recentSleepWakeHistoryCount);
  if(wakeName.length() > 0) {
    playNamedAnimationBlocking(wakeName, 0, false);
  }

  mood = MOOD_IDLE;
  loadDefaultIdleFace(true);
  scheduleNextRandomEvent();
}

void checkSleepMode() {
  if(systemSleeping || !config.sleepEnabled) return;
  if(config.sleepTimeoutMinutes < MIN_SLEEP_TIMEOUT_MINUTES) config.sleepTimeoutMinutes = MIN_SLEEP_TIMEOUT_MINUTES;
  if(prevTouch || sleepTransitionRunning) return;

  unsigned long timeoutMs = (unsigned long)config.sleepTimeoutMinutes * 60UL * 1000UL;
  if(timeoutMs < 60000UL) timeoutMs = 60000UL;

  // Sure dolduysa uykuya gec. Bildirim/idle/random/event uykuyu ertelemez;
  // enterSleepMode() zaten hepsini kapatip ekrani minimal moda alir.
  if(millis() - lastInteractionTime >= timeoutMs) {
    enterSleepMode();
  }
}

// ============================================================
// CONFIG
// ============================================================
// JSON dump/load helper'lari. saveConfig + loadConfig +
// handleApiConfig export/import 7 farkli string-listesi icin ayni patten'i
// tekrar tekrar yaziyordu (~14 dump + 5 load = ~75 satir tekrar). Bu
// helper'lar her cagriyi 1 satira indiriyor.
inline void dumpStringArrayToJson(JsonDocument& doc, const char* key, String* arr, int count) {
  JsonArray a = doc[key].to<JsonArray>();
  for(int i = 0; i < count; i++) a.add(arr[i]);
}

// JsonObject overload - handleApiConfig export icin (root yerine sub-object'e yazma).
inline void dumpStringArrayToJson(JsonObject obj, const char* key, String* arr, int count) {
  JsonArray a = obj[key].to<JsonArray>();
  for(int i = 0; i < count; i++) a.add(arr[i]);
}

// loadConfig pattern'i: count'u sifirla, JsonArray varsa max kapasiteye kadar
// string'leri arr'a doldur. Bos string'leri atla (eski sanitize davranisi).
inline void loadStringArrayFromJson(JsonDocument& doc, const char* key, String* arr, int& count, int maxCount) {
  count = 0;
  if(doc[key].is<JsonArray>()) {
    for(JsonVariant v : doc[key].as<JsonArray>()) {
      if(count >= maxCount) break;
      String name = v.as<String>();
      if(name.length() > 0) arr[count++] = name;
    }
  }
}

// JsonObject overload: accel sub-object'ten dizi yuklemek icin
inline void loadStringArrayFromJson(JsonObject obj, const char* key, String* arr, int& count, int maxCount) {
  count = 0;
  if(obj[key].is<JsonArray>()) {
    for(JsonVariant v : obj[key].as<JsonArray>()) {
      if(count >= maxCount) break;
      String name = v.as<String>();
      if(name.length() > 0) arr[count++] = name;
    }
  }
}

void loadConfig() {
  // Default yuz artik Yuzler sekmesindeki aktif yuzdur.
  // Eski configlerle uyum icin defaultFace alanini tutuyoruz ama currentFace ile ayni kullaniliyor.
  config.defaultFace = "";
  config.currentFace = "";
  config.wifiSSID = "";
  config.wifiPassword = "";
  config.wifiAutoConnect = false;
  config.wifiTimeoutMinutes = 15;
  config.wifiTapCount = DEFAULT_TAP_TRIGGER_COUNT;
  config.wifiTapWindowSec = DEFAULT_WIFI_TAP_WINDOW_SEC;
  config.batteryTapCount = DEFAULT_BATTERY_TAP_COUNT;
  config.batteryTapWindowSec = DEFAULT_BATTERY_TAP_WINDOW_SEC;
  randomEventMinMs = 15000;
  randomEventMaxMs = 20000;
  idleEventMinMs = (unsigned long)DEFAULT_IDLE_EVENT_MIN_SEC * 1000UL;
  idleEventMaxMs = (unsigned long)DEFAULT_IDLE_EVENT_MAX_SEC * 1000UL;
  faceCategoryCount = 0;
  config.longPressMs = DEFAULT_LONG_PRESS_MS;
  config.deepPettingMs = DEFAULT_DEEP_PETTING_MS;
  config.touchDebounceMs = DEFAULT_TOUCH_DEBOUNCE_MS;
  config.displayBrightnessPercent = DEFAULT_DISPLAY_BRIGHTNESS_PERCENT;
  config.sleepEnabled = DEFAULT_SLEEP_ENABLED;
  config.sleepTimeoutMinutes = DEFAULT_SLEEP_TIMEOUT_MINUTES;
  config.sleepEnterFace = "";
  config.sleepWakeFace = "";
  sleepEnterEventCount = 0;
  sleepWakeEventCount = 0;
  // sleepEnterMs/sleepWakeMs alanlari kaldirildi.
  config.sleepMode = DEFAULT_SLEEP_MODE;
  config.sleepDeepAfterMinutes = DEFAULT_SLEEP_DEEP_AFTER_MINUTES;
  openingEventCount = 0;
  randomEventCount = 0;
  // Her load baslangicinda durumu varsayilan olarak ok kabul ediyoruz;
  // alttaki dallar gerekirse degistiriyor.
  configStatus = "ok";
  
  if(!littlefsReady) {
    Serial.println("Config okunamadi: LittleFS hazir degil");
    configStatus = "fallback-defaults";
    return;
  }

  // Onceki saveConfig yarida kaldiysa /config.tmp kalintisi olabilir.
  // Burada temizlemek paranoyak ama ucuz.
  if(LittleFS.exists("/config.tmp")) {
    LittleFS.remove("/config.tmp");
  }

  if(!LittleFS.exists("/config.json")) {
    // Belki onceki acilista /config.bak yedek kalmıs olabilir.
    if(LittleFS.exists("/config.bak")) {
      Serial.println("config.json yok ama config.bak bulundu - geri yukleniyor");
      LittleFS.rename("/config.bak", "/config.json");
      configStatus = "restored-bak";
    } else {
      configStatus = "fresh";
      saveConfig();
      return;
    }
  }

  // Parse fail durumunda /config.bak'a fallback dene.
  bool parsedOk = false;
  {
    File f = LittleFS.open("/config.json", "r");
    if(f) {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, f);
      f.close();
      if(!err) {
        // BASARILI parse - asagidaki block'a bilgi vermek icin doc'u yeniden okumak yerine
        // surec icinde kullaniyoruz. Bu blogu kapatip alt bloga gecmek yerine
        // doc'u burada parse etmis durumdayiz; ana parse'a dusturmek icin asagi
        // taşıyalim.
        parsedOk = true;
      } else {
        Serial.println("Config parse hatasi - .bak deneniyor");
      }
    }
  }

  if(!parsedOk) {
    if(LittleFS.exists("/config.bak")) {
      // .bak'i config.json olarak kullan
      LittleFS.remove("/config.json");
      if(LittleFS.rename("/config.bak", "/config.json")) {
        Serial.println("Config .bak'tan geri yuklendi");
        configStatus = "restored-bak";
      } else {
        Serial.println("Config .bak rename basarisiz");
        configStatus = "fallback-defaults";
        return;
      }
    } else {
      Serial.println("Config parse fail ve yedek yok - default ile devam");
      configStatus = "fallback-defaults";
      return;
    }
  }

  File f = LittleFS.open("/config.json", "r");
  if(!f) return;
  
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  
  if(err) {
    Serial.println("Config parse hatasi (.bak da basarisiz)");
    return;
  }
  
  // Geriye donuk uyumluluk: eski config'te defaultFace varsa currentFace bosken onu kullan.
  String storedDefaultFace = doc["defaultFace"] | "";
  config.currentFace = doc["currentFace"] | storedDefaultFace;
  config.defaultFace = config.currentFace;
  config.wifiSSID = doc["wifiSSID"] | "";
  config.wifiPassword = doc["wifiPassword"] | "";
  config.wifiAutoConnect = doc["wifiAutoConnect"] | false;
  config.wifiTimeoutMinutes = doc["wifiTimeoutMinutes"] | 15;
  if(config.wifiTimeoutMinutes < 1) config.wifiTimeoutMinutes = 1;
  if(config.wifiTimeoutMinutes > 120) config.wifiTimeoutMinutes = 120;
  config.wifiTapCount = doc["wifiTapCount"] | DEFAULT_TAP_TRIGGER_COUNT;
  if(config.wifiTapCount < MIN_TAP_TRIGGER_COUNT) config.wifiTapCount = MIN_TAP_TRIGGER_COUNT;
  if(config.wifiTapCount > MAX_TAP_TRIGGER_COUNT) config.wifiTapCount = MAX_TAP_TRIGGER_COUNT;
  config.wifiTapWindowSec = doc["wifiTapWindowSec"] | DEFAULT_WIFI_TAP_WINDOW_SEC;
  if(config.wifiTapWindowSec < MIN_TAP_WINDOW_SEC) config.wifiTapWindowSec = MIN_TAP_WINDOW_SEC;
  if(config.wifiTapWindowSec > MAX_TAP_WINDOW_SEC) config.wifiTapWindowSec = MAX_TAP_WINDOW_SEC;
  config.batteryTapCount = doc["batteryTapCount"] | DEFAULT_BATTERY_TAP_COUNT;
  if(config.batteryTapCount < MIN_TAP_TRIGGER_COUNT) config.batteryTapCount = MIN_TAP_TRIGGER_COUNT;
  if(config.batteryTapCount > MAX_TAP_TRIGGER_COUNT) config.batteryTapCount = MAX_TAP_TRIGGER_COUNT;
  config.batteryTapWindowSec = doc["batteryTapWindowSec"] | DEFAULT_BATTERY_TAP_WINDOW_SEC;
  if(config.batteryTapWindowSec < MIN_TAP_WINDOW_SEC) config.batteryTapWindowSec = MIN_TAP_WINDOW_SEC;
  if(config.batteryTapWindowSec > MAX_TAP_WINDOW_SEC) config.batteryTapWindowSec = MAX_TAP_WINDOW_SEC;
  config.longPressMs = doc["longPressMs"] | DEFAULT_LONG_PRESS_MS;
  if(config.longPressMs < LONG_PRESS_MIN_MS) config.longPressMs = LONG_PRESS_MIN_MS;
  if(config.longPressMs > LONG_PRESS_MAX_MS) config.longPressMs = LONG_PRESS_MAX_MS;
  config.deepPettingMs = doc["deepPettingMs"] | DEFAULT_DEEP_PETTING_MS;
  if(config.deepPettingMs < DEEP_PETTING_MIN_MS) config.deepPettingMs = DEEP_PETTING_MIN_MS;
  if(config.deepPettingMs > DEEP_PETTING_MAX_MS) config.deepPettingMs = DEEP_PETTING_MAX_MS;
  config.touchDebounceMs = doc["touchDebounceMs"] | DEFAULT_TOUCH_DEBOUNCE_MS;
  if(config.touchDebounceMs < TOUCH_DEBOUNCE_MIN_MS) config.touchDebounceMs = TOUCH_DEBOUNCE_MIN_MS;
  if(config.touchDebounceMs > TOUCH_DEBOUNCE_MAX_MS) config.touchDebounceMs = TOUCH_DEBOUNCE_MAX_MS;
  config.displayBrightnessPercent = doc["displayBrightnessPercent"] | DEFAULT_DISPLAY_BRIGHTNESS_PERCENT;
  if(config.displayBrightnessPercent < MIN_DISPLAY_BRIGHTNESS_PERCENT) config.displayBrightnessPercent = MIN_DISPLAY_BRIGHTNESS_PERCENT;
  if(config.displayBrightnessPercent > MAX_DISPLAY_BRIGHTNESS_PERCENT) config.displayBrightnessPercent = MAX_DISPLAY_BRIGHTNESS_PERCENT;
  config.sleepEnabled = doc["sleepEnabled"] | DEFAULT_SLEEP_ENABLED;
  config.sleepTimeoutMinutes = doc["sleepTimeoutMinutes"] | DEFAULT_SLEEP_TIMEOUT_MINUTES;
  if(config.sleepTimeoutMinutes < MIN_SLEEP_TIMEOUT_MINUTES) config.sleepTimeoutMinutes = MIN_SLEEP_TIMEOUT_MINUTES;
  if(config.sleepTimeoutMinutes > MAX_SLEEP_TIMEOUT_MINUTES) config.sleepTimeoutMinutes = MAX_SLEEP_TIMEOUT_MINUTES;
  config.sleepEnterFace = doc["sleepEnterFace"] | "";
  config.sleepWakeFace = doc["sleepWakeFace"] | "";
  // sleepEnterMs / sleepWakeMs okuma silindi - alan struct'tan kaldirildi,
  // eski config.json'larda varsa ihmal ediliyor.
  config.sleepMode = doc["sleepMode"] | DEFAULT_SLEEP_MODE;
  if(config.sleepMode < SLEEP_MODE_LIGHT || config.sleepMode > SLEEP_MODE_HYBRID) config.sleepMode = DEFAULT_SLEEP_MODE;
  config.sleepDeepAfterMinutes = doc["sleepDeepAfterMinutes"] | DEFAULT_SLEEP_DEEP_AFTER_MINUTES;
  if(config.sleepDeepAfterMinutes < MIN_SLEEP_DEEP_AFTER_MINUTES) config.sleepDeepAfterMinutes = MIN_SLEEP_DEEP_AFTER_MINUTES;
  if(config.sleepDeepAfterMinutes > MAX_SLEEP_DEEP_AFTER_MINUTES) config.sleepDeepAfterMinutes = MAX_SLEEP_DEEP_AFTER_MINUTES;
  speedMultiplier = doc["speedMultiplier"] | 1.0;
  randomEventMinMs = (unsigned long)(doc["randomEventMinSec"] | DEFAULT_RANDOM_EVENT_MIN_SEC) * 1000UL;
  randomEventMaxMs = (unsigned long)(doc["randomEventMaxSec"] | DEFAULT_RANDOM_EVENT_MAX_SEC) * 1000UL;
  if(randomEventMinMs < 5000UL) randomEventMinMs = 5000UL;
  if(randomEventMinMs > 120000UL) randomEventMinMs = 120000UL;
  if(randomEventMaxMs > 120000UL) randomEventMaxMs = 120000UL;
  if(randomEventMaxMs < randomEventMinMs) randomEventMaxMs = randomEventMinMs;
  if(randomEventMinMs == 15000UL && randomEventMaxMs == 20000UL) {
    randomEventMinMs = (unsigned long)DEFAULT_RANDOM_EVENT_MIN_SEC * 1000UL;
    randomEventMaxMs = (unsigned long)DEFAULT_RANDOM_EVENT_MAX_SEC * 1000UL;
  }
  idleEventMinMs = (unsigned long)(doc["idleEventMinSec"] | DEFAULT_IDLE_EVENT_MIN_SEC) * 1000UL;
  idleEventMaxMs = (unsigned long)(doc["idleEventMaxSec"] | DEFAULT_IDLE_EVENT_MAX_SEC) * 1000UL;
  if(idleEventMinMs < 1000UL) idleEventMinMs = 1000UL;
  if(idleEventMinMs > 120000UL) idleEventMinMs = 120000UL;
  if(idleEventMaxMs > 120000UL) idleEventMaxMs = 120000UL;
  if(idleEventMaxMs < idleEventMinMs) idleEventMaxMs = idleEventMinMs;
  if(idleEventMinMs == 5000UL && idleEventMaxMs == 14000UL) {
    idleEventMinMs = (unsigned long)DEFAULT_IDLE_EVENT_MIN_SEC * 1000UL;
    idleEventMaxMs = (unsigned long)DEFAULT_IDLE_EVENT_MAX_SEC * 1000UL;
  }
  if(speedMultiplier < 0.3f) speedMultiplier = 0.3f;
  if(speedMultiplier > 3.0f) speedMultiplier = 3.0f;
  
  // Idle listesi: eski format ["face"] ve yeni format [{name,loops,seconds}] uyumlu
  idleAnimCount = 0;
  if(doc["idleList"].is<JsonArray>()) {
    JsonArray arr = doc["idleList"].as<JsonArray>();
    for(JsonVariant v : arr) {
      if(idleAnimCount >= MAX_IDLE_LIST) break;
      if(v.is<JsonObject>()) {
        idleAnims[idleAnimCount].name = v["name"].as<String>();
        idleAnims[idleAnimCount].loops = v["loops"] | 1;
        idleAnims[idleAnimCount].seconds = v["seconds"] | 0;
      } else {
        idleAnims[idleAnimCount].name = v.as<String>();
        idleAnims[idleAnimCount].loops = 1;
        idleAnims[idleAnimCount].seconds = 0;
      }
      if(idleAnims[idleAnimCount].loops < IDLE_LOOPS_MIN) idleAnims[idleAnimCount].loops = IDLE_LOOPS_MIN;
      if(idleAnims[idleAnimCount].loops > IDLE_LOOPS_MAX) idleAnims[idleAnimCount].loops = IDLE_LOOPS_MAX;
      if(idleAnims[idleAnimCount].seconds < IDLE_SECONDS_MIN) idleAnims[idleAnimCount].seconds = IDLE_SECONDS_MIN;
      if(idleAnims[idleAnimCount].seconds > IDLE_SECONDS_MAX) idleAnims[idleAnimCount].seconds = IDLE_SECONDS_MAX;
      if(idleAnims[idleAnimCount].name.length() > 0) idleAnimCount++;
    }
  }

  // Kategori map'i
  faceCategoryCount = 0;
  if(doc["faceCategories"].is<JsonObject>()) {
    JsonObject cats = doc["faceCategories"].as<JsonObject>();
    for(JsonPair kv : cats) {
      if(faceCategoryCount >= 80) break;
      faceCategoryNames[faceCategoryCount] = kv.key().c_str();
      faceCategoryValues[faceCategoryCount] = kv.value().as<String>();
      faceCategoryCount++;
    }
  }
  
  // 5 reaction/event listesi load pattern'i helper ile sadelestirildi.
  loadStringArrayFromJson(doc, "pokeReactions", pokeReactions, pokeReactionCount, MAX_REACTION_LIST);
  loadStringArrayFromJson(doc, "pettingReactions", pettingReactions, pettingReactionCount, MAX_REACTION_LIST);
  loadStringArrayFromJson(doc, "deepPettingReactions", deepPettingReactions, deepPettingReactionCount, MAX_REACTION_LIST);
  loadStringArrayFromJson(doc, "openingEvents", openingEvents, openingEventCount, MAX_REACTION_LIST);
  loadStringArrayFromJson(doc, "randomEvents", randomEvents, randomEventCount, MAX_REACTION_LIST);

  // Uykuya giris / uyanma event listeleri (config.sleepEnterFace fallback'li, helper sade kullanim icin uygun degil)
  sleepEnterEventCount = 0;
  if(doc["sleepEnterEvents"].is<JsonArray>()) {
    loadStringArrayFromJson(doc, "sleepEnterEvents", sleepEnterEvents, sleepEnterEventCount, MAX_REACTION_LIST);
  } else if(config.sleepEnterFace.length() > 0) {
    sleepEnterEvents[sleepEnterEventCount++] = config.sleepEnterFace;
  }

  sleepWakeEventCount = 0;
  if(doc["sleepWakeEvents"].is<JsonArray>()) {
    loadStringArrayFromJson(doc, "sleepWakeEvents", sleepWakeEvents, sleepWakeEventCount, MAX_REACTION_LIST);
  } else if(config.sleepWakeFace.length() > 0) {
    sleepWakeEvents[sleepWakeEventCount++] = config.sleepWakeFace;
  }

  // ivmeolcer config yukle
  if(doc["accel"].is<JsonObject>()) {
    JsonObject a = doc["accel"].as<JsonObject>();
    accelEnabled              = a["enabled"]               | true;
    accelSoftThreshold        = a["softShakeThreshold"]    | 0.5f;
    accelHardThreshold        = a["hardShakeThreshold"]    | 1.5f;
    accelAccelThreshold       = a["accelerationThreshold"] | 0.8f;
    accelFreefallDurMs        = (unsigned long)(a["freefallDuration"] | 200);
    accelPickupInactivityMs   = (unsigned long)(a["pickupInactivityMs"] | 30000);
    // Clamp
    if(accelSoftThreshold  < 0.1f) accelSoftThreshold  = 0.1f;
    if(accelSoftThreshold  > 3.0f) accelSoftThreshold  = 3.0f;
    if(accelHardThreshold  < 0.5f) accelHardThreshold  = 0.5f;
    if(accelHardThreshold  > 8.0f) accelHardThreshold  = 8.0f;
    if(accelAccelThreshold < 0.1f) accelAccelThreshold = 0.1f;
    if(accelAccelThreshold > 5.0f) accelAccelThreshold = 5.0f;
    if(accelFreefallDurMs  < 50UL)    accelFreefallDurMs  = 50UL;
    if(accelFreefallDurMs  > 1000UL)  accelFreefallDurMs  = 1000UL;
    if(accelPickupInactivityMs < 5000UL)   accelPickupInactivityMs = 5000UL;
    if(accelPickupInactivityMs > 120000UL) accelPickupInactivityMs = 120000UL;
    loadStringArrayFromJson(a, "softShakeReactions",     accelSoftShakeReactions,   accelSoftShakeCount,   MAX_REACTION_LIST);
    loadStringArrayFromJson(a, "hardShakeReactions",     accelHardShakeReactions,   accelHardShakeCount,   MAX_REACTION_LIST);
    loadStringArrayFromJson(a, "accelerationReactions",  accelAccelReactions,       accelAccelCount,       MAX_REACTION_LIST);
    loadStringArrayFromJson(a, "freefallReactions",      accelFreefallReactions,    accelFreefallCount,    MAX_REACTION_LIST);
    loadStringArrayFromJson(a, "orientationReactions",   accelOrientationReactions, accelOrientationCount, MAX_REACTION_LIST);
    loadStringArrayFromJson(a, "pickupReactions",        accelPickupReactions,      accelPickupCount,      MAX_REACTION_LIST);
  }

  sanitizeConfigAgainstFiles(true);
  resetRandomHistories();

  // Basarili load sonrasi mevcut config.json'u .bak olarak yedekle.
  // Bir sonraki saveConfig sirasinda gug kesilir veya parse fail olursa
  // loadConfig bu .bak'tan donmeyi denyeacek.
  if(LittleFS.exists("/config.json")) {
    if(LittleFS.exists("/config.bak")) LittleFS.remove("/config.bak");
    File src = LittleFS.open("/config.json", "r");
    File dst = LittleFS.open("/config.bak", "w");
    if(src && dst) {
      uint8_t buf[256];
      while(true) {
        size_t r = src.read(buf, sizeof(buf));
        if(r == 0) break;
        dst.write(buf, r);
      }
    }
    if(src) src.close();
    if(dst) dst.close();
  }
}

void saveConfig() {
  if(!littlefsReady) {
    Serial.println("Config kaydedilemedi: LittleFS hazir degil");
    return;
  }

  JsonDocument doc;
  // Default yuz ayri bir animasyon ayari degil; Yuzler sekmesindeki aktif yuzdur.
  config.defaultFace = config.currentFace;
  config.sleepEnterFace = sleepEnterEventCount > 0 ? sleepEnterEvents[0] : "";
  config.sleepWakeFace = sleepWakeEventCount > 0 ? sleepWakeEvents[0] : "";
  doc["defaultFace"] = config.currentFace;
  doc["currentFace"] = config.currentFace;
  doc["wifiSSID"] = config.wifiSSID;
  doc["wifiPassword"] = config.wifiPassword;
  doc["wifiAutoConnect"] = config.wifiAutoConnect;
  doc["wifiTimeoutMinutes"] = config.wifiTimeoutMinutes;
  doc["wifiTapCount"] = config.wifiTapCount;
  doc["wifiTapWindowSec"] = config.wifiTapWindowSec;
  doc["batteryTapCount"] = config.batteryTapCount;
  doc["batteryTapWindowSec"] = config.batteryTapWindowSec;
  doc["longPressMs"] = config.longPressMs;
  doc["deepPettingMs"] = config.deepPettingMs;
  doc["touchDebounceMs"] = config.touchDebounceMs;
  doc["displayBrightnessPercent"] = config.displayBrightnessPercent;
  doc["displayBrightnessContrast"] = displayBrightnessPercentToContrast(config.displayBrightnessPercent);
  doc["sleepEnabled"] = config.sleepEnabled;
  doc["sleepTimeoutMinutes"] = config.sleepTimeoutMinutes;
  doc["sleepEnterFace"] = config.sleepEnterFace;
  doc["sleepWakeFace"] = config.sleepWakeFace;
  // sleepEnterMs/sleepWakeMs yazma silindi.
  doc["sleepMode"] = config.sleepMode;
  doc["sleepDeepAfterMinutes"] = config.sleepDeepAfterMinutes;
  doc["speedMultiplier"] = speedMultiplier;
  doc["randomEventMinSec"] = randomEventMinMs / 1000UL;
  doc["randomEventMaxSec"] = randomEventMaxMs / 1000UL;
  doc["idleEventMinSec"] = idleEventMinMs / 1000UL;
  doc["idleEventMaxSec"] = idleEventMaxMs / 1000UL;
  
  JsonArray idleArr = doc["idleList"].to<JsonArray>();
  for(int i = 0; i < idleAnimCount; i++) {
    JsonObject o = idleArr.add<JsonObject>();
    o["name"] = idleAnims[i].name;
    o["loops"] = idleAnims[i].loops <= 0 ? 1 : idleAnims[i].loops;
    o["seconds"] = idleAnims[i].seconds < 0 ? 0 : idleAnims[i].seconds;
  }

  JsonObject cats = doc["faceCategories"].to<JsonObject>();
  for(int i = 0; i < faceCategoryCount; i++) cats[faceCategoryNames[i]] = faceCategoryValues[i];
  
  // 7 array dump pattern'i helper ile sadelestirildi.
  dumpStringArrayToJson(doc, "pokeReactions", pokeReactions, pokeReactionCount);
  dumpStringArrayToJson(doc, "pettingReactions", pettingReactions, pettingReactionCount);
  dumpStringArrayToJson(doc, "deepPettingReactions", deepPettingReactions, deepPettingReactionCount);
  dumpStringArrayToJson(doc, "openingEvents", openingEvents, openingEventCount);
  dumpStringArrayToJson(doc, "randomEvents", randomEvents, randomEventCount);
  dumpStringArrayToJson(doc, "sleepEnterEvents", sleepEnterEvents, sleepEnterEventCount);
  dumpStringArrayToJson(doc, "sleepWakeEvents", sleepWakeEvents, sleepWakeEventCount);

  // ivmeolcer config kaydet
  JsonObject aObj = doc["accel"].to<JsonObject>();
  aObj["enabled"]                = accelEnabled;
  aObj["softShakeThreshold"]     = accelSoftThreshold;
  aObj["hardShakeThreshold"]     = accelHardThreshold;
  aObj["accelerationThreshold"]  = accelAccelThreshold;
  aObj["freefallDuration"]       = (int)accelFreefallDurMs;
  aObj["pickupInactivityMs"]     = (int)accelPickupInactivityMs;
  dumpStringArrayToJson(aObj, "softShakeReactions",    accelSoftShakeReactions,   accelSoftShakeCount);
  dumpStringArrayToJson(aObj, "hardShakeReactions",    accelHardShakeReactions,   accelHardShakeCount);
  dumpStringArrayToJson(aObj, "accelerationReactions", accelAccelReactions,       accelAccelCount);
  dumpStringArrayToJson(aObj, "freefallReactions",     accelFreefallReactions,    accelFreefallCount);
  dumpStringArrayToJson(aObj, "orientationReactions",  accelOrientationReactions, accelOrientationCount);
  dumpStringArrayToJson(aObj, "pickupReactions",       accelPickupReactions,      accelPickupCount);
  
  // Atomic write. Once /config.tmp'a yaz, sonra /config.json olarak rename.
  // Boylece yazma sirasinda gug kesilse bile /config.json son tutarlı haliyle
  // kalir; sadece /config.tmp partial olur ve bir sonraki acilista temizlenir.
  // Ek: saveConfig basarili olmadan once mevcut /config.json'u /config.bak'a
  // kopyalamiyoruz cunku bu daha karmasik IO ister; loadConfig parse fail
  // durumunda /config.bak deniyoruz, ki burayi loadConfig'te periyodik
  // basarili load sonrasi kendisi yedekler.
  const char* CONFIG_TMP = "/config.tmp";
  const char* CONFIG_PATH = "/config.json";
  // Eski tmp varsa temizle
  if(LittleFS.exists(CONFIG_TMP)) LittleFS.remove(CONFIG_TMP);
  File f = LittleFS.open(CONFIG_TMP, "w");
  if(!f) {
    Serial.println("Config tmp acilamadi");
    return;
  }
  size_t written = serializeJson(doc, f);
  f.close();
  if(written == 0) {
    Serial.println("Config serialize basarisiz - tmp siliniyor");
    LittleFS.remove(CONFIG_TMP);
    return;
  }
  // Hedef varsa sil ve rename et
  if(LittleFS.exists(CONFIG_PATH)) LittleFS.remove(CONFIG_PATH);
  if(!LittleFS.rename(CONFIG_TMP, CONFIG_PATH)) {
    Serial.println("Config rename basarisiz - tmp siliniyor");
    LittleFS.remove(CONFIG_TMP);
  }
}

// ============================================================
// ANIMASYON ENGINE
// ============================================================
unsigned long getAnimationTotalDurationMs(Animation& anim) {
  if(!anim.loaded || anim.frame_count == 0) return 1000;
  unsigned long total = 0;
  for(int i = 0; i < anim.frame_count; i++) {
    total += getFrameDelay(anim, i);
  }
  return total;
}

unsigned long getAdjustedAnimationDurationMs(Animation& anim, unsigned long minMs, unsigned long maxMs) {
  unsigned long total = getAnimationTotalDurationMs(anim);
  unsigned long adjusted = (unsigned long)(total / speedMultiplier);
  if(adjusted < minMs) adjusted = minMs;
  if(maxMs > 0 && adjusted > maxMs) adjusted = maxMs;
  return adjusted;
}

// Aktif animasyonu bastan yeniden oynatmak yerine mevcut karenin kalanini ve
// sonraki kareleri tamamlamak icin gereken sureyi hesaplar. PETTING_AFTER
// gecisinde kullanilir; boylece parmak kalkinca animasyon sifirlanmaz.
unsigned long getRemainingAnimationDurationMs(Animation& anim, int frameIdx, unsigned long now) {
  if(!anim.loaded || !anim.frames_data || anim.frame_count == 0) return 100UL;
  if(frameIdx < 0 || frameIdx >= anim.frame_count) frameIdx = 0;

  unsigned long remaining = 0;
  for(int i = frameIdx; i < anim.frame_count; i++) {
    unsigned long frameDelay = (unsigned long)(getFrameDelay(anim, i) / speedMultiplier);
    if(frameDelay < 20UL) frameDelay = 20UL;

    if(i == frameIdx) {
      unsigned long elapsed = now - lastFrameTime;
      if(elapsed < frameDelay) remaining += frameDelay - elapsed;
    } else {
      remaining += frameDelay;
    }
  }

  if(remaining < 80UL) remaining = 80UL;
  if(remaining > 15000UL) remaining = 15000UL;
  return remaining;
}

void scheduleNextRandomEvent() {
  if(randomEventCount <= 0) {
    nextRandomEventAt = 0;
    return;
  }
  if(randomEventMaxMs < randomEventMinMs) randomEventMaxMs = randomEventMinMs;
  unsigned long span = randomEventMaxMs - randomEventMinMs;
  unsigned long extra = (span > 0) ? (unsigned long)random((long)span + 1) : 0;
  nextRandomEventAt = millis() + randomEventMinMs + extra;
}

void scheduleNextIdleEvent() {
  if(idleAnimCount <= 0) {
    nextIdleEventAt = 0;
    return;
  }
  if(idleEventMaxMs < idleEventMinMs) idleEventMaxMs = idleEventMinMs;
  unsigned long span = idleEventMaxMs - idleEventMinMs;
  unsigned long extra = (span > 0) ? (unsigned long)random((long)span + 1) : 0;
  nextIdleEventAt = millis() + idleEventMinMs + extra;
}

bool defaultFaceIsStable(unsigned long now) {
  if(mood != MOOD_IDLE || idleEventPlaying) return false;
  if(!activeAnim || activeAnim != &currentAnim || !currentAnim.loaded) return false;
  if(config.currentFace.length() > 0 && currentAnim.name != config.currentFace) return false;
  return (now - currentIdleStartTime >= AUTO_DEFAULT_DWELL_MS);
}

bool autoCooldownReady(unsigned long now) {
  return lastAutoEventEndedAt == 0 || (now - lastAutoEventEndedAt >= AUTO_EVENT_COOLDOWN_MS);
}

bool randomEventDue(unsigned long now) {
  return randomEventCount > 0 && nextRandomEventAt > 0 && (long)(now - nextRandomEventAt) >= 0;
}

bool randomEventSoon(unsigned long now) {
  if(randomEventCount <= 0 || nextRandomEventAt == 0) return false;
  long remaining = (long)(nextRandomEventAt - now);
  return remaining >= 0 && (unsigned long)remaining <= AUTO_RANDOM_PRIORITY_WINDOW_MS;
}

bool loadDefaultIdleFace(bool scheduleIdleEventNow) {
  unsigned long now = millis();
  bool returningFromIdleEvent = idleEventPlaying;
  bool hardDisplayRecovery = forceHardIdleDisplayRecovery || returningFromIdleEvent;
  forceHardIdleDisplayRecovery = false;
  idleEventPlaying = false;

  String idleName = config.currentFace;
  if(idleName.length() == 0 || !validateFaceFile(idleName)) {
    String fallbackFace = findFirstUsableFaceName();
    if(fallbackFace.length() > 0) {
      idleName = fallbackFace;
      config.currentFace = fallbackFace;
      config.defaultFace = fallbackFace;
      saveConfig();
      Serial.printf("[DEFAULT] auto_select face=%s\n", fallbackFace.c_str());
    } else {
      enterFallbackWaitMode(idleName, "Secili yuz yok/bozuk ve kullanilabilir baska yuz bulunamadi.");
      return false;
    }
  }

  bool currentAnimSane = currentAnim.loaded && currentAnim.frames_data &&
                         currentAnim.frame_count > 0 &&
                         currentAnim.bytes_per_row == 16 && currentAnim.height == 64;

  // WiFi acikken default animasyon zaten RAM'de duruyorsa onu koru.
  // Hard recovery adinda bile olsa burada free+reload yapmak WiFi aktifken tehlikeli:
  // AP sonrasinda max alloc heap blink gibi buyuk yuzleri yeniden yuklemeye yetmeyebilir.
  bool keepCurrentDuringWifi = wifiActive && currentAnimSane && (currentAnim.name == idleName);

  // Tepki/idle-event donusunde WiFi kapaliyken default animasyonu diskten yeniden yukle.
  // WiFi acikken ise sadece OLED/cache temizle, currentAnim'i RAM'de koru.
  if(hardDisplayRecovery && currentAnim.loaded && !keepCurrentDuringWifi) {
    if(activeAnim == &currentAnim) activeAnim = nullptr;
    freeAnimation(currentAnim);
    currentAnimSane = false;
  }
  if(!currentAnimSane || currentAnim.name != idleName) {
    // WiFi acikken snapshot/low-memory moda gecme; default animasyon RAM'de kalir.
    // Bu mod default yuzu tek kareye dusurup donuk goruntu yapiyordu.
    // currentAnim kaybolduysa normal yoldan yuklemeyi dene; ama WiFi acilisinda
    // startWiFiAP zaten currentAnim'i korudugu icin buraya normalde dusmemeli.
    if(!loadAnimation(idleName, currentAnim)) {
      enterFallbackWaitMode(idleName, "Dosya okunamadi, arayuzden tekrar sec/yukle.");
      return false;
    }
    activeFrame = 0;
    lastFrameTime = now;
  }

  activeAnim = &currentAnim;
  // Idle/event animasyonundan ana yuze donuste pointer ayni slotta (&currentAnim)
  // kaldigi icin draw skip cache'i bazen eski kareyi temizlemeyebiliyor.
  // Cache'i sert sifirla; gerekirse OLED buffer'i bir kez temizle.
  lastDrawnAnim = nullptr;
  lastDrawnFrame = -999;
  displayDirty = true;
  if(hardDisplayRecovery) {
    u8g2.setPowerSave(0);
    applyDisplayBrightness();
    u8g2.clearBuffer();
    u8g2.sendBuffer();
    delay(8);
    Serial.println("[DISPLAY] hard idle refresh");
  }
  currentIdleStartTime = now;
  currentIdleDurationMs = 0; // ana yüz süreyle bitmez, kendi içinde döngü yapar
  waitingForFaceSelection = false;
  waitingMissingFaceName = "";
  markDisplayDirty();

  if(scheduleIdleEventNow) scheduleNextIdleEvent();
  else if(idleAnimCount <= 0) nextIdleEventAt = 0;

  Serial.printf("[DEFAULT] active face=%s wifi=%d free=%u max=%u\n", idleName.c_str(), wifiActive ? 1 : 0, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  return true;
}

int pickRandomIdleIndex() {
  if(idleAnimCount <= 0) return -1;

  int valid[20];
  int validCount = 0;
  int preferred[20];
  int preferredCount = 0;
  int fallbackNoImmediate[20];
  int fallbackNoImmediateCount = 0;

  for(int i = 0; i < idleAnimCount; i++) {
    String name = idleAnims[i].name;
    if(name.length() > 0 && name != config.currentFace && faceFileExists(name)) {
      valid[validCount++] = i;
      if(!isNameInHistory(name, recentIdleHistory, recentIdleHistoryCount)) {
        preferred[preferredCount++] = i;
      }
      // Eger son 4'u tamamen dislamak mumkun degilse bile bir oncekini tekrar etmeyelim.
      if(recentIdleHistoryCount <= 0 || name != recentIdleHistory[0]) {
        fallbackNoImmediate[fallbackNoImmediateCount++] = i;
      }
    }
  }

  if(validCount <= 0) return -1;

  int selectedIdx;
  if(preferredCount > 0) {
    selectedIdx = preferred[random(preferredCount)];
  } else if(fallbackNoImmediateCount > 0) {
    selectedIdx = fallbackNoImmediate[random(fallbackNoImmediateCount)];
  } else {
    selectedIdx = valid[random(validCount)];
  }

  pushRandomHistory(recentIdleHistory, recentIdleHistoryCount, idleAnims[selectedIdx].name);
  return selectedIdx;
}

bool startRandomIdleEvent() {
  if(wifiActive) return false;
  unsigned long now = millis();
  int selectedIdx = pickRandomIdleIndex();
  if(selectedIdx < 0) {
    scheduleNextIdleEvent();
    return false;
  }

  String nextName = idleAnims[selectedIdx].name;
  if(!playEventFaceSafe(nextName, MOOD_IDLE, EVENT_IDLE, IDLE_EVENT_OPTIONS)) {
    Serial.printf("[IDLE] skip face=%s reason=event_load_failed\n", nextName.c_str());
    scheduleNextIdleEvent();
    return false;
  }

  lastIdleEventIndex = selectedIdx;
  idleEventPlaying = true;

  unsigned long animTotalMs = getAnimationTotalDurationMs(eventAnim);
  unsigned long adjustedTotalMs = (unsigned long)(animTotalMs / speedMultiplier);
  if(adjustedTotalMs < 500UL) adjustedTotalMs = 500UL;
  int loops = idleAnims[selectedIdx].loops;
  if(loops < 1) loops = 1;
  if(loops > 5) loops = 5;
  int fixedSec = idleAnims[selectedIdx].seconds;
  if(fixedSec < 0) fixedSec = 0;
  if(fixedSec > 60) fixedSec = 60;
  currentIdleDurationMs = fixedSec > 0 ? (unsigned long)fixedSec * 1000UL : adjustedTotalMs * (unsigned long)loops;
  if(currentIdleDurationMs < 500UL) currentIdleDurationMs = 500UL;
  currentIdleStartTime = now;
  Serial.printf("[IDLE] start face=%s duration=%lu loops=%d fixedSec=%d\n",
                nextName.c_str(), currentIdleDurationMs, loops, fixedSec);
  return true;
}
void selectIdleNext() {
  // Eski sirali idle mantigi yerine ana yuz + rastgele ara idle mantigi.
  loadDefaultIdleFace(true);
}

bool isNameInHistory(const String& name, String* history, int historyCount) {
  if(name.length() == 0) return false;
  int limit = historyCount;
  if(limit > RANDOM_HISTORY_SIZE) limit = RANDOM_HISTORY_SIZE;
  for(int i = 0; i < limit; i++) {
    if(history[i] == name) return true;
  }
  return false;
}

void pushRandomHistory(String* history, int& historyCount, const String& name) {
  if(name.length() == 0) return;

  // Ayni ismi gecmiste tekrar tekrar tutma; once varsa cikar.
  int writeIdx = 0;
  int oldCount = historyCount;
  if(oldCount > RANDOM_HISTORY_SIZE) oldCount = RANDOM_HISTORY_SIZE;
  for(int i = 0; i < oldCount; i++) {
    if(history[i] != name && writeIdx < RANDOM_HISTORY_SIZE) {
      history[writeIdx++] = history[i];
    }
  }

  for(int i = RANDOM_HISTORY_SIZE - 1; i > 0; i--) {
    history[i] = (i - 1 < writeIdx) ? history[i - 1] : "";
  }
  history[0] = name;
  historyCount = writeIdx + 1;
  if(historyCount > RANDOM_HISTORY_SIZE) historyCount = RANDOM_HISTORY_SIZE;
}

void resetRandomHistories() {
  for(int i = 0; i < RANDOM_HISTORY_SIZE; i++) {
    recentIdleHistory[i] = "";
    recentPokeHistory[i] = "";
    recentPettingHistory[i] = "";
    recentDeepHistory[i] = "";
    recentRandomEventHistory[i] = "";
    recentSleepEnterHistory[i] = "";
    recentSleepWakeHistory[i] = "";
    // 
    recentAccelSoftHistory[i]     = "";
    recentAccelHardHistory[i]     = "";
    recentAccelAccelHistory[i]    = "";
    recentAccelFreefallHistory[i] = "";
    recentAccelOrientHistory[i]   = "";
    recentAccelPickupHistory[i]   = "";
  }
  recentIdleHistoryCount = 0;
  recentPokeHistoryCount = 0;
  recentPettingHistoryCount = 0;
  recentDeepHistoryCount = 0;
  recentRandomEventHistoryCount = 0;
  recentSleepEnterHistoryCount = 0;
  recentSleepWakeHistoryCount = 0;
  // 
  recentAccelSoftHistoryCount     = 0;
  recentAccelHardHistoryCount     = 0;
  recentAccelAccelHistoryCount    = 0;
  recentAccelFreefallHistoryCount = 0;
  recentAccelOrientHistoryCount   = 0;
  recentAccelPickupHistoryCount   = 0;
}

String getRandomReaction(String* list, int count) {
  if(count == 0) return "";

  // Eksik/silinmis yuzlere denk gelirse sessizce atla.
  int valid[50];
  int validCount = 0;
  for(int i = 0; i < count && i < 50; i++) {
    String name = list[i];
    if(name.length() > 0 && faceFileExists(name)) {
      valid[validCount++] = i;
    }
  }
  if(validCount <= 0) return "";
  return list[valid[random(validCount)]];
}


int getFaceFrameCountQuick(const String& name) {
  if(!littlefsReady || name.length() == 0) return -1;
  String path = "/faces/" + name + ".bin";
  File f = LittleFS.open(path, "r");
  if(!f) return -1;
  uint8_t header[BIN_HEADER_SIZE];
  int n = f.read(header, BIN_HEADER_SIZE);
  f.close();
  if(n != BIN_HEADER_SIZE) return -1;
  if(memcmp(header, BIN_MAGIC, 4) != 0 && memcmp(header, BIN_MAGIC_V3, 4) != 0) return -1;
  if(header[5] != 16 || header[6] != 64) return -1;
  return (int)header[4];
}

bool wifiSafeFaceCandidate(const String& name, int maxFrames) {
  if(name.length() == 0 || !faceFileExists(name)) return false;
  int frames = getFaceFrameCountQuick(name);
  if(frames <= 0) return false;
  if(frames > maxFrames) return false;
  // 128x64 XBM: her frame 1024 byte + delay. Default blink zaten RAM'de.
  // WiFi acikken eventAnim icin tek parca malloc guvenlik payi birak.
  size_t estimated = (size_t)frames * (size_t)(1 + 16 * 64);
  size_t maxBlock = ESP.getMaxAllocHeap();
  size_t freeHeap = ESP.getFreeHeap();
  return maxBlock > estimated + 12000 && freeHeap > estimated + 22000;
}

String getRandomWifiSafeReactionWithHistory(String* list, int count, String* history, int& historyCount, int maxFrames) {
  if(count == 0) return "";
  int valid[50];
  int validCount = 0;
  int preferred[50];
  int preferredCount = 0;
  int fallbackNoImmediate[50];
  int fallbackNoImmediateCount = 0;

  for(int i = 0; i < count && i < 50; i++) {
    String name = list[i];
    if(wifiSafeFaceCandidate(name, maxFrames)) {
      valid[validCount++] = i;
      if(!isNameInHistory(name, history, historyCount)) preferred[preferredCount++] = i;
      if(historyCount <= 0 || name != history[0]) fallbackNoImmediate[fallbackNoImmediateCount++] = i;
    }
  }
  if(validCount <= 0) return "";

  int selectedIdx;
  if(preferredCount > 0) selectedIdx = preferred[random(preferredCount)];
  else if(fallbackNoImmediateCount > 0) selectedIdx = fallbackNoImmediate[random(fallbackNoImmediateCount)];
  else selectedIdx = valid[random(validCount)];

  String selectedName = list[selectedIdx];
  pushRandomHistory(history, historyCount, selectedName);
  return selectedName;
}


const char* eventSourceName(int source) {
  switch(source) {
    case EVENT_TOUCH: return "TOUCH";
    case EVENT_ACCEL: return "ACCEL";
    case EVENT_PETTING: return "PETTING";
    case EVENT_DEEP_PETTING: return "DEEP_PETTING";
    case EVENT_IDLE: return "IDLE";
    case EVENT_RANDOM: return "RANDOM";
    case EVENT_TEST: return "TEST";
    case EVENT_OPENING: return "OPENING";
    case EVENT_WEB: return "WEB";
    default: return "UNKNOWN";
  }
}

bool playEventFaceSafe(const String& name, MoodState targetMood, EventSource source, const EventPlayOptions& options) {
  if(name.length() == 0) {
    if(options.logResult) {
      Serial.printf("[EVENT] skip src=%s reason=no_face wifi=%d free=%u max=%u\n",
                    eventSourceName(source), wifiActive ? 1 : 0, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    }
    return false;
  }

  if(!faceFileExists(name)) {
    if(options.logResult) {
      Serial.printf("[EVENT] skip src=%s face=%s reason=missing wifi=%d free=%u max=%u\n",
                    eventSourceName(source), name.c_str(), wifiActive ? 1 : 0, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    }
    return false;
  }

  int frames = getFaceFrameCountQuick(name);
  if(wifiActive) {
    if(!options.allowDuringWifi) {
      if(options.logResult) {
        Serial.printf("[EVENT] skip src=%s face=%s reason=wifi_blocked frames=%d free=%u max=%u\n",
                      eventSourceName(source), name.c_str(), frames, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      }
      return false;
    }
    if(!wifiSafeFaceCandidate(name, options.maxFramesDuringWifi)) {
      if(options.logResult) {
        Serial.printf("[EVENT] skip src=%s face=%s reason=too_big_wifi frames=%d limit=%u free=%u max=%u\n",
                      eventSourceName(source), name.c_str(), frames, options.maxFramesDuringWifi,
                      ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      }
      return false;
    }
  }

  if(source != EVENT_IDLE) {
    idleEventPlaying = false;
  }

  if(options.logResult) {
    Serial.printf("[EVENT] start src=%s face=%s mood=%d wifi=%d frames=%d free=%u max=%u\n",
                  eventSourceName(source), name.c_str(), (int)targetMood, wifiActive ? 1 : 0,
                  frames, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }

  if(!loadAnimation(name, eventAnim)) {
    if(options.logResult) {
      Serial.printf("[EVENT] fail src=%s face=%s reason=load_failed wifi=%d free=%u max=%u\n",
                    eventSourceName(source), name.c_str(), wifiActive ? 1 : 0, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    }
    return false;
  }

  activeAnim = &eventAnim;
  if(options.resetFrame) {
    activeFrame = 0;
    lastFrameTime = millis();
  }
  markDisplayDirty();

  if(options.logResult) {
    Serial.printf("[EVENT] ok src=%s face=%s mood=%d duration=%lu wifi=%d free=%u max=%u\n",
                  eventSourceName(source), name.c_str(), (int)targetMood,
                  getAdjustedAnimationDurationMs(eventAnim, 800UL, 15000UL), wifiActive ? 1 : 0,
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }
  return true;
}

EventSource eventSourceFromMood(MoodState state) {
  switch(state) {
    case MOOD_POKED: return EVENT_TOUCH;
    case MOOD_ACCEL_EVENT: return EVENT_ACCEL;
    case MOOD_PETTING: return EVENT_PETTING;
    case MOOD_DEEP_PETTING:
    case MOOD_PETTING_AFTER: return EVENT_DEEP_PETTING;
    case MOOD_RANDOM_EVENT: return EVENT_RANDOM;
    case MOOD_TEST_EVENT: return EVENT_TEST;
    default: return EVENT_UNKNOWN;
  }
}

bool returnToDefaultFaceSafe(int source, bool scheduleIdleEventNow, bool scheduleRandomEventNow) {
  unsigned long now = millis();
  bool hardRefresh = forceHardIdleDisplayRecovery;
  forceHardIdleDisplayRecovery = false;

  idleEventPlaying = false;
  currentIdleStartTime = now;
  currentIdleDurationMs = 0;
  deepPettingPending = false;

  // Gecici event slotunu bosalt ama default/currentAnim slotuna dokunma.
  // Refactor'un ana kurali: default yuz RAM'deyse reload/free yapilmaz.
  freeTransientAnimations();

  mood = MOOD_IDLE;
  moodStartTime = now;

  bool currentDefaultReady = currentAnim.loaded && currentAnim.frames_data &&
                             currentAnim.frame_count > 0 &&
                             currentAnim.bytes_per_row == 16 && currentAnim.height == 64 &&
                             currentAnim.name == config.currentFace;

  if(currentDefaultReady) {
    activeAnim = &currentAnim;
    if(activeFrame < 0 || activeFrame >= currentAnim.frame_count) activeFrame = 0;
    lastFrameTime = now;

    lastDrawnAnim = nullptr;
    lastDrawnFrame = -999;
    displayDirty = true;

    if(hardRefresh) {
      u8g2.setPowerSave(0);
      applyDisplayBrightness();
      u8g2.clearBuffer();
      u8g2.sendBuffer();
      delay(6);
      Serial.printf("[RETURN] hard refresh src=%s\n", eventSourceName(source));
    }

    waitingForFaceSelection = false;
    waitingMissingFaceName = "";
    markDisplayDirty();

    if(scheduleIdleEventNow) scheduleNextIdleEvent();
    else if(idleAnimCount <= 0) nextIdleEventAt = 0;
    if(scheduleRandomEventNow) scheduleNextRandomEvent();

    Serial.printf("[RETURN] ok src=%s via=ram face=%s wifi=%d free=%u max=%u\n",
                  eventSourceName(source), currentAnim.name.c_str(), wifiActive ? 1 : 0,
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return true;
  }

  // Default RAM'de degilse eski guvenli yukleme yoluna dus. WiFi acikken bu normalde
  // olmamali; yine de fallback icin loadDefaultIdleFace kullanilir.
  forceHardIdleDisplayRecovery = hardRefresh;
  bool ok = loadDefaultIdleFace(scheduleIdleEventNow);
  if(ok && scheduleRandomEventNow) scheduleNextRandomEvent();
  Serial.printf("[RETURN] %s src=%s via=reload wifi=%d free=%u max=%u\n",
                ok ? "ok" : "fail", eventSourceName(source), wifiActive ? 1 : 0,
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  return ok;
}

String getRandomReactionWithHistory(String* list, int count, String* history, int& historyCount) {
  if(count == 0) return "";

  int valid[50];
  int validCount = 0;
  int preferred[50];
  int preferredCount = 0;
  int fallbackNoImmediate[50];
  int fallbackNoImmediateCount = 0;

  for(int i = 0; i < count && i < 50; i++) {
    String name = list[i];
    if(name.length() > 0 && faceFileExists(name)) {
      valid[validCount++] = i;
      if(!isNameInHistory(name, history, historyCount)) {
        preferred[preferredCount++] = i;
      }
      if(historyCount <= 0 || name != history[0]) {
        fallbackNoImmediate[fallbackNoImmediateCount++] = i;
      }
    }
  }

  if(validCount <= 0) return "";

  int selectedIdx;
  if(preferredCount > 0) {
    selectedIdx = preferred[random(preferredCount)];
  } else if(fallbackNoImmediateCount > 0) {
    selectedIdx = fallbackNoImmediate[random(fallbackNoImmediateCount)];
  } else {
    selectedIdx = valid[random(validCount)];
  }

  String selectedName = list[selectedIdx];
  pushRandomHistory(history, historyCount, selectedName);
  return selectedName;
}

void enterMood(MoodState newMood) {
  MoodState previousMood = mood;
  unsigned long now = millis();

  // WiFi acikken default animasyon RAM'de korunur.
  // Fiziksel tepkiler (tap, sevilme, ivmeolcer) guvenli/kucuk animasyonlarla calisir.
  // Otomatik random/idle eventler WiFi acikken baslatilmaz; web testleri kendi endpoint'inden gider.
  if(wifiActive &&
     newMood != MOOD_IDLE &&
     newMood != MOOD_POKED &&
     newMood != MOOD_ACCEL_EVENT &&
     newMood != MOOD_PETTING &&
     newMood != MOOD_DEEP_PETTING &&
     newMood != MOOD_PETTING_AFTER) {
    return;
  }
  if(newMood == MOOD_IDLE &&
     (previousMood == MOOD_ACCEL_EVENT || previousMood == MOOD_RANDOM_EVENT ||
      previousMood == MOOD_TEST_EVENT || previousMood == MOOD_POKED ||
      previousMood == MOOD_PETTING || previousMood == MOOD_DEEP_PETTING ||
      previousMood == MOOD_PETTING_AFTER)) {
    forceHardIdleDisplayRecovery = true;
  }
  bool preservePettingFrame = (newMood == MOOD_PETTING_AFTER) &&
                              (previousMood == MOOD_PETTING || previousMood == MOOD_DEEP_PETTING) &&
                              activeAnim && activeAnim->loaded;

  mood = newMood;
  moodStartTime = now;
  if(!preservePettingFrame) {
    activeFrame = 0;
    lastFrameTime = now;
  }
  if(newMood != MOOD_PETTING) deepPettingPending = false;
  markDisplayDirty();

  // Gecici animasyon slotu artik playEventFaceSafe() icinde tek merkezden temizlenir.
  
  switch(newMood) {
    case MOOD_POKED: {
      String name = wifiActive
        ? getRandomWifiSafeReactionWithHistory(pokeReactions, pokeReactionCount, recentPokeHistory, recentPokeHistoryCount, TOUCH_EVENT_OPTIONS.maxFramesDuringWifi)
        : getRandomReactionWithHistory(pokeReactions, pokeReactionCount, recentPokeHistory, recentPokeHistoryCount);
      if(playEventFaceSafe(name, MOOD_POKED, EVENT_TOUCH, TOUCH_EVENT_OPTIONS)) {
        pokeReactionDurationMs = getAdjustedAnimationDurationMs(eventAnim, 800UL, 15000UL);
      } else {
        enterMood(MOOD_IDLE);
        return;
      }
      break;
    }
    case MOOD_PETTING: {
      String name = wifiActive
        ? getRandomWifiSafeReactionWithHistory(pettingReactions, pettingReactionCount, recentPettingHistory, recentPettingHistoryCount, PETTING_EVENT_OPTIONS.maxFramesDuringWifi)
        : getRandomReactionWithHistory(pettingReactions, pettingReactionCount, recentPettingHistory, recentPettingHistoryCount);
      if(!playEventFaceSafe(name, MOOD_PETTING, EVENT_PETTING, PETTING_EVENT_OPTIONS)) {
        enterMood(MOOD_IDLE);
        return;
      }
      break;
    }
    case MOOD_DEEP_PETTING: {
      String name = wifiActive
        ? getRandomWifiSafeReactionWithHistory(deepPettingReactions, deepPettingReactionCount, recentDeepHistory, recentDeepHistoryCount, DEEP_PETTING_EVENT_OPTIONS.maxFramesDuringWifi)
        : getRandomReactionWithHistory(deepPettingReactions, deepPettingReactionCount, recentDeepHistory, recentDeepHistoryCount);
      if(!playEventFaceSafe(name, MOOD_DEEP_PETTING, EVENT_DEEP_PETTING, DEEP_PETTING_EVENT_OPTIONS)) {
        enterMood(MOOD_IDLE);
        return;
      }
      break;
    }
    case MOOD_PETTING_AFTER:
      // Parmak kalkinca aktif kareyi sifirlama. Sadece o anki dongunun kalan
      // kismini tamamla; eski kod activeFrame=0 yapip animasyonu bastan bir kez
      // daha oynattigi icin 'yarim + tam tekrar' goruntusu olusuyordu.
      if(activeAnim && activeAnim->loaded) {
        pettingAfterDurationMs = getRemainingAnimationDurationMs(*activeAnim, activeFrame, now);
      } else {
        pettingAfterDurationMs = 100UL;
      }
      break;
    case MOOD_RANDOM_EVENT: {
      String name = getRandomReactionWithHistory(randomEvents, randomEventCount, recentRandomEventHistory, recentRandomEventHistoryCount);
      if(playEventFaceSafe(name, MOOD_RANDOM_EVENT, EVENT_RANDOM, RANDOM_EVENT_OPTIONS)) {
        unsigned long total = getAnimationTotalDurationMs(eventAnim);
        unsigned long adjusted = (unsigned long)(total / speedMultiplier);
        randomEventDurationMs = max((unsigned long)800, adjusted);
        randomEventStartTime = millis();
        Serial.printf("[RANDOM] start face=%s duration=%lu\n", name.c_str(), randomEventDurationMs);
      } else {
        enterMood(MOOD_IDLE);
        return;
      }
      break;
    }
    case MOOD_TEST_EVENT:
      break;
    case MOOD_ACCEL_EVENT: {
      // Animasyon merkezi loader tarafindan eventAnim'a yuklendi; burada sure kaydi yapilir.
      unsigned long total = getAnimationTotalDurationMs(eventAnim);
      unsigned long adjusted = (unsigned long)(total / speedMultiplier);
      accelEventDurationMs = max((unsigned long)800UL, adjusted);
      accelEventStartTime  = now;
      break;
    }
    case MOOD_IDLE: {
      returnToDefaultFaceSafe(eventSourceFromMood(previousMood), true, true);
      break;
    }
  }
}


bool startTestAnimation(const String& kind) {
  String name = "";

  // UI test butonlari hem kategori adiyla hem de dogrudan secili animasyon adiyla gelebilir.
  // Dogrudan ad geldiyse kaydedilmemis checkbox secimleri de test edilebilir.
  String k = kind;
  k.trim();

  if(k == "opening" || k == "opening-event") name = getRandomReaction(openingEvents, openingEventCount);
  else if(k == "random" || k == "random-event") name = getRandomReaction(randomEvents, randomEventCount);
  else if(k == "poke" || k == "durtme") name = getRandomReaction(pokeReactions, pokeReactionCount);
  else if(k == "petting" || k == "sevilme") name = getRandomReaction(pettingReactions, pettingReactionCount);
  else if(k == "deep" || k == "deep-petting" || k == "deepPetting" || k == "cok-sevilme") name = getRandomReaction(deepPettingReactions, deepPettingReactionCount);
  else if(k == "sleep-enter" || k == "sleepEnter") name = getRandomReaction(sleepEnterEvents, sleepEnterEventCount);
  else if(k == "sleep-wake" || k == "sleepWake") name = getRandomReaction(sleepWakeEvents, sleepWakeEventCount);
  else if(k == "active") name = config.currentFace;
  else if(faceFileExists(k)) name = k;

  if(name.length() == 0 || !faceFileExists(name)) {
    Serial.printf("[TEST] skip kind=%s reason=no_face_or_missing\n", k.c_str());
    return false;
  }

  // Test animasyonu da merkezi guvenli event loader'dan gecsin.
  // WiFi acikken cok buyuk animasyonlar ekrani bozmasin diye limit uygulanir.
  if(!playEventFaceSafe(name, MOOD_TEST_EVENT, EVENT_TEST, TEST_EVENT_OPTIONS)) {
    Serial.printf("[TEST] fail kind=%s face=%s\n", k.c_str(), name.c_str());
    return false;
  }

  mood = MOOD_TEST_EVENT;
  moodStartTime = millis();
  activeFrame = 0;
  lastFrameTime = millis();
  testEventStartTime = millis();
  unsigned long total = getAnimationTotalDurationMs(eventAnim);
  unsigned long adjusted = (unsigned long)(total / speedMultiplier);
  testEventDurationMs = max((unsigned long)800, min((unsigned long)8000, adjusted));
  Serial.printf("[TEST] ok kind=%s face=%s duration=%lu wifi=%d\n", k.c_str(), name.c_str(), testEventDurationMs, wifiActive ? 1 : 0);
  return true;
}

void updateAnimation() {
  unsigned long now = millis();
  
  // Mood gecisleri
  if(mood == MOOD_POKED && now - moodStartTime >= pokeReactionDurationMs) {
    enterMood(MOOD_IDLE);
    return;
  }
  
  if(mood == MOOD_PETTING_AFTER && now - moodStartTime >= pettingAfterDurationMs) {
    enterMood(MOOD_IDLE);
    return;
  }

  if(mood == MOOD_RANDOM_EVENT && now - randomEventStartTime >= randomEventDurationMs) {
    lastAutoEventEndedAt = now;
    enterMood(MOOD_IDLE);
    return;
  }

  if(mood == MOOD_TEST_EVENT && now - testEventStartTime >= testEventDurationMs) {
    enterMood(MOOD_IDLE);
    return;
  }

  // Ivmeolcer event bitti
  if(mood == MOOD_ACCEL_EVENT && now - accelEventStartTime >= accelEventDurationMs) {
    enterMood(MOOD_IDLE);
    return;
  }
  
  // Petting -> Deep petting gecisi. Esik dokunmanin baslangicindan olculur.
  // Eski davranis dongu/frame-0 sinirini bekliyordu; uzun petting yuzunu bazen hic
  // gostermedigi icin artik esik dolunca hemen deep petting'e geciyoruz.
  if(mood == MOOD_PETTING && !deepPettingPending &&
     now - touchStartTime >= (unsigned long)config.deepPettingMs &&
     deepPettingReactionCount > 0) {
    Serial.printf("[PETTING] deep threshold hold=%lu/%d wifi=%d deepCount=%d\n",
                  (unsigned long)(now - touchStartTime), config.deepPettingMs, wifiActive ? 1 : 0, deepPettingReactionCount);
    enterMood(MOOD_DEEP_PETTING);
    return;
  }
  
  // Idle event veya tepki bittikten sonra ekranda ara animasyonun son frame'i kalırsa
  // bir sonraki dokunmaya kadar "dondu" gibi görünür. Idle modda otomatik event
  // oynamıyorken aktif slot default yüze işaret etmiyorsa hemen toparla.
  if(mood == MOOD_IDLE && !idleEventPlaying && !waitingForFaceSelection) {
    if(!activeAnim || activeAnim != &currentAnim || !currentAnim.loaded ||
       !currentAnim.frames_data || currentAnim.frame_count == 0 ||
       currentAnim.bytes_per_row != 16 || currentAnim.height != 64 ||
       (config.currentFace.length() > 0 && currentAnim.name != config.currentFace)) {
      returnToDefaultFaceSafe(EVENT_UNKNOWN, false, false);
      return;
    }
  }

  // WiFi acikken otomatik random/idle event baslatma. Default yuz aksin;
  // web arayuzu acikken buyuk yuz dosyalariyla heap zorlanmasin.

  // Otomatik eventler kuyruklanmaz. Yalnızca default yüz bir süre stabil kaldıktan
  // ve son otomatik davranıştan sonra yeterli boşluk oluştuktan sonra başlar.
  if(!wifiActive && defaultFaceIsStable(now) && autoCooldownReady(now) && randomEventDue(now)) {
    enterMood(MOOD_RANDOM_EVENT);
    return;
  }
  
  if(!wifiActive && defaultFaceIsStable(now) && autoCooldownReady(now) &&
     idleAnimCount > 0 && nextIdleEventAt > 0 && (long)(now - nextIdleEventAt) >= 0 &&
     !randomEventDue(now) && !randomEventSoon(now)) {
    startRandomIdleEvent();
    return;
  }

  // Ara idle bittiyse ana/default yuze don. Sonraki otomatik davranis hemen
  // arkasindan baslamasin diye cooldown burada baslar.
  if(mood == MOOD_IDLE && idleEventPlaying && currentIdleDurationMs > 0 && now - currentIdleStartTime >= currentIdleDurationMs) {
    lastAutoEventEndedAt = now;
    forceHardIdleDisplayRecovery = true;
    returnToDefaultFaceSafe(EVENT_IDLE, true, true);
    return;
  }
  
  // Frame ilerleme
  if(activeAnim && activeAnim->loaded) {
    uint16_t delay_ms = getFrameDelay(*activeAnim, activeFrame);
    delay_ms = (uint16_t)(delay_ms / speedMultiplier);
    if(delay_ms < 20) delay_ms = 20;
    
    if(now - lastFrameTime >= delay_ms) {
      int previousFrame = activeFrame;
      activeFrame = (activeFrame + 1) % activeAnim->frame_count;
      lastFrameTime = now;

      if(mood == MOOD_PETTING && deepPettingPending &&
         previousFrame != activeFrame && activeFrame == 0) {
        enterMood(MOOD_DEEP_PETTING);
        return;
      }
    } else if(activeAnim->frame_count > 1 && now - lastFrameTime > 5000UL) {
      // Beklenmeyen zamanlama takilmasinda animasyonu tek frame'de birakma.
      activeFrame = (activeFrame + 1) % activeAnim->frame_count;
      lastFrameTime = now;
      markDisplayDirty();
    }
  }
}


void serviceDisplayDuringBlockingWork() {
  unsigned long now = millis();
  if(now - lastBlockingDisplayServiceAt < 15UL) {
    delay(0);
    return;
  }
  lastBlockingDisplayServiceAt = now;

  if(activeAnim && activeAnim->loaded) {
    uint16_t delay_ms = getFrameDelay(*activeAnim, activeFrame);
    delay_ms = (uint16_t)(delay_ms / speedMultiplier);
    if(delay_ms < 20) delay_ms = 20;
    if(now - lastFrameTime >= delay_ms) {
      activeFrame = (activeFrame + 1) % activeAnim->frame_count;
      lastFrameTime = now;
      markDisplayDirty();
    }
  }

  if(!showingBatteryNotification && !showingWifiNotification) {
    drawCurrentFrame();
  }
  delay(0);
}


void drawWifiStatusIcon() {
  if(!wifiActive) return;
  // Kucuk, sade WiFi simgesi: sag ust kose.
  int cx = 118;
  int cy = 10;
  u8g2.drawCircle(cx, cy, 7, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  u8g2.drawCircle(cx, cy, 4, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  u8g2.drawDisc(cx, cy - 1, 1);
}

void drawCurrentFrame() {
  // Skip redundant flush. updateAnimation frame'i ilerletmediginde
  // ve mood/icon state degismediginde clearBuffer+drawXBM+sendBuffer maliyetini
  // odemiyoruz. SH1106 SPI sendBuffer ~6-8ms; her loop tick'i odenince
  // CPU/SPI bos yere mesgul. Notification rendering'den donulurken
  // markDisplayDirty() ile zorla redraw tetikleniyor.
  if(!activeAnim || !activeAnim->loaded) {
    // Fallback yuze gec - sadece state degistiyse ciz.
    if(lastDrawnAnim != nullptr || displayDirty) {
      drawFallbackFace();
      lastDrawnAnim = nullptr;
      lastDrawnFrame = -1;
      displayDirty = false;
    }
    return;
  }
  
  bool needRedraw = displayDirty ||
                    lastDrawnAnim != activeAnim ||
                    lastDrawnFrame != activeFrame ||
                    lastDrawnCharging != batteryCharging ||
                    lastDrawnWifi != wifiActive;
  if(!needRedraw) return;

  uint8_t* frameData = getFrameData(*activeAnim, activeFrame);
  if(!frameData) {
    drawFallbackFace();
    lastDrawnAnim = nullptr;
    lastDrawnFrame = -1;
    displayDirty = false;
    return;
  }
  
  u8g2.clearBuffer();
  u8g2.drawXBM(0, 0, 128, 64, frameData);
  drawChargingBatteryOverlay();
  if(showingBatteryNotification) drawBatteryNotification();
  drawWifiStatusIcon();
  u8g2.sendBuffer();

  lastDrawnAnim = activeAnim;
  lastDrawnFrame = activeFrame;
  lastDrawnCharging = batteryCharging;
  lastDrawnWifi = wifiActive;
  displayDirty = false;
}

// ============================================================
// DOKUNMA YONETIMI
// ============================================================
int countRecentTaps(unsigned long now, unsigned long windowMs) {
  int c = 0;
  for(int i = 0; i < tapCount; i++) {
    if(now - tapTimes[i] < windowMs) c++;
  }
  return c;
}

// ─── DOKUNMA OKUMA (ADC fallback) ─────────────────────────────
// Donanim durumu: TTP223 OUT sinyali GPIO1'e ulasiyor (dokununca ~2.9V,
// multimetre ile dogrulandi) ama bu kartta GPIO1'in dijital giris kati
// 2.9V'u HIGH okuyamiyor (3.3V jumper ile okuyor — giris esigi hasarli).
// ADC katmani saglam: bosta ~200-400, dokununca ~3800-4095.
// Bu yuzden dokunma ADC esigiyle okunur:
//  - 15ms cache: loop/blocking animasyon donguleri kac kez cagirirsa cagirsin
//    15ms'de en fazla 1 analogRead — frame timing bozulmaz.
//  - Histerezis: >2200 basildi, <1200 birakildi — ADC gurultusu esik
//    civarinda sahte gecis uretemez.
// NOT: Bu fonksiyonun tanimi bilerek dosyanin DERININDE (tum tip ve global
// tanimlarindan sonra). Dosya tepesine fonksiyon tanimi koymak Arduino'nun
// auto-prototype ekleme noktasini kaydirip yapisal sorunlar cikarabiliyor.
bool readTouchActive() {
  static unsigned long lastReadMs = 0;
  static bool lastState = false;
  unsigned long now = millis();
  if(now - lastReadMs >= 15) {
    lastReadMs = now;
    touchAdcLast = analogRead(PIN_TOUCH);
    if(lastState) {
      if(touchAdcLast < 1200) lastState = false;
    } else {
      if(touchAdcLast > 2200) lastState = true;
    }
  }
  return lastState;
}

void handleTouch() {
  unsigned long now = millis();
  static bool rawPrev = false;
  static bool stableTouch = false;
  static unsigned long rawChangedAt = 0;
  static bool pendingBatteryGesture = false;
  static unsigned long pendingBatteryTriggerAt = 0;
  static unsigned long lastBatteryTapNotifyAt = 0;

  bool rawTouch = readTouchActive();
  if(rawTouch != rawPrev) { rawPrev = rawTouch; rawChangedAt = now; touchRawTransitions++; }
  bool touch = stableTouch;
  if(now - rawChangedAt >= (unsigned long)config.touchDebounceMs) {
    stableTouch = rawTouch;
    touch = stableTouch;
  }

  // Uyku modunda sadece dokunmayi dinle. Event/idle/random/bildirim hicbir sey uykuyu bozmaz.
  if(systemSleeping) {
    if(touch && !prevTouch) {
      wakeFromSleepMode();
    }
    prevTouch = touch;
    return;
  }

  // Uykudan uyandiran ilk dokunus ayrica poke/petting/tap sayilmasin.
  // Kullanici parmagini kaldirdiktan sonraki dokunuslar normal davranir.
  if(ignoreTouchUntilRelease) {
    if(!touch) {
      ignoreTouchUntilRelease = false;
      prevTouch = false;
      touchStartTime = now;
      Serial.println("[TOUCH] ignore temizlendi (birakma algilandi)");
    } else {
      prevTouch = true;
    }
    return;
  }

  unsigned long wifiWindowMs = (unsigned long)config.wifiTapWindowSec * 1000UL;
  unsigned long batteryWindowMs = (unsigned long)config.batteryTapWindowSec * 1000UL;
  unsigned long keepWindowMs = wifiWindowMs > batteryWindowMs ? wifiWindowMs : batteryWindowMs;
  
  // Dokunma basladi
  if(touch && !prevTouch) {
    touchStartTime = now;
    lastInteractionTime = now;
    
    // Hizli tap detector: WiFi ve pil ayni dokunma gecmisini okuyabilir,
    // ama pil bildirimi WiFi gesture'inin onune gecmesin diye kisa bekletilir.
    int validCount = 0;
    for(int i = 0; i < tapCount; i++) {
      if(now - tapTimes[i] < keepWindowMs) {
        tapTimes[validCount++] = tapTimes[i];
      }
    }
    tapCount = validCount;
    
    // Yeni tap ekle. Buffer dolu olsa bile en yeni tap her zaman kaydedilmeli.
    // Onceki davranisla 30+ tap'lik gesture'lar 30. taptan sonra hic eklenmiyordu;
    // simdi en eskiyi atip yeniyi yaziyoruz (logical circular buffer).
    if(tapCount >= MAX_TAP_HISTORY) {
      // Sondaki bir slotu acmak icin sola kaydir.
      for(int i = 1; i < MAX_TAP_HISTORY; i++) tapTimes[i - 1] = tapTimes[i];
      tapCount = MAX_TAP_HISTORY - 1;
    }
    tapTimes[tapCount++] = now;

    // TESHIS: her kaydedilen tap Serial'a yazilir. Gesture sorunlarinda
    // sayacin gercekten ilerleyip ilerlemedigini gosterir. (Sorun cozulunce
    // istenirse kaldirilabilir.)
    Serial.printf("[TAP] wifi=%d/%d pil=%d/%d\n",
                  countRecentTaps(now, wifiWindowMs), config.wifiTapCount,
                  countRecentTaps(now, batteryWindowMs), config.batteryTapCount);

    int recentWifiTaps = countRecentTaps(now, wifiWindowMs);
    if(recentWifiTaps >= config.wifiTapCount) {
      pendingBatteryGesture = false;  // WiFi oncelikli: 10 tap yaparken 5. tap pil ekranini acmasin
      Serial.printf("WiFi tap esigi algilandi (%d/%ds) - WiFi %s\n",
                    config.wifiTapCount, config.wifiTapWindowSec, wifiActive ? "kapaniyor" : "aciliyor");
      tapCount = 0;
      // WiFi gesture tetiklendiginde pil bildirimini de "yeni" olarak isaretle ki
      // hemen sonra 5 tap throttle'i nedeniyle bypass olmasın ama gereksiz cakismayi onler.
      lastBatteryTapNotifyAt = now;
      if(wifiActive) stopWiFiAP(); else startWiFiAP();
      // WiFi'yi acan son tap ayni zamanda poke/petting olarak islenmesin.
      ignoreTouchUntilRelease = true;
      prevTouch = touch;
      return;
    } else {
      int recentBatteryTaps = countRecentTaps(now, batteryWindowMs);
      // lastBatteryTapNotifyAt=0 baslangicta throttle kosulu
      // (now > batteryWindowMs + BATTERY_NOTIFICATION_DURATION_MS) olarak kalip
      // ilk 10 saniye gesture'i bloklamasını engelliyoruz: 0 ise her zaman gec.
      bool throttleClear = (lastBatteryTapNotifyAt == 0) ||
                           (now - lastBatteryTapNotifyAt > (batteryWindowMs + BATTERY_NOTIFICATION_DURATION_MS));
      if(recentBatteryTaps >= config.batteryTapCount && throttleClear) {
        // Hemen gosterme: kullanici WiFi icin tap'lemeye devam ediyorsa engellemesin.
        // Her yeni tap geldiginde tetigi son tap+settle'a tasi.
        // Eger pendingBatteryGesture zaten aktifse bu yine olur ve guncellenmis
        // olur; degilse de aktif edilir.
        pendingBatteryGesture = true;
        pendingBatteryTriggerAt = now + BATTERY_GESTURE_SETTLE_MS;
      } else if(pendingBatteryGesture) {
        // Pil esigi henuz dolmadi ama bekleyen bir gesture var: yeni tap geldi,
        // tetigi son tap+settle'a guncelle ki kullanici hala tap'liyorsa erken patlamasın.
        pendingBatteryTriggerAt = now + BATTERY_GESTURE_SETTLE_MS;
      }
    }
  }

  // Pil gesture'i esige ulastiysa, kisa bir duraksama bekle.
  // Bu sayede 7 sn icinde 10 tap WiFi komutu, 5 sn icinde 5 tap pil komutuyla cakisip bolunmez.
  if(pendingBatteryGesture && now >= pendingBatteryTriggerAt) {
    int recentWifiTaps = countRecentTaps(now, wifiWindowMs);
    if(recentWifiTaps >= config.wifiTapCount) {
      pendingBatteryGesture = false;
      Serial.printf("WiFi tap esigi algilandi (%d/%ds) - WiFi %s\n",
                    config.wifiTapCount, config.wifiTapWindowSec, wifiActive ? "kapaniyor" : "aciliyor");
      tapCount = 0;
      if(wifiActive) stopWiFiAP(); else startWiFiAP();
      ignoreTouchUntilRelease = true;
      prevTouch = touch;
      return;
    } else {
      int recentBatteryTaps = countRecentTaps(now, batteryWindowMs);
      if(recentBatteryTaps >= config.batteryTapCount) {
        updateBatteryMonitor(true);
        showBatteryNotification(batteryPercent, batteryVoltage);
        lastBatteryTapNotifyAt = now;
        Serial.printf("Pil tap esigi algilandi (%d/%ds) - %d%% %.2fV\n",
                      config.batteryTapCount, config.batteryTapWindowSec, batteryPercent, batteryVoltage);
      }
      pendingBatteryGesture = false;
    }
  }
  
  // Uzun basma -> petting
  if(touch && (now - touchStartTime >= (unsigned long)config.longPressMs)) {
    if(mood != MOOD_PETTING && mood != MOOD_DEEP_PETTING) {
      Serial.printf("[PETTING] long press hold=%lu/%d wifi=%d petCount=%d deepCount=%d\n",
                    (unsigned long)(now - touchStartTime), config.longPressMs, wifiActive ? 1 : 0,
                    pettingReactionCount, deepPettingReactionCount);
      enterMood(MOOD_PETTING);
    }
  }
  
  // Dokunma bitti
  if(!touch && prevTouch) {
    unsigned long duration = now - touchStartTime;
    
    if(duration < (unsigned long)config.longPressMs) {
      // Kisa dokun-birak -> poke. WiFi acikken guvenli/kucuk event secilir.
      if(mood == MOOD_IDLE) {
        enterMood(MOOD_POKED);
      }
    } else {
      // Uzun basma bitti -> petting after
      if(mood == MOOD_PETTING || mood == MOOD_DEEP_PETTING) {
        enterMood(MOOD_PETTING_AFTER);
      }
    }
    
    lastInteractionTime = now;
  }
  
  prevTouch = touch;
}

// ============================================================
// PIL OLCUMU
// ============================================================
void setupBatteryMonitor() {
  pinMode(PIN_BATTERY_ADC, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);
  // Acilista trend'i gercekten sifirla. Pil okuma rutinindeki force=true
  // bayrari sadece "okuma araliklari beklemeden olc" anlamina gelir, charging
  // trendine dokunmamali.
  updateBatteryMonitor(true, /*resetTrend=*/true);
  previousBatteryPercent = batteryPercent;
}

float readBatteryVoltage() {
  // Onceden 24 sample x 700us delay = ~17ms blocking ADC. Bu loop her
  // BATTERY_READ_INTERVAL_MS'de bir patliyordu ve drawCurrentFrame ile birlikte
  // gozle gorulur frame jitter veriyordu. Software low-pass (updateBatteryMonitor
  // icindeki 0.75/0.25 EMA) zaten gurultuyu yumusatiyor, bu yuzden sample sayisini
  // 8'e ve sample arasini 200us'a dusurduk: 8 x 250us toplam ~1.6ms blocking,
  // okuma kalitesi gozle gorulur degismiyor (multimetreyle dogrulanan kalibrasyon
  // 1.026f hala uyumlu kaliyor).
  const int samples = 8;
  uint32_t totalMv = 0;
  for(int i = 0; i < samples; i++) {
    totalMv += analogReadMilliVolts(PIN_BATTERY_ADC);
    delayMicroseconds(200);
  }
  float adcVoltage = (float)totalMv / (float)samples / 1000.0f;
  return adcVoltage * BATTERY_DIVIDER_RATIO * BATTERY_CALIBRATION;
}

int batteryVoltageToPercent(float voltage) {
  // Tek hucre LiPo icin yaklasik acik-devre yuzde tablosu.
  // Yuk altindayken deger biraz oynayabilir; bu yuzden sadece tahmini yuzdedir.
  const float volts[] = {3.30f, 3.50f, 3.60f, 3.70f, 3.75f, 3.80f, 3.85f, 3.90f, 3.95f, 4.00f, 4.10f, 4.20f};
  const int pcts[]    = {0,     5,     10,    20,    30,    40,    50,    60,    70,    80,    90,    100};
  const int n = sizeof(pcts) / sizeof(pcts[0]);

  if(voltage <= volts[0]) return 0;
  if(voltage >= volts[n - 1]) return 100;

  for(int i = 1; i < n; i++) {
    if(voltage <= volts[i]) {
      float ratio = (voltage - volts[i - 1]) / (volts[i] - volts[i - 1]);
      int pct = pcts[i - 1] + (int)((pcts[i] - pcts[i - 1]) * ratio + 0.5f);
      if(pct < 0) pct = 0;
      if(pct > 100) pct = 100;
      return pct;
    }
  }
  return 0;
}

void updateBatteryMonitor(bool force, bool resetTrend) {
  unsigned long now = millis();
  if(!force && (now - lastBatteryReadTime < BATTERY_READ_INTERVAL_MS)) return;
  lastBatteryReadTime = now;

  float measured = readBatteryVoltage();
  if(measured < 0.1f || measured > 5.5f) {
    // Baglanti yoksa veya beklenmedik okuma varsa eski degeri bozma.
    return;
  }

  if(batteryVoltage < 0.1f) batteryVoltage = measured;
  else batteryVoltage = batteryVoltage * 0.75f + measured * 0.25f;

  batteryPercent = batteryVoltageToPercent(batteryVoltage);
  // force=true gesture okumalari sarj tahmininin baseline'ini bozmasin.
  updateBatteryChargingEstimate(resetTrend);
}

void showBatteryNotification(int percent, float voltage) {
  // Pil bildirimi overlay olarak cizilir; WiFi bildirimiyle ust uste binmez.
  showingWifiNotification = false;
  showingBatteryNotification = true;
  batteryNotificationStartTime = millis();
  batteryNotificationPercent = percent;
  batteryNotificationVoltage = voltage;
  lastDrawnAnim = nullptr;
  lastDrawnFrame = -999;
  markDisplayDirty();
}

void checkBatteryWarnings() {
  updateBatteryMonitor(false);
  if(batteryPercent < 0) return;

  if(previousBatteryPercent < 0) {
    previousBatteryPercent = batteryPercent;
    return;
  }

  for(int i = 0; i < BATTERY_WARN_COUNT; i++) {
    int threshold = BATTERY_WARN_THRESHOLDS[i];

    // Sarjla/USB ile tekrar yukari cikarsa ayni esik bir sonraki dususte yeniden uyarabilir.
    if(batteryPercent >= threshold + 5) batteryWarnShown[i] = false;

    if(!batteryWarnShown[i] && previousBatteryPercent > threshold && batteryPercent <= threshold) {
      batteryWarnShown[i] = true;
      showBatteryNotification(batteryPercent, batteryVoltage);
      Serial.printf("Pil uyarisi: %d%% (%.2fV)\n", batteryPercent, batteryVoltage);
      break;
    }
  }

  previousBatteryPercent = batteryPercent;
}

void drawBatterySmallIcon(int x, int y, int percent) {
  if(percent < 0) return;
  int pct = percent;
  if(pct < 0) pct = 0;
  if(pct > 100) pct = 100;

  u8g2.drawFrame(x, y, 16, 8);
  u8g2.drawBox(x + 16, y + 2, 2, 4);
  int fillW = map(pct, 0, 100, 0, 14);
  if(fillW > 0) u8g2.drawBox(x + 1, y + 1, fillW, 6);
}

void updateBatteryChargingEstimate(bool resetTrend) {
  unsigned long now = millis();

  // resetTrend=true sadece gercek baseline reset durumunda gelir
  // (acilista veya pile yeniden baglanma sonrasi). Gesture/force okumalari
  // burayi tetiklemez; aksi halde sarjda iken tap ile pil bildirimi acilinca
  // batteryCharging=false oluyor ve sembol kayboluyordu.
  if(resetTrend || batteryChargeTrendVoltage < 0.1f) {
    batteryChargeTrendVoltage = batteryVoltage;
    batteryChargePeakVoltage = batteryVoltage;
    lastBatteryChargeTrendTime = now;
    lastBatteryChargingSeenTime = 0;
    batteryCharging = false;
    return;
  }

  if(batteryVoltage > batteryChargePeakVoltage) {
    batteryChargePeakVoltage = batteryVoltage;
  }

  // Sarjdan cikarilinca voltaj genelde kisa surede biraz geri oturur.
  // Boyle bir dusus gorursek hold suresini beklemeden sarj bilgisini kapat.
  if(batteryCharging && batteryChargePeakVoltage - batteryVoltage >= BATTERY_CHARGE_DROP_OFF_V) {
    batteryCharging = false;
    lastBatteryChargingSeenTime = 0;
    batteryChargeTrendVoltage = batteryVoltage;
    batteryChargePeakVoltage = batteryVoltage;
    lastBatteryChargeTrendTime = now;
    return;
  }

  if(now - lastBatteryChargeTrendTime < BATTERY_CHARGE_DETECT_INTERVAL_MS) {
    // Bir sure once yukselme yakalandiysa, voltaj anlik sabitlense bile bir muddet sarj oluyor kabul et.
    batteryCharging = (lastBatteryChargingSeenTime > 0 && now - lastBatteryChargingSeenTime < BATTERY_CHARGING_HOLD_MS);
    return;
  }

  float delta = batteryVoltage - batteryChargeTrendVoltage;
  if(delta >= BATTERY_CHARGE_RISE_THRESHOLD_V) {
    lastBatteryChargingSeenTime = now;
    batteryCharging = true;
    if(batteryVoltage > batteryChargePeakVoltage) batteryChargePeakVoltage = batteryVoltage;
  } else {
    // CHRG pini ESP32'ye bagli olmadigi icin bu kesin sarj bilgisi degil, voltaj trendi tahminidir.
    // 4.0V civarindan sonra voltaj artisi yavaslayabilir; bu yuzden son yukselmeden sonra 10 dk tutuyoruz.
    batteryCharging = (lastBatteryChargingSeenTime > 0 && now - lastBatteryChargingSeenTime < BATTERY_CHARGING_HOLD_MS);
  }

  batteryChargeTrendVoltage = batteryVoltage;
  lastBatteryChargeTrendTime = now;
}

void drawChargingBatteryOverlay() {
  if(!batteryCharging || batteryPercent < 0) return;

  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", batteryPercent);

  u8g2.setFont(u8g2_font_5x8_tr);
  int w = u8g2.getStrWidth(buf);
  // Animasyonun ustune okunabilir olsun diye sol ustte minik siyah zemin.
  u8g2.setDrawColor(0);
  u8g2.drawBox(0, 0, w + 4, 10);
  u8g2.setDrawColor(1);
  u8g2.drawStr(2, 8, buf);
}

void drawBatteryNotification() {
  // Pil bilgisi full-screen degil, mevcut yuzun ustune
  // kucuk overlay olarak cizilir. Burada clearBuffer()/sendBuffer() YOK.
  // Boylece pil bildirimi bitince default yuzu diskten yeniden yuklemek veya
  // OLED'e hard recovery yapmak gerekmez; animasyon state'i bozulmaz.
  int pct = batteryNotificationPercent >= 0 ? batteryNotificationPercent : batteryPercent;
  float volt = batteryNotificationVoltage > 0.1f ? batteryNotificationVoltage : batteryVoltage;
  if(pct < 0) pct = 0;
  if(pct > 100) pct = 100;

  char pctBuf[12];
  snprintf(pctBuf, sizeof(pctBuf), "%d%%", pct);
  char voltBuf[16];
  snprintf(voltBuf, sizeof(voltBuf), "%.2fV", volt);

  // Sag alt kose: 58x22 px kompakt kutu. Siyah zemin onceki frame'i temizler,
  // beyaz cerceve ve yazi overlay'i okunur yapar.
  const int x = 68;
  const int y = 41;
  const int w = 60;
  const int h = 23;

  u8g2.setDrawColor(0);
  u8g2.drawBox(x, y, w, h);
  u8g2.setDrawColor(1);
  u8g2.drawFrame(x, y, w, h);
  drawBatterySmallIcon(x + 4, y + 4, pct);

  u8g2.setFont(u8g2_font_6x10_tr);
  int pw = u8g2.getStrWidth(pctBuf);
  u8g2.drawStr(x + w - pw - 4, y + 10, pctBuf);

  u8g2.setFont(u8g2_font_5x8_tr);
  int vw = u8g2.getStrWidth(voltBuf);
  u8g2.drawStr(x + w - vw - 4, y + 20, voltBuf);

  u8g2.setDrawColor(1);
}

// ============================================================
// WIFI / AP
// ============================================================
String getAPSSID() {
  return String(AP_SSID);
}

// WiFi acildiginda ekranda gosterilen bildirim suresi (ms)
const unsigned long WIFI_NOTIFICATION_DURATION_MS = 3000;  // WiFi overlay suresi
unsigned long wifiNotificationStartTime = 0;
bool showingWifiNotification = false;

void showWifiNotification() {
  // WiFi full-screen bildirimi yok ve burada hard recovery/reload yok.
  // AP basladiktan sonra default yuzu yeniden LittleFS'ten yuklemeye calismak
  // heap/max-alloc dusuk oldugu icin blink gibi buyuk yuzleri "bozuk" sanabiliyor.
  // Sadece mevcut animasyonu dirty yap; drawCurrentFrame WiFi ikonunu eklesin.
  showingBatteryNotification = false;
  showingWifiNotification = false;
  lastDrawnWifi = !wifiActive;
  markDisplayDirty();
}

void drawWifiNotification() {
  String ssid = getAPSSID();
  String ip = WiFi.softAPIP().toString();
  
  u8g2.clearBuffer();
  
  // Sol ust: kucuk WiFi sembolu
  // Merkez (10, 8), yukari kavisler
  int cx = 10, cy = 10;
  u8g2.drawCircle(cx, cy, 7, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  u8g2.drawCircle(cx, cy, 4, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  u8g2.drawDisc(cx, cy - 1, 1);
  
  // Ust: baslik
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(24, 10, "WiFi Active");
  
  // Ayraci cizgi
  u8g2.drawHLine(0, 16, 128);
  
  // SSID
  u8g2.setFont(u8g2_font_5x8_tr);
  u8g2.drawStr(2, 28, "AP:");
  u8g2.setFont(u8g2_font_6x10_tr);
  // SSID uzun ise kucuk fontla
  if(u8g2.getStrWidth(ssid.c_str()) > 100) {
    u8g2.setFont(u8g2_font_5x8_tr);
  }
  u8g2.drawStr(22, 28, ssid.c_str());
  
  // IP
  u8g2.setFont(u8g2_font_5x8_tr);
  u8g2.drawStr(2, 42, "IP:");
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(22, 42, ip.c_str());
  
  // Alt: ayraci cizgi + ipucu mesaji
  u8g2.drawHLine(0, 48, 128);
  u8g2.setFont(u8g2_font_5x8_tr);
    u8g2.drawStr(2, 60, "Open 192.168.4.1");

  u8g2.sendBuffer();
  // Notification ayni kareyi her loop'ta tekrar basmasin.
  displayDirty = false;
  lastDrawnAnim = nullptr;
  lastDrawnFrame = -999;
}

void recoverDisplayAfterNotification(const char* source) {
  // Notification recovery default animasyonu yeniden yuklemez.
  // Eski yol pil/WiFi bildirimi sonrasi blink gibi buyuk yuzleri tekrar
  // LittleFS'ten yukleyip heap fragmentasyonu ve OLED bozulmasi uretiyordu.
  Serial.printf("[DISPLAY] notification recover safe: %s\n", source ? source : "?");
  showingBatteryNotification = false;
  showingWifiNotification = false;
  lastDrawnAnim = nullptr;
  lastDrawnFrame = -999;
  lastDrawnCharging = !batteryCharging;
  lastDrawnWifi = !wifiActive;
  markDisplayDirty();
  drawCurrentFrame();
}

void restoreDefaultFaceAfterWiFi() {
  // WiFi kapanirken default animasyonu gereksiz reload etme.
  // currentAnim saglamsa ayni RAM slotundan devam eder.
  bool currentDefaultReady = currentAnim.loaded && currentAnim.frames_data &&
                             currentAnim.frame_count > 0 &&
                             currentAnim.name == config.currentFace;
  freeAnimation(eventAnim);
  mood = MOOD_IDLE;
  moodStartTime = millis();
  idleEventPlaying = false;
  deepPettingPending = false;

  if(currentDefaultReady) {
    activeAnim = &currentAnim;
    if(activeFrame < 0 || activeFrame >= currentAnim.frame_count) activeFrame = 0;
    lastFrameTime = millis();
    lastDrawnAnim = nullptr;
    lastDrawnFrame = -999;
    markDisplayDirty();
    Serial.println("[WIFI] default resume via=ram");
  } else {
    activeAnim = nullptr;
    activeFrame = 0;
    markDisplayDirty();
    if(!loadDefaultIdleFace(false)) {
      Serial.println("[WIFI] default resume failed; fallback active");
    }
  }
}

void startWiFiAP() {
  if(wifiActive) return;

  String ssid = getAPSSID();
  Serial.printf("[WIFI] ap_start ssid=%s\n", ssid.c_str());

  heapBeforeWiFiStart = ESP.getFreeHeap();

  // WiFi acilirken default yuzu ASLA bosaltma.
  // Eski low-memory snapshot yolu tamamen kapali.
  // Cihaz ayar arayuzune girerken gecici event birakilir; default blink/currentFace
  // RAM'de kalir ve sadece WiFi ikon overlay'i cizilir.
  freeAnimation(eventAnim);
  idleEventPlaying = false;
  deepPettingPending = false;
  mood = MOOD_IDLE;
  moodStartTime = millis();

  bool currentDefaultReady = currentAnim.loaded && currentAnim.frames_data &&
                             currentAnim.frame_count > 0 &&
                             currentAnim.name == config.currentFace;
  if(!currentDefaultReady) {
    // WiFi baslamadan once heap henuz daha temizken default yuzu yukle.
    // AP basladiktan sonra buyuk yuz reload etmek yerine burada bitiriyoruz.
    if(!loadDefaultIdleFace(false)) {
      Serial.println("WiFi oncesi default yuz yuklenemedi; mevcut/fallback cizim korunacak.");
    }
  }
  if(currentAnim.loaded && currentAnim.frames_data) {
    activeAnim = &currentAnim;
    if(activeFrame < 0 || activeFrame >= currentAnim.frame_count) activeFrame = 0;
    lastFrameTime = millis();
  }
  lastDrawnWifi = !wifiActive;
  markDisplayDirty();
  Serial.printf("[WIFI] default animasyon RAMDE KORUNUYOR + SAFE eventler aktif, free=%u, max=%u\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // AP arayuzu icin yalnizca AP modunu ac. Eski WIFI_AP_STA yolu gereksiz STA
  // bufferlari ayiriyor ve kayitli ağa baglanmaya calisirken AP kanalini
  // degistirebiliyordu. STA ancak kullanici arayuzden Baglan'a bastiginda acilir.
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  delay(30);
  WiFi.mode(WIFI_AP);
  delay(30);
  // WiFi TX gucu 13 dBm: yerel ayar arayuzu icin yeterli, besleme piklerini azaltir.
  if(!WiFi.setTxPower(WIFI_POWER_13dBm)) {
    Serial.println("Uyari: WiFi TX gucu 13 dBm'e ayarlanamadi");
  }

  IPAddress apIP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, gateway, subnet);
  bool apStarted = WiFi.softAP(ssid.c_str(), AP_PASSWORD, 1, false, 4);
  if(!apStarted) {
    Serial.println("HATA: WiFi AP baslatilamadi");
    WiFi.mode(WIFI_OFF);
    wifiActive = false;
    restoreDefaultFaceAfterWiFi();
    return;
  }

  delay(80);

  // handler'lar setup()'ta bir kez kayitli; burada sadece listener acilir.
  server.begin();

  wifiActive = true;
  wifiActivatedTime = millis();
  lastWebActivityTime = millis();
  lastAPClientSeenTime = 0;
  heapAfterWiFiStart = ESP.getFreeHeap();

  Serial.printf("[WIFI] ap_ready ssid=%s ip=%s free=%u max=%u\n",
                ssid.c_str(), WiFi.softAPIP().toString().c_str(),
                heapAfterWiFiStart, ESP.getMaxAllocHeap());

  // Ekranda bildirim goster
  showWifiNotification();
}

void stopWiFiAP() {
  if(!wifiActive) return;

  Serial.println("[WIFI] ap_stop");
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_OFF);
  delay(30);
  wifiActive = false;
  lastAPClientSeenTime = 0;
  showingWifiNotification = false;
  restoreDefaultFaceAfterWiFi();
}

// ============================================================
// WEB SERVER (HTML/JS arayuz string'leri uzun, alt fonksiyonlarda)
// ============================================================

// Index.html - tek sayfa, sekmeli, glassmorphism
// Web UI Mochi_html.h dosyasinda gzip-compressed byte array.
// handleRoot artik bu byte array'i Content-Encoding: gzip ile serve ediyor.

// ============================================================
// WEB SERVER HANDLER'LARI
// ============================================================
void handleRoot() {
  lastWebActivityTime = millis();

  // WebServer'in kendi binary-safe PROGMEM yolunu kullan. Manuel WiFiClient
  // dongusu baglanti durumuna gore HTML'i yarida kesebiliyor ve handler
  // tamamlanmadigi icin sonraki /api isteklerini de kilitleyebiliyordu.
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.send_P(200, PSTR("text/html; charset=utf-8"),
                reinterpret_cast<PGM_P>(INDEX_HTML_GZ), INDEX_HTML_GZ_LEN);

  serviceDisplayDuringBlockingWork();
}

void handleApiFaces() {
  lastWebActivityTime = millis();
  
  if(!littlefsReady) {
    JsonDocument errDoc;
    errDoc["error"] = littlefsStatus;
    errDoc["faces"].to<JsonArray>();
    String out; serializeJson(errDoc, out);
    server.send(200, "application/json", out);
    return;
  }

  JsonDocument doc;
  doc["active"] = config.currentFace;
  JsonArray arr = doc["faces"].to<JsonArray>();
  
  File root = LittleFS.open("/faces");
  if(root && root.isDirectory()) {
    File f = root.openNextFile();
    while(f) {
      if(!f.isDirectory()) {
        String fname = f.name();
        // Upload sirasinda olusan tmp dosyasini listede gosterme.
        if(fname.endsWith(".bin") && !fname.startsWith("_upload.tmp") && fname.indexOf("/_upload.tmp") < 0) {
          // Header oku, frame sayisini ve formati al
          uint8_t header[BIN_HEADER_SIZE];
          if(f.read(header, BIN_HEADER_SIZE) == BIN_HEADER_SIZE && 
             (memcmp(header, BIN_MAGIC, 4) == 0 || memcmp(header, BIN_MAGIC_V3, 4) == 0)) {
            JsonObject o = arr.add<JsonObject>();
            String name = fname.substring(0, fname.length() - 4);  // .bin sil
            int slashIdx = name.lastIndexOf('/');
            if(slashIdx >= 0) name = name.substring(slashIdx + 1);
            o["name"] = name;
            o["frames"] = header[4];
            o["size"] = f.size();
            o["format"] = (memcmp(header, BIN_MAGIC_V3, 4) == 0) ? "MOC3" : "MOC2";
            o["category"] = getFaceCategory(name);
          }
        }
      }
      f = root.openNextFile();
    }
  }
  
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleApiPreview() {
  lastWebActivityTime = millis();

  if(!littlefsReady) {
    server.send(503, "text/plain", littlefsStatus);
    return;
  }
  if(!server.hasArg("name")) {
    server.send(400, "text/plain", "name?");
    return;
  }

  String name = sanitizeFaceName(server.arg("name"));
  if(name.length() == 0) {
    server.send(400, "text/plain", "bad name");
    return;
  }

  String path = "/faces/" + name + ".bin";
  File f = LittleFS.open(path, "r");
  if(!f) {
    server.send(404, "text/plain", "not found");
    return;
  }

  uint8_t header[BIN_HEADER_SIZE];
  if(f.read(header, BIN_HEADER_SIZE) != BIN_HEADER_SIZE) {
    f.close();
    server.send(400, "text/plain", "bad header");
    return;
  }

  bool isMoc2 = (memcmp(header, BIN_MAGIC, 4) == 0);
  bool isMoc3 = (memcmp(header, BIN_MAGIC_V3, 4) == 0);
  if(!isMoc2 && !isMoc3) {
    f.close();
    server.send(400, "text/plain", "bad magic");
    return;
  }
  if(header[4] == 0 || header[5] != 16 || header[6] != 64) {
    f.close();
    server.send(400, "text/plain", "bad size");
    return;
  }
  if(isMoc3 && header[7] != BIN_FORMAT_RLE_DELTA) {
    f.close();
    server.send(400, "text/plain", "bad moc3");
    return;
  }

  // YENIDEN TASARIM: Eskiden endpoint 3 frame ORNEKLIYORDU (heap korumasi) —
  // 74 karelik animasyon UI'da 3 karede donunce ~25x hizli/atlamali gorunuyordu.
  // Artik dosya OLDUGU GIBI stream edilir; MOC3 decode TARAYICIDA yapilir (JS
  // RLE-delta cozucu). Cihaz maliyeti: sadece chunk'li dosya okuma + TCP.
  // Ornekleme yok — tum kareler gercek delay'leriyle gider. Eski 3 static
  // buffer (~5KB kalici RAM) da bu sayede kaldirildi.
  size_t total = f.size();
  f.seek(0, SeekSet);

  server.sendHeader("Content-Type", "application/octet-stream");
  server.sendHeader("Cache-Control", "max-age=30");
  server.setContentLength(total);
  server.send(200, "application/octet-stream", "");

  static uint8_t chunkBuf[1024];
  size_t sent = 0;
  while(sent < total) {
    size_t n = f.read(chunkBuf, sizeof(chunkBuf));
    if(n == 0) break;
    server.sendContent((const char*)chunkBuf, n);
    sent += n;
    // Chunk'lar arasinda OLED'i besle — buyuk dosyada ekran donmasin.
    serviceDisplayDuringBlockingWork();
  }
  f.close();
}

void handleApiSetActive() {
  lastWebActivityTime = millis();
  
  if(!server.hasArg("name")) {
    server.send(400, "text/plain", "name?");
    return;
  }
  
  // Path traversal'a karsi sanitize.
  String name = sanitizeFaceName(server.arg("name"));
  if(name.length() == 0) {
    server.send(400, "text/plain", "bad name");
    return;
  }
  if(!faceFileExists(name)) {
    server.send(404, "text/plain", "face not found: " + name);
    return;
  }

  config.currentFace = name;
  config.defaultFace = name;
  saveConfig();
  
  // Hemen ana/default yuze don ve ara idle planini yenile.
  mood = MOOD_IDLE;
  waitingForFaceSelection = false;
  waitingMissingFaceName = "";
  idleEventPlaying = false;
  nextIdleEventAt = 0;
  if(loadDefaultIdleFace(true)) {
    scheduleNextRandomEvent();
  }

  server.send(200, "text/plain", "ok");
}

void handleApiDelete() {
  lastWebActivityTime = millis();

  if(!littlefsReady) {
    server.send(503, "text/plain", littlefsStatus);
    return;
  }
  
  if(!server.hasArg("name")) {
    server.send(400, "text/plain", "name?");
    return;
  }
  
  // Path traversal'a karsi sanitize. "../config" gibi adlar reddedilir.
  String name = sanitizeFaceName(server.arg("name"));
  if(name.length() == 0) {
    server.send(400, "text/plain", "bad name");
    return;
  }
  String path = "/faces/" + name + ".bin";
  
  if(LittleFS.remove(path)) {
    invalidateFaceCache();  // dosya silindi, cache'i bir sonraki sorguda yenile
    removeIdleAnimByName(name);
    removeNameFromArray(pokeReactions, pokeReactionCount, name);
    removeNameFromArray(pettingReactions, pettingReactionCount, name);
    removeNameFromArray(deepPettingReactions, deepPettingReactionCount, name);
    removeNameFromArray(openingEvents, openingEventCount, name);
    removeNameFromArray(randomEvents, randomEventCount, name);
    removeNameFromArray(sleepEnterEvents, sleepEnterEventCount, name);
    removeNameFromArray(sleepWakeEvents, sleepWakeEventCount, name);
    if(config.sleepEnterFace == name) config.sleepEnterFace = "";
    if(config.sleepWakeFace == name) config.sleepWakeFace = "";
    removeFaceCategory(name);
    resetRandomHistories();

    // Aktif/default yuz silindiyse config'i KAYDETMEDEN ONCE bosalt.
    // Eski sira: removeName -> saveConfig -> if(active) clear -> NO save.
    // Bu, /config.json'un silinmis bir yuze isaret etmeye devam etmesine neden
    // oluyordu; sonraki boot'ta cihaz olmayan yuzu yuklemeye calisiyordu.
    bool wasActive = (config.currentFace == name || currentAnim.name == name);
    if(wasActive) {
      config.currentFace = "";
      config.defaultFace = "";
    }
    saveConfig();
    if(wasActive) {
      enterFallbackWaitMode(name, "Aktif yuz silindi; yeni yuz secmeni bekliyorum.");
    }
    server.send(200, "text/plain", "ok");
  } else {
    server.send(500, "text/plain", "fail");
  }
}

void handleApiDeleteAll() {
  lastWebActivityTime = millis();

  if(!littlefsReady) {
    server.send(503, "text/plain", littlefsStatus);
    return;
  }
  
  File root = LittleFS.open("/faces");
  if(root && root.isDirectory()) {
    File f = root.openNextFile();
    while(f) {
      String name = f.name();
      f.close();
      if(name.endsWith(".bin")) {
        String fullPath = "/faces/" + name.substring(name.lastIndexOf('/') + 1);
        LittleFS.remove(fullPath);
      }
      f = root.openNextFile();
    }
  }
  invalidateFaceCache();  // tum yuzler silindi, cache temizlensin
  
  // Reset bazi config
  config.currentFace = "";
  config.defaultFace = "";
  idleAnimCount = 0;
  pokeReactionCount = 0;
  pettingReactionCount = 0;
  deepPettingReactionCount = 0;
  openingEventCount = 0;
  randomEventCount = 0;
  sleepEnterEventCount = 0;
  sleepWakeEventCount = 0;
  faceCategoryCount = 0;
  config.sleepEnterFace = "";
  config.sleepWakeFace = "";
  nextRandomEventAt = 0;
  nextIdleEventAt = 0;
  resetRandomHistories();
  saveConfig();
  enterFallbackWaitMode("", "Tum yuzler silindi; arayuzden yeni yuz yuklemeni bekliyorum.");
  
  server.send(200, "text/plain", "ok");
}

// Upload handler
File uploadFile;
// Upload boyunca veri /faces/_upload.tmp.bin'a yazilir; UPLOAD_FILE_END'de
// validateFaceFileAtPath ile dogrulanir, sonra atomik olarak final adina rename
// edilir. Boylece partial/bozuk upload, varsa eski calisan ayni isimli yuzu
// bozmaz; eski dosya rename anina kadar dokunulmamis kalir.
const char* UPLOAD_TMP_PATH = "/faces/_upload.tmp.bin";

void handleUpload() {
  lastWebActivityTime = millis();
  if(!littlefsReady) return;
  HTTPUpload& upload = server.upload();
  
  if(upload.status == UPLOAD_FILE_START) {
    uploadHadBinExtension = false;
    uploadHadValidContent = false;
    lastUploadFaceName = "";

    String filename = upload.filename;
    if(!filename.endsWith(".bin")) {
      Serial.println("Sadece .bin dosyalari");
      return;
    }

    // Path traversal'a karsi sanitize. Dosya icindeki "/" karakterlerini
    // soyup bos yere yazmadan "name" kismini cekiyoruz, sonra sanitizer'dan
    // geciriyoruz. Sanitize fail ederse upload'i hic baslatmiyoruz.
    String cleanName = filename;
    int slashIdx = cleanName.lastIndexOf('/');
    if(slashIdx >= 0) cleanName = cleanName.substring(slashIdx + 1);
    if(cleanName.endsWith(".bin")) cleanName = cleanName.substring(0, cleanName.length() - 4);
    cleanName = sanitizeFaceName(cleanName);
    if(cleanName.length() == 0) {
      Serial.printf("Upload reddedildi (gecersiz isim): %s\n", filename.c_str());
      return;
    }
    lastUploadFaceName = cleanName;
    uploadHadBinExtension = true;

    if(!LittleFS.exists("/faces")) LittleFS.mkdir("/faces");

    // Daima TMP path'a yaz; eski dosya bozulmasin.
    if(uploadFile) uploadFile.close();
    LittleFS.remove(UPLOAD_TMP_PATH);  // varsa kalintiyi temizle
    uploadFile = LittleFS.open(UPLOAD_TMP_PATH, "w");
    Serial.printf("Upload basliyor (tmp): %s -> %s\n", filename.c_str(), cleanName.c_str());
  } else if(upload.status == UPLOAD_FILE_WRITE) {
    if(uploadFile) {
      uploadFile.write(upload.buf, upload.currentSize);
      serviceDisplayDuringBlockingWork();
    }
  } else if(upload.status == UPLOAD_FILE_END) {
    if(uploadFile) {
      uploadFile.close();
      Serial.printf("Upload bitti (tmp): %d byte\n", upload.totalSize);
    }
  }
}

void handleUploadComplete() {
  lastWebActivityTime = millis();

  // Tmp dosyayi dogrula, gecersizse sil; gecerliyse atomik rename ile
  // final isime al. Eski yuz dosyasi rename anına kadar dokunulmamis kaliyor;
  // bu rename overwrite yaptıgından eski dosya icerigi bozulmaz, atomik degisir.
  if(uploadHadBinExtension && lastUploadFaceName.length() > 0) {
    if(validateFaceFileAtPath(UPLOAD_TMP_PATH)) {
      String finalPath = "/faces/" + lastUploadFaceName + ".bin";
      // Bazi LittleFS surumlerinde rename hedef varsa fail ediyor; once silelim.
      if(LittleFS.exists(finalPath)) LittleFS.remove(finalPath);
      if(LittleFS.rename(UPLOAD_TMP_PATH, finalPath)) {
        uploadHadValidContent = true;
        invalidateFaceCache();  // yeni yuz eklendi/uzerine yazildi, cache yenilensin
        Serial.printf("Upload basariyla tamamlandi: %s\n", finalPath.c_str());
      } else {
        Serial.printf("Rename basarisiz: %s -> %s\n", UPLOAD_TMP_PATH, finalPath.c_str());
        LittleFS.remove(UPLOAD_TMP_PATH);
        uploadHadValidContent = false;
      }
    } else {
      Serial.printf("Upload gecersiz - tmp siliniyor: %s\n", lastUploadFaceName.c_str());
      LittleFS.remove(UPLOAD_TMP_PATH);
      uploadHadValidContent = false;
    }
  } else {
    // Header asla baslamadi (.bin uzantisi yok / sanitize fail) - gene de kalintiyi sil.
    LittleFS.remove(UPLOAD_TMP_PATH);
  }

  // Hic yuz yokken ilk yuklenen dosyayi otomatik aktif yap ki fallback'te kalmasin.
  if(uploadHadValidContent && lastUploadFaceName.length() > 0 && faceFileExists(lastUploadFaceName)) {
    bool activeMissing = (config.currentFace.length() == 0 || !faceFileExists(config.currentFace) || waitingForFaceSelection);
    if(activeMissing) {
      config.currentFace = lastUploadFaceName;
      config.defaultFace = lastUploadFaceName;
      saveConfig();
      mood = MOOD_IDLE;
      waitingForFaceSelection = false;
      waitingMissingFaceName = "";
      idleEventPlaying = false;
      nextIdleEventAt = 0;
      bool activated = false;
      activated = loadDefaultIdleFace(true);
      if(activated) scheduleNextRandomEvent();
      if(activated) {
        Serial.printf("Ilk/yeni yuz otomatik aktif edildi: %s\n", lastUploadFaceName.c_str());
      }
    }
  }

  JsonDocument doc;
  doc["ok"] = uploadHadValidContent;
  doc["name"] = lastUploadFaceName;
  if(uploadHadBinExtension && !uploadHadValidContent) {
    doc["error"] = "Corrupt or unsupported .bin (must be 128x64 MOC2/MOC3)";
  } else if(!uploadHadBinExtension) {
    doc["error"] = "Invalid name/extension (.bin required, A-Z 0-9 _ - max 32 chars)";
  }
  doc["fsUsed"] = littlefsReady ? LittleFS.usedBytes() : 0;
  doc["fsTotal"] = littlefsReady ? LittleFS.totalBytes() : 0;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleApiConfig() {
  lastWebActivityTime = millis();
  
  if(server.method() == HTTP_GET) {
    JsonDocument doc;
    doc["defaultFace"] = config.currentFace;
    doc["currentFace"] = config.currentFace;
    doc["wifiSSID"] = config.wifiSSID;
    doc["wifiAutoConnect"] = config.wifiAutoConnect;
    doc["wifiTimeoutMinutes"] = config.wifiTimeoutMinutes;
    doc["wifiTapCount"] = config.wifiTapCount;
    doc["wifiTapWindowSec"] = config.wifiTapWindowSec;
    doc["batteryTapCount"] = config.batteryTapCount;
    doc["batteryTapWindowSec"] = config.batteryTapWindowSec;
    doc["longPressMs"] = config.longPressMs;
    doc["deepPettingMs"] = config.deepPettingMs;
    doc["touchDebounceMs"] = config.touchDebounceMs;
    doc["displayBrightnessPercent"] = config.displayBrightnessPercent;
    doc["displayBrightnessContrast"] = displayBrightnessPercentToContrast(config.displayBrightnessPercent);
    doc["sleepEnabled"] = config.sleepEnabled;
    doc["sleepTimeoutMinutes"] = config.sleepTimeoutMinutes;
    doc["sleepEnterFace"] = config.sleepEnterFace;
    doc["sleepWakeFace"] = config.sleepWakeFace;
    // sleepEnterMs/sleepWakeMs export silindi.
    doc["sleepMode"] = config.sleepMode;
    doc["sleepDeepAfterMinutes"] = config.sleepDeepAfterMinutes;
    doc["speedMultiplier"] = speedMultiplier;
    doc["randomEventMinSec"] = randomEventMinMs / 1000UL;
    doc["randomEventMaxSec"] = randomEventMaxMs / 1000UL;
    doc["idleEventMinSec"] = idleEventMinMs / 1000UL;
    doc["idleEventMaxSec"] = idleEventMaxMs / 1000UL;
    
    JsonArray idleArr = doc["idleList"].to<JsonArray>();
    for(int i = 0; i < idleAnimCount; i++) {
      JsonObject o = idleArr.add<JsonObject>();
      o["name"] = idleAnims[i].name;
      o["loops"] = idleAnims[i].loops <= 0 ? 1 : idleAnims[i].loops;
      o["seconds"] = idleAnims[i].seconds < 0 ? 0 : idleAnims[i].seconds;
    }

    JsonObject cats = doc["faceCategories"].to<JsonObject>();
    for(int i = 0; i < faceCategoryCount; i++) cats[faceCategoryNames[i]] = faceCategoryValues[i];
    
    // 7 array dump pattern'i helper ile sadelestirildi.
    dumpStringArrayToJson(doc, "pokeReactions", pokeReactions, pokeReactionCount);
    dumpStringArrayToJson(doc, "pettingReactions", pettingReactions, pettingReactionCount);
    dumpStringArrayToJson(doc, "deepPettingReactions", deepPettingReactions, deepPettingReactionCount);
    dumpStringArrayToJson(doc, "openingEvents", openingEvents, openingEventCount);
    dumpStringArrayToJson(doc, "randomEvents", randomEvents, randomEventCount);
    dumpStringArrayToJson(doc, "sleepEnterEvents", sleepEnterEvents, sleepEnterEventCount);
    dumpStringArrayToJson(doc, "sleepWakeEvents", sleepWakeEvents, sleepWakeEventCount);

    // ivmeolcer config GET response'a ekle
    JsonObject aGet = doc["accel"].to<JsonObject>();
    aGet["enabled"]                = accelEnabled;
    aGet["softShakeThreshold"]     = accelSoftThreshold;
    aGet["hardShakeThreshold"]     = accelHardThreshold;
    aGet["accelerationThreshold"]  = accelAccelThreshold;
    aGet["freefallDuration"]       = (int)accelFreefallDurMs;
    aGet["pickupInactivityMs"]     = (int)accelPickupInactivityMs;
    aGet["ready"]                  = accelReady;
    dumpStringArrayToJson(aGet, "softShakeReactions",    accelSoftShakeReactions,   accelSoftShakeCount);
    dumpStringArrayToJson(aGet, "hardShakeReactions",    accelHardShakeReactions,   accelHardShakeCount);
    dumpStringArrayToJson(aGet, "accelerationReactions", accelAccelReactions,       accelAccelCount);
    dumpStringArrayToJson(aGet, "freefallReactions",     accelFreefallReactions,    accelFreefallCount);
    dumpStringArrayToJson(aGet, "orientationReactions",  accelOrientationReactions, accelOrientationCount);
    dumpStringArrayToJson(aGet, "pickupReactions",       accelPickupReactions,      accelPickupCount);
    
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  } else if(server.method() == HTTP_POST) {
    String body = server.arg("plain");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if(err) {
      server.send(400, "text/plain", "json err");
      return;
    }
    
    // Eski UI config gonderirse uyumluluk icin defaultFace'i currentFace bosken kabul et.
    if(doc["currentFace"].is<String>()) {
      config.currentFace = doc["currentFace"].as<String>();
    } else if(doc["defaultFace"].is<String>()) {
      config.currentFace = doc["defaultFace"].as<String>();
    }
    config.defaultFace = config.currentFace;
    if(doc["wifiTimeoutMinutes"].is<int>()) {
      config.wifiTimeoutMinutes = doc["wifiTimeoutMinutes"].as<int>();
      if(config.wifiTimeoutMinutes < 1) config.wifiTimeoutMinutes = 1;
      if(config.wifiTimeoutMinutes > 120) config.wifiTimeoutMinutes = 120;
    }
    if(doc["wifiTapCount"].is<int>()) {
      config.wifiTapCount = doc["wifiTapCount"].as<int>();
      if(config.wifiTapCount < MIN_TAP_TRIGGER_COUNT) config.wifiTapCount = MIN_TAP_TRIGGER_COUNT;
      if(config.wifiTapCount > MAX_TAP_TRIGGER_COUNT) config.wifiTapCount = MAX_TAP_TRIGGER_COUNT;
    }
    if(doc["wifiTapWindowSec"].is<int>()) {
      config.wifiTapWindowSec = doc["wifiTapWindowSec"].as<int>();
      if(config.wifiTapWindowSec < MIN_TAP_WINDOW_SEC) config.wifiTapWindowSec = MIN_TAP_WINDOW_SEC;
      if(config.wifiTapWindowSec > MAX_TAP_WINDOW_SEC) config.wifiTapWindowSec = MAX_TAP_WINDOW_SEC;
    }
    if(doc["batteryTapCount"].is<int>()) {
      config.batteryTapCount = doc["batteryTapCount"].as<int>();
      if(config.batteryTapCount < MIN_TAP_TRIGGER_COUNT) config.batteryTapCount = MIN_TAP_TRIGGER_COUNT;
      if(config.batteryTapCount > MAX_TAP_TRIGGER_COUNT) config.batteryTapCount = MAX_TAP_TRIGGER_COUNT;
    }
    if(doc["batteryTapWindowSec"].is<int>()) {
      config.batteryTapWindowSec = doc["batteryTapWindowSec"].as<int>();
      if(config.batteryTapWindowSec < MIN_TAP_WINDOW_SEC) config.batteryTapWindowSec = MIN_TAP_WINDOW_SEC;
      if(config.batteryTapWindowSec > MAX_TAP_WINDOW_SEC) config.batteryTapWindowSec = MAX_TAP_WINDOW_SEC;
    }
    if(doc["longPressMs"].is<int>()) {
      config.longPressMs = doc["longPressMs"].as<int>();
      if(config.longPressMs < LONG_PRESS_MIN_MS) config.longPressMs = LONG_PRESS_MIN_MS;
      if(config.longPressMs > LONG_PRESS_MAX_MS) config.longPressMs = LONG_PRESS_MAX_MS;
    }
    if(doc["deepPettingMs"].is<int>()) {
      config.deepPettingMs = doc["deepPettingMs"].as<int>();
      if(config.deepPettingMs < DEEP_PETTING_MIN_MS) config.deepPettingMs = DEEP_PETTING_MIN_MS;
      if(config.deepPettingMs > DEEP_PETTING_MAX_MS) config.deepPettingMs = DEEP_PETTING_MAX_MS;
    }
    if(doc["touchDebounceMs"].is<int>()) {
      config.touchDebounceMs = doc["touchDebounceMs"].as<int>();
      if(config.touchDebounceMs < TOUCH_DEBOUNCE_MIN_MS) config.touchDebounceMs = TOUCH_DEBOUNCE_MIN_MS;
      if(config.touchDebounceMs > TOUCH_DEBOUNCE_MAX_MS) config.touchDebounceMs = TOUCH_DEBOUNCE_MAX_MS;
    }
    if(doc["displayBrightnessPercent"].is<int>()) {
      config.displayBrightnessPercent = doc["displayBrightnessPercent"].as<int>();
      if(config.displayBrightnessPercent < MIN_DISPLAY_BRIGHTNESS_PERCENT) config.displayBrightnessPercent = MIN_DISPLAY_BRIGHTNESS_PERCENT;
      if(config.displayBrightnessPercent > MAX_DISPLAY_BRIGHTNESS_PERCENT) config.displayBrightnessPercent = MAX_DISPLAY_BRIGHTNESS_PERCENT;
      applyDisplayBrightness();
    }
    if(doc["sleepEnabled"].is<bool>()) config.sleepEnabled = doc["sleepEnabled"].as<bool>();
    if(doc["sleepTimeoutMinutes"].is<int>()) {
      config.sleepTimeoutMinutes = doc["sleepTimeoutMinutes"].as<int>();
      if(config.sleepTimeoutMinutes < MIN_SLEEP_TIMEOUT_MINUTES) config.sleepTimeoutMinutes = MIN_SLEEP_TIMEOUT_MINUTES;
      if(config.sleepTimeoutMinutes > MAX_SLEEP_TIMEOUT_MINUTES) config.sleepTimeoutMinutes = MAX_SLEEP_TIMEOUT_MINUTES;
    }
    if(doc["sleepEnterFace"].is<String>()) config.sleepEnterFace = doc["sleepEnterFace"].as<String>();
    if(doc["sleepWakeFace"].is<String>()) config.sleepWakeFace = doc["sleepWakeFace"].as<String>();
    if(doc["sleepEnterEvents"].is<JsonArray>()) {
      sleepEnterEventCount = 0;
      for(JsonVariant v : doc["sleepEnterEvents"].as<JsonArray>()) {
        if(sleepEnterEventCount < 50) sleepEnterEvents[sleepEnterEventCount++] = v.as<String>();
      }
    } else if(doc["sleepEnterFace"].is<String>()) {
      sleepEnterEventCount = 0;
      if(config.sleepEnterFace.length() > 0) sleepEnterEvents[sleepEnterEventCount++] = config.sleepEnterFace;
    }
    if(doc["sleepWakeEvents"].is<JsonArray>()) {
      sleepWakeEventCount = 0;
      for(JsonVariant v : doc["sleepWakeEvents"].as<JsonArray>()) {
        if(sleepWakeEventCount < 50) sleepWakeEvents[sleepWakeEventCount++] = v.as<String>();
      }
    } else if(doc["sleepWakeFace"].is<String>()) {
      sleepWakeEventCount = 0;
      if(config.sleepWakeFace.length() > 0) sleepWakeEvents[sleepWakeEventCount++] = config.sleepWakeFace;
    }
    // sleepEnterMs/sleepWakeMs import okuma blogu silindi.
    if(doc["sleepMode"].is<int>()) {
      config.sleepMode = doc["sleepMode"].as<int>();
      if(config.sleepMode < SLEEP_MODE_LIGHT || config.sleepMode > SLEEP_MODE_HYBRID) config.sleepMode = DEFAULT_SLEEP_MODE;
    }
    if(doc["sleepDeepAfterMinutes"].is<int>()) {
      config.sleepDeepAfterMinutes = doc["sleepDeepAfterMinutes"].as<int>();
      if(config.sleepDeepAfterMinutes < MIN_SLEEP_DEEP_AFTER_MINUTES) config.sleepDeepAfterMinutes = MIN_SLEEP_DEEP_AFTER_MINUTES;
      if(config.sleepDeepAfterMinutes > MAX_SLEEP_DEEP_AFTER_MINUTES) config.sleepDeepAfterMinutes = MAX_SLEEP_DEEP_AFTER_MINUTES;
    }
    pruneMissingFacesFromArray(sleepEnterEvents, sleepEnterEventCount);
    pruneMissingFacesFromArray(sleepWakeEvents, sleepWakeEventCount);
    config.sleepEnterFace = sleepEnterEventCount > 0 ? sleepEnterEvents[0] : "";
    config.sleepWakeFace = sleepWakeEventCount > 0 ? sleepWakeEvents[0] : "";
    if(doc["wifiAutoConnect"].is<bool>()) config.wifiAutoConnect = doc["wifiAutoConnect"].as<bool>();
    if(doc["wifiSSID"].is<String>()) config.wifiSSID = doc["wifiSSID"].as<String>();
    if(doc["speedMultiplier"].is<float>()) speedMultiplier = doc["speedMultiplier"].as<float>();
    if(speedMultiplier < 0.3f) speedMultiplier = 0.3f;
    if(speedMultiplier > 3.0f) speedMultiplier = 3.0f;
    if(doc["randomEventMinSec"].is<int>()) randomEventMinMs = (unsigned long)doc["randomEventMinSec"].as<int>() * 1000UL;
    if(doc["randomEventMaxSec"].is<int>()) randomEventMaxMs = (unsigned long)doc["randomEventMaxSec"].as<int>() * 1000UL;
    if(randomEventMinMs < 5000UL) randomEventMinMs = 5000UL;
    if(randomEventMinMs > 120000UL) randomEventMinMs = 120000UL;
    if(randomEventMaxMs > 120000UL) randomEventMaxMs = 120000UL;
    if(randomEventMaxMs < randomEventMinMs) randomEventMaxMs = randomEventMinMs;
    if(doc["idleEventMinSec"].is<int>()) idleEventMinMs = (unsigned long)doc["idleEventMinSec"].as<int>() * 1000UL;
    if(doc["idleEventMaxSec"].is<int>()) idleEventMaxMs = (unsigned long)doc["idleEventMaxSec"].as<int>() * 1000UL;
    if(idleEventMinMs < 1000UL) idleEventMinMs = 1000UL;
    if(idleEventMinMs > 120000UL) idleEventMinMs = 120000UL;
    if(idleEventMaxMs > 120000UL) idleEventMaxMs = 120000UL;
    if(idleEventMaxMs < idleEventMinMs) idleEventMaxMs = idleEventMinMs;
    
    if(doc["idleList"].is<JsonArray>()) {
      idleAnimCount = 0;
      for(JsonVariant v : doc["idleList"].as<JsonArray>()) {
        if(idleAnimCount >= MAX_IDLE_LIST) break;
        if(v.is<JsonObject>()) {
          idleAnims[idleAnimCount].name = v["name"].as<String>();
          idleAnims[idleAnimCount].loops = v["loops"] | 1;
          idleAnims[idleAnimCount].seconds = v["seconds"] | 0;
        } else {
          idleAnims[idleAnimCount].name = v.as<String>();
          idleAnims[idleAnimCount].loops = 1;
          idleAnims[idleAnimCount].seconds = 0;
        }
        if(idleAnims[idleAnimCount].loops < IDLE_LOOPS_MIN) idleAnims[idleAnimCount].loops = IDLE_LOOPS_MIN;
        if(idleAnims[idleAnimCount].loops > IDLE_LOOPS_MAX) idleAnims[idleAnimCount].loops = IDLE_LOOPS_MAX;
        if(idleAnims[idleAnimCount].seconds < IDLE_SECONDS_MIN) idleAnims[idleAnimCount].seconds = IDLE_SECONDS_MIN;
        if(idleAnims[idleAnimCount].seconds > IDLE_SECONDS_MAX) idleAnims[idleAnimCount].seconds = IDLE_SECONDS_MAX;
        if(idleAnims[idleAnimCount].name.length() > 0) idleAnimCount++;
      }
    }

    if(doc["faceCategories"].is<JsonObject>()) {
      faceCategoryCount = 0;
      JsonObject cats = doc["faceCategories"].as<JsonObject>();
      for(JsonPair kv : cats) {
        if(faceCategoryCount >= 80) break;
        faceCategoryNames[faceCategoryCount] = kv.key().c_str();
        faceCategoryValues[faceCategoryCount] = kv.value().as<String>();
        faceCategoryCount++;
      }
    }
    
    if(doc["pokeReactions"].is<JsonArray>()) {
      pokeReactionCount = 0;
      for(JsonVariant v : doc["pokeReactions"].as<JsonArray>()) {
        if(pokeReactionCount < 50) pokeReactions[pokeReactionCount++] = v.as<String>();
      }
    }
    
    if(doc["pettingReactions"].is<JsonArray>()) {
      pettingReactionCount = 0;
      for(JsonVariant v : doc["pettingReactions"].as<JsonArray>()) {
        if(pettingReactionCount < 50) pettingReactions[pettingReactionCount++] = v.as<String>();
      }
    }
    
    if(doc["deepPettingReactions"].is<JsonArray>()) {
      deepPettingReactionCount = 0;
      for(JsonVariant v : doc["deepPettingReactions"].as<JsonArray>()) {
        if(deepPettingReactionCount < 50) deepPettingReactions[deepPettingReactionCount++] = v.as<String>();
      }
    }

    if(doc["openingEvents"].is<JsonArray>()) {
      openingEventCount = 0;
      for(JsonVariant v : doc["openingEvents"].as<JsonArray>()) {
        if(openingEventCount < 50) openingEvents[openingEventCount++] = v.as<String>();
      }
    }

    if(doc["randomEvents"].is<JsonArray>()) {
      randomEventCount = 0;
      for(JsonVariant v : doc["randomEvents"].as<JsonArray>()) {
        if(randomEventCount < 50) randomEvents[randomEventCount++] = v.as<String>();
      }
    }

    if(doc["sleepEnterEvents"].is<JsonArray>()) {
      sleepEnterEventCount = 0;
      for(JsonVariant v : doc["sleepEnterEvents"].as<JsonArray>()) {
        if(sleepEnterEventCount < 50) sleepEnterEvents[sleepEnterEventCount++] = v.as<String>();
      }
    }

    if(doc["sleepWakeEvents"].is<JsonArray>()) {
      sleepWakeEventCount = 0;
      for(JsonVariant v : doc["sleepWakeEvents"].as<JsonArray>()) {
        if(sleepWakeEventCount < 50) sleepWakeEvents[sleepWakeEventCount++] = v.as<String>();
      }
    }

    // ivmeolcer config POST isle
    if(doc["accel"].is<JsonObject>()) {
      JsonObject a = doc["accel"].as<JsonObject>();
      if(a["enabled"].is<bool>()) accelEnabled = a["enabled"].as<bool>();
      if(a["softShakeThreshold"].is<float>()) {
        accelSoftThreshold = a["softShakeThreshold"].as<float>();
        if(accelSoftThreshold < 0.1f) accelSoftThreshold = 0.1f;
        if(accelSoftThreshold > 3.0f) accelSoftThreshold = 3.0f;
      }
      if(a["hardShakeThreshold"].is<float>()) {
        accelHardThreshold = a["hardShakeThreshold"].as<float>();
        if(accelHardThreshold < 0.5f) accelHardThreshold = 0.5f;
        if(accelHardThreshold > 8.0f) accelHardThreshold = 8.0f;
      }
      if(a["accelerationThreshold"].is<float>()) {
        accelAccelThreshold = a["accelerationThreshold"].as<float>();
        if(accelAccelThreshold < 0.1f) accelAccelThreshold = 0.1f;
        if(accelAccelThreshold > 5.0f) accelAccelThreshold = 5.0f;
      }
      if(a["freefallDuration"].is<int>()) {
        accelFreefallDurMs = (unsigned long)a["freefallDuration"].as<int>();
        if(accelFreefallDurMs < 50UL)   accelFreefallDurMs = 50UL;
        if(accelFreefallDurMs > 1000UL) accelFreefallDurMs = 1000UL;
      }
      if(a["pickupInactivityMs"].is<int>()) {
        accelPickupInactivityMs = (unsigned long)a["pickupInactivityMs"].as<int>();
        if(accelPickupInactivityMs < 5000UL)   accelPickupInactivityMs = 5000UL;
        if(accelPickupInactivityMs > 120000UL) accelPickupInactivityMs = 120000UL;
      }
      if(a["softShakeReactions"].is<JsonArray>()) {
        accelSoftShakeCount = 0;
        loadStringArrayFromJson(a, "softShakeReactions", accelSoftShakeReactions, accelSoftShakeCount, MAX_REACTION_LIST);
      }
      if(a["hardShakeReactions"].is<JsonArray>()) {
        accelHardShakeCount = 0;
        loadStringArrayFromJson(a, "hardShakeReactions", accelHardShakeReactions, accelHardShakeCount, MAX_REACTION_LIST);
      }
      if(a["accelerationReactions"].is<JsonArray>()) {
        accelAccelCount = 0;
        loadStringArrayFromJson(a, "accelerationReactions", accelAccelReactions, accelAccelCount, MAX_REACTION_LIST);
      }
      if(a["freefallReactions"].is<JsonArray>()) {
        accelFreefallCount = 0;
        loadStringArrayFromJson(a, "freefallReactions", accelFreefallReactions, accelFreefallCount, MAX_REACTION_LIST);
      }
      if(a["orientationReactions"].is<JsonArray>()) {
        accelOrientationCount = 0;
        loadStringArrayFromJson(a, "orientationReactions", accelOrientationReactions, accelOrientationCount, MAX_REACTION_LIST);
      }
      if(a["pickupReactions"].is<JsonArray>()) {
        accelPickupCount = 0;
        loadStringArrayFromJson(a, "pickupReactions", accelPickupReactions, accelPickupCount, MAX_REACTION_LIST);
      }
      // Esik degistiyse sensor donanimini yeni soft esikle guncelle
      applyAccelHardwareConfig(accelSoftThreshold);
    }

    sanitizeConfigAgainstFiles(false);
    resetRandomHistories();


    if(mood == MOOD_IDLE) {
      // Idle ara animasyonlari arayuzden degistiginde ana/default yuze don
      // ve rastgele ara idle planini yenile.
      idleEventPlaying = false;
      nextIdleEventAt = 0;
      currentIdleStartTime = 0;
      currentIdleDurationMs = 0;
      selectIdleNext();
      scheduleNextRandomEvent();
    }
    
    saveConfig();
    server.send(200, "text/plain", "ok");
  }
}

void handleApiConfigExport() {
  lastWebActivityTime = millis();
  JsonDocument doc;
  doc["defaultFace"] = config.currentFace;
  doc["currentFace"] = config.currentFace;
  doc["wifiSSID"] = config.wifiSSID;
  doc["wifiAutoConnect"] = config.wifiAutoConnect;
  doc["wifiTimeoutMinutes"] = config.wifiTimeoutMinutes;
  doc["wifiTapCount"] = config.wifiTapCount;
  doc["wifiTapWindowSec"] = config.wifiTapWindowSec;
  doc["batteryTapCount"] = config.batteryTapCount;
  doc["batteryTapWindowSec"] = config.batteryTapWindowSec;
  doc["longPressMs"] = config.longPressMs;
  doc["deepPettingMs"] = config.deepPettingMs;
  doc["touchDebounceMs"] = config.touchDebounceMs;
  doc["displayBrightnessPercent"] = config.displayBrightnessPercent;
  doc["displayBrightnessContrast"] = displayBrightnessPercentToContrast(config.displayBrightnessPercent);
  doc["sleepEnabled"] = config.sleepEnabled;
  doc["sleepTimeoutMinutes"] = config.sleepTimeoutMinutes;
  doc["sleepEnterFace"] = config.sleepEnterFace;
  doc["sleepWakeFace"] = config.sleepWakeFace;
  // sleepEnterMs/sleepWakeMs yazma silindi.
  doc["sleepMode"] = config.sleepMode;
  doc["sleepDeepAfterMinutes"] = config.sleepDeepAfterMinutes;
  doc["speedMultiplier"] = speedMultiplier;
  doc["randomEventMinSec"] = randomEventMinMs / 1000UL;
  doc["randomEventMaxSec"] = randomEventMaxMs / 1000UL;
  doc["idleEventMinSec"] = idleEventMinMs / 1000UL;
  doc["idleEventMaxSec"] = idleEventMaxMs / 1000UL;

  JsonArray idleArr = doc["idleList"].to<JsonArray>();
  for(int i = 0; i < idleAnimCount; i++) {
    JsonObject o = idleArr.add<JsonObject>();
    o["name"] = idleAnims[i].name;
    o["loops"] = idleAnims[i].loops <= 0 ? 1 : idleAnims[i].loops;
    o["seconds"] = idleAnims[i].seconds < 0 ? 0 : idleAnims[i].seconds;
  }

  JsonObject cats = doc["faceCategories"].to<JsonObject>();
  for(int i = 0; i < faceCategoryCount; i++) cats[faceCategoryNames[i]] = faceCategoryValues[i];

  // 7 array dump pattern'i helper ile sadelestirildi.
  dumpStringArrayToJson(doc, "pokeReactions", pokeReactions, pokeReactionCount);
  dumpStringArrayToJson(doc, "pettingReactions", pettingReactions, pettingReactionCount);
  dumpStringArrayToJson(doc, "deepPettingReactions", deepPettingReactions, deepPettingReactionCount);
  dumpStringArrayToJson(doc, "openingEvents", openingEvents, openingEventCount);
  dumpStringArrayToJson(doc, "randomEvents", randomEvents, randomEventCount);
  dumpStringArrayToJson(doc, "sleepEnterEvents", sleepEnterEvents, sleepEnterEventCount);
  dumpStringArrayToJson(doc, "sleepWakeEvents", sleepWakeEvents, sleepWakeEventCount);

  // ivmeolcer config export'a ekle
  JsonObject aExp = doc["accel"].to<JsonObject>();
  aExp["enabled"]                = accelEnabled;
  aExp["softShakeThreshold"]     = accelSoftThreshold;
  aExp["hardShakeThreshold"]     = accelHardThreshold;
  aExp["accelerationThreshold"]  = accelAccelThreshold;
  aExp["freefallDuration"]       = (int)accelFreefallDurMs;
  aExp["pickupInactivityMs"]     = (int)accelPickupInactivityMs;
  dumpStringArrayToJson(aExp, "softShakeReactions",    accelSoftShakeReactions,   accelSoftShakeCount);
  dumpStringArrayToJson(aExp, "hardShakeReactions",    accelHardShakeReactions,   accelHardShakeCount);
  dumpStringArrayToJson(aExp, "accelerationReactions", accelAccelReactions,       accelAccelCount);
  dumpStringArrayToJson(aExp, "freefallReactions",     accelFreefallReactions,    accelFreefallCount);
  dumpStringArrayToJson(aExp, "orientationReactions",  accelOrientationReactions, accelOrientationCount);
  dumpStringArrayToJson(aExp, "pickupReactions",       accelPickupReactions,      accelPickupCount);

  String out;
  serializeJsonPretty(doc, out);
  server.sendHeader("Content-Disposition", "attachment; filename=mochi_config.json");
  server.send(200, "application/json", out);
}

void handleApiWifiStatus() {
  lastWebActivityTime = millis();
  JsonDocument doc;
  doc["apSSID"] = getAPSSID();
  doc["apClients"] = WiFi.softAPgetStationNum();
  doc["staConnected"] = (WiFi.status() == WL_CONNECTED);
  if(WiFi.status() == WL_CONNECTED) {
    doc["ip"] = WiFi.localIP().toString();
  } else {
    doc["ip"] = WiFi.softAPIP().toString();
  }
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleApiWifiScan() {
  lastWebActivityTime = millis();
  int n = WiFi.scanNetworks();
  
  JsonDocument doc;
  JsonArray arr = doc["networks"].to<JsonArray>();
  for(int i = 0; i < n; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
  }
  
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleApiWifiConnect() {
  lastWebActivityTime = millis();
  String body = server.arg("plain");
  JsonDocument doc;
  if(deserializeJson(doc, body)) {
    server.send(400, "text/plain", "json err");
    return;
  }
  
  config.wifiSSID = doc["ssid"].as<String>();
  config.wifiPassword = doc["password"].as<String>();
  config.wifiAutoConnect = doc["auto"] | false;
  saveConfig();

  // AP'yi kapatmadan STA'yi ekle. AP acilisinda AP_STA kullanmiyoruz; bu mod
  // yalnizca kullanici gercekten bir ağa baglanmak istediginde gerekli.
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(config.wifiSSID.c_str(), config.wifiPassword.c_str());
  server.send(200, "text/plain", "ok");
}

void handleApiSys() {
  lastWebActivityTime = millis();
  JsonDocument doc;
  doc["deviceName"] = DEVICE_NAME;
  doc["uptime"] = millis() / 1000;
  doc["freeRam"] = ESP.getFreeHeap();
  doc["wifiLowMemoryDisplayMode"] = false;
  doc["wifiSnapshotValid"] = false;
  doc["heapBeforeWiFiStart"] = heapBeforeWiFiStart;
  doc["heapAfterWiFiStart"] = heapAfterWiFiStart;
  // Heap fragmentation ve memory leak debug icin ek metrikler.
  // - maxAllocHeap: heap'te alinabilecek en buyuk tek blok (fragmentation gostergesi)
  // - minFreeHeap: bu boot boyunca gorulen en dusuk free heap (worst-case takip)
  doc["maxAllocHeap"] = ESP.getMaxAllocHeap();
  doc["minFreeHeap"] = ESP.getMinFreeHeap();
  doc["fsReady"] = littlefsReady;
  doc["fsFormattedThisBoot"] = littlefsFormattedThisBoot;
  doc["fsStatus"] = littlefsStatus;
  doc["fsUsed"] = littlefsReady ? LittleFS.usedBytes() : 0;
  doc["fsTotal"] = littlefsReady ? LittleFS.totalBytes() : 0;
  updateBatteryMonitor(false);
  doc["batteryPercent"] = batteryPercent;
  doc["batteryVoltage"] = batteryVoltage;
  doc["batteryCharging"] = batteryCharging;
  doc["batteryChargeDetect"] = "voltage_trend";
  doc["batteryAdcPin"] = PIN_BATTERY_ADC;
  doc["displayBrightnessPercent"] = config.displayBrightnessPercent;
  doc["displayBrightnessContrast"] = displayBrightnessPercentToContrast(config.displayBrightnessPercent);
  doc["sleeping"] = systemSleeping;
  doc["sleepEnabled"] = config.sleepEnabled;
  doc["sleepTimeoutMinutes"] = config.sleepTimeoutMinutes;
  doc["sleepMode"] = config.sleepMode;
  doc["sleepDeepAfterMinutes"] = config.sleepDeepAfterMinutes;
  doc["sleepForSec"] = systemSleeping ? (millis() - sleepEnteredAt) / 1000 : 0;
  // En son animasyon yukleme/parse hatasi (varsa). UI debug overlay'inde
  // gosterilebilir. Bos = hata yok.
  doc["lastAnimError"] = lastAnimError;
  doc["lastAnimErrorAgoSec"] = lastAnimErrorAt > 0 ? (millis() - lastAnimErrorAt) / 1000 : 0;
  // Config yukleme durumu. UI bu degere gore "config bozuldu, .bak'tan
  // geri yuklendi - kontrol et" gibi uyari gosterebilir.
  doc["configStatus"] = configStatus;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleApiBattery() {
  lastWebActivityTime = millis();
  updateBatteryMonitor(false);
  JsonDocument doc;
  doc["percent"] = batteryPercent;
  doc["voltage"] = batteryVoltage;
  doc["charging"] = batteryCharging;
  doc["chargeDetect"] = "voltage_trend";
  doc["adcPin"] = PIN_BATTERY_ADC;
  doc["divider"] = "330k/330k";
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleApiScript() {
  lastWebActivityTime = millis();

  if(!littlefsReady) {
    server.send(503, "text/plain", littlefsStatus);
    return;
  }
  
  if(LittleFS.exists("/gif_to_bin.py")) {
    File f = LittleFS.open("/gif_to_bin.py", "r");
    if(f) {
      server.sendHeader("Content-Disposition", "attachment; filename=gif_to_bin.py");
      server.streamFile(f, "text/x-python");
      f.close();
      return;
    }
  }
  server.send(404, "text/plain", "Script not found - not uploaded to LittleFS");
}

void handleApiPing() {
  lastWebActivityTime = millis();
  server.send(200, "text/plain", "ok");
}

void handleApiRestart() {
  server.send(200, "text/plain", "ok");
  delay(200);
  ESP.restart();
}

// Sleep test endpoint. Kullanici UI'dan butona basinca sleep timeout'unu
// beklemeden hemen uyku transition'ini tetikleyebilir. Sleep enabled/disabled
// kontrolu burada yapilmiyor; "test" oldugu icin enabled olmasa bile
// gostermek istiyoruz - DOKUNMA ile uyaniyor.
void handleApiSleepTest() {
  lastWebActivityTime = millis();
  // 200ms sonra uykuya gec ki HTTP cevabi onceden gitsin (server.stop ile beraber WiFi kapanacak).
  server.send(200, "text/plain", "ok - sleep in 1s");
  delay(1000);
  // checkSleepMode tasarladigimiz akisla mod'a gore light/deep secimi yapar.
  // Aktiviteyi simulasyon icin geriye al ki checkSleepMode hemen tetiklensin.
  lastInteractionTime = 0;
  checkSleepMode();
}


void handleApiTest() {
  lastWebActivityTime = millis();

  // name parametresi UI'da o an isaretli olan animasyonu kaydetmeden test etmek icin kullanilir.
  // Yoksa kind kategorisinin kayitli listesinden rastgele secilir.
  String kind = server.hasArg("kind") ? server.arg("kind") : "active";
  String forcedName = server.hasArg("name") ? sanitizeFaceName(server.arg("name")) : "";

  if(forcedName.length() > 0) {
    if(!faceFileExists(forcedName)) {
      Serial.printf("[TEST] face not found name=%s kind=%s\n", forcedName.c_str(), kind.c_str());
      server.send(404, "text/plain", "face not found: " + forcedName);
      return;
    }
    if(startTestAnimation(forcedName)) {
      server.send(200, "text/plain", "ok");
    } else {
      server.send(500, "text/plain", "load failed or too large during WiFi");
    }
    return;
  }

  if(startTestAnimation(kind)) {
    server.send(200, "text/plain", "ok");
  } else {
    server.send(404, "text/plain", "no animation for kind: " + kind);
  }
}


// ============================================================
// IVMEOLCER (MMA8452Q) IMPLEMENTASYONU
// ============================================================

// ISR'lar — IRAM_ATTR zorunlu; ISR'da sadece flag set et
void IRAM_ATTR accelISR_INT1() { accelInt1Fired = true; }
void IRAM_ATTR accelISR_INT2() { accelInt2Fired = true; }

// ivmeolcer event oynatmaya uygun mu?
bool accelEventAllowed() {
  if(!accelEnabled || !accelReady) return false;
  // PETTING / DEEP_PETTING sirasinda hic bir ivmeolcer event calikmasin
  if(mood == MOOD_PETTING || mood == MOOD_DEEP_PETTING) return false;
  // Dokunma tepkisi oynarken de bekle
  if(mood == MOOD_POKED) return false;
  return true;
}

// Bir ivmeolcer kategorisi icin animasyon yukle ve MOOD_ACCEL_EVENT'e gec
// list/count: reaction listesi; history/histCnt: gecmis buffer;
// cooldownEnd: bu kategori icin cooldown bitis zamani pointer'i; cooldownMs: cooldown suresi
bool triggerAccelReaction(String* list, int count,
                           String* history, int& histCnt,
                           unsigned long* cooldownEnd, unsigned long cooldownMs) {
  unsigned long now = millis();
  String name = wifiActive
    ? getRandomWifiSafeReactionWithHistory(list, count, history, histCnt, ACCEL_EVENT_OPTIONS.maxFramesDuringWifi)
    : getRandomReactionWithHistory(list, count, history, histCnt);

  if(!playEventFaceSafe(name, MOOD_ACCEL_EVENT, EVENT_ACCEL, ACCEL_EVENT_OPTIONS)) {
    return false;
  }

  *cooldownEnd = now + cooldownMs;
  lastInteractionTime = now;  // fiziksel etkilesim: uyku zamanlayicisini sifirla
  enterMood(MOOD_ACCEL_EVENT);
  return true;
}

// INT pinleri floating/polarite/latch yuzunden firtina yaptiginda kullanilan
// guvenli yazilimsal fallback. MMA8452Q hâlâ I2C uzerinden okunur; sadece INT
// hatlarina guvenmeyiz. Sallama algisi icin iki sinyali beraber kullanir:
// 1) |g|-1 dinamik buyukluk, 2) ardisik vektor farki. Bu, yatay sallamada
// toplam |g| 1'e yakin kalsa bile hareketi yakalamaya yardim eder.
void handleAccelPollingFallback() {
  unsigned long now = millis();
  if(!accelEventAllowed()) return;
  if(systemSleeping) return;  // polling uykuda calismaz; wake icin INT gerekir

  static unsigned long lastPollMs = 0;
  static float lastX = 0.0f, lastY = 0.0f, lastZ = 1.0f;
  static bool havePrev = false;

  if(now - lastPollMs < ACCEL_POLL_FALLBACK_INTERVAL_MS) return;
  lastPollMs = now;

  accel.read();
  float x = accel.cx;
  float y = accel.cy;
  float z = accel.cz;
  float mag = sqrt(x*x + y*y + z*z);
  float dynMag = fabs(mag - 1.0f);

  float deltaMag = 0.0f;
  if(havePrev) {
    float dx = x - lastX;
    float dy = y - lastY;
    float dz = z - lastZ;
    deltaMag = sqrt(dx*dx + dy*dy + dz*dz);
  }
  havePrev = true;
  lastX = x; lastY = y; lastZ = z;

  // Dokunma kaynakli mekanik tiklar sallama event'i sayilmasin.
  bool touchArtifactWindow = readTouchActive() || (now - touchStartTime < 1500UL);
  if(touchArtifactWindow) return;

  // Devam eden event varsa yeni event yukleyip animasyonu kesme.
  if(mood == MOOD_ACCEL_EVENT || mood == MOOD_RANDOM_EVENT || mood == MOOD_TEST_EVENT) return;

  float score = (dynMag > deltaMag) ? dynMag : deltaMag;
  static unsigned long lastPollDebugMs = 0;
  if(score >= 0.20f && now - lastPollDebugMs >= 800UL) {
    Serial.printf("[ACCEL-POLL] score=%.2f dyn=%.2f delta=%.2f counts(s/h/a)=%d/%d/%d en=%d ready=%d\n",
                  score, dynMag, deltaMag, accelSoftShakeCount, accelHardShakeCount,
                  accelAccelCount, (int)accelEnabled, (int)accelReady);
    lastPollDebugMs = now;
  }

  // Sert -> acceleration -> soft onceligi. Cooldown ve liste bos kontrolu korunur.
  if(score >= accelHardThreshold && now >= accelCooldownHardEnd && accelHardShakeCount > 0) {
    Serial.printf("[ACCEL-POLL] hard score=%.2f dyn=%.2f delta=%.2f\n", score, dynMag, deltaMag);
    triggerAccelReaction(accelHardShakeReactions, accelHardShakeCount,
                          recentAccelHardHistory, recentAccelHardHistoryCount,
                          &accelCooldownHardEnd, ACCEL_COOLDOWN_HARD_MS);
    return;
  }

  if(score >= accelAccelThreshold && score < accelHardThreshold &&
     now >= accelCooldownAccelEnd && accelAccelCount > 0) {
    Serial.printf("[ACCEL-POLL] acceleration score=%.2f dyn=%.2f delta=%.2f\n", score, dynMag, deltaMag);
    triggerAccelReaction(accelAccelReactions, accelAccelCount,
                          recentAccelAccelHistory, recentAccelAccelHistoryCount,
                          &accelCooldownAccelEnd, ACCEL_COOLDOWN_ACCEL_MS);
    return;
  }

  if(score >= accelSoftThreshold && score < accelAccelThreshold &&
     now >= accelCooldownSoftEnd && accelSoftShakeCount > 0) {
    Serial.printf("[ACCEL-POLL] soft score=%.2f dyn=%.2f delta=%.2f\n", score, dynMag, deltaMag);
    triggerAccelReaction(accelSoftShakeReactions, accelSoftShakeCount,
                          recentAccelSoftHistory, recentAccelSoftHistoryCount,
                          &accelCooldownSoftEnd, ACCEL_COOLDOWN_SOFT_MS);
    return;
  }
}

// Her loop iterasyonunda cagrilir — interrupt bayraklarini isleme koydurur.
// Tasarim: TRANSIENT (high-pass, INT1) tek "sallanma" kaynagi; event geldiginde
// anlik dinamik ivmeye gore soft/acceleration/hard olarak siniflandirilir.
// PL/orientation INT2'den gelir. Freefall yazilimsal (throttled magnitude).
void handleAccelEvents() {
  unsigned long now = millis();

  // INT hatlari firtina nedeniyle devreden alindiysa sensörü kapatma;
  // yazilimsal polling fallback ile sallama tepkilerini uretmeye devam et.
  if(accelInterruptsSuspended) {
    handleAccelPollingFallback();
    return;
  }

  // ISR FIRTINA KORUMASI: INT pini donanimsal bir sorunla (floating, yanlis
  // polarite, kablo, sensor arizasi) surekli tetikleniyorsa ISR CPU'yu bogar;
  // dokunma debounce'i hic oturmaz ve TUM gesture'lar olur (tam da "ne wifi
  // ne pil calisiyor" tablosu). Saniyede 30+ event fizyolojik degil —
  // interrupt'lari sok, sensoru kapat, Serial'a raporla.
  static unsigned long isrWindowStart = 0;
  static uint16_t isrWindowCount = 0;
  if(accelInt1Fired || accelInt2Fired) isrWindowCount++;
  if(now - isrWindowStart >= 1000UL) {
    if(isrWindowCount > 30) {
      detachInterrupt(digitalPinToInterrupt(PIN_ACCEL_INT1));
      detachInterrupt(digitalPinToInterrupt(PIN_ACCEL_INT2));
      accelInt1Fired = false;
      accelInt2Fired = false;
      accelInterruptsSuspended = true;
      // accelEnabled=false yapma: aksi halde handleAccelEvents tamamen kapanir ve
      // sallama yuzleri hic oynayamaz. Sadece INT hatlarini birakip I2C polling'e gec.
      Serial.printf("[ACCEL] INT firtinasi tespit edildi (%u/sn)! INT hatlari kapatildi, I2C polling fallback aktif.\n", isrWindowCount);
      Serial.printf("[ACCEL] Pin seviyeleri: INT1(GPIO%d)=%d INT2(GPIO%d)=%d\n",
                    PIN_ACCEL_INT1, digitalRead(PIN_ACCEL_INT1),
                    PIN_ACCEL_INT2, digitalRead(PIN_ACCEL_INT2));
      Serial.println("[ACCEL] Not: Sleep'ten sallamayla uyandirma INT'siz calismaz; aktif modda sallama yuzleri calisir.");
      handleAccelPollingFallback();
      return;
    }
    isrWindowStart = now;
    isrWindowCount = 0;
  }

  if(!accelEventAllowed()) {
    // Bayraklari ve donanim latch'lerini temizle ki birikmesin.
    // accelHardWakePending de dusurulur: uyanista baska bir mod (petting vb.)
    // oncelik aldiysa gecikmis hard-shake animasyonu dakikalar sonra oynamasin.
    accelHardWakePending = false;
    if(accelInt1Fired || accelInt2Fired) {
      accelInt1Fired = false;
      accelInt2Fired = false;
      if(accelReady) {
        accelReadReg(ACCEL_REG_TRANS_SRC);
        accelReadReg(ACCEL_REG_PL_STATUS);
      }
    }
    return;
  }

  // ── Light sleep'ten sert sallanma ile uyanildi: siniflandirmasiz hard tetikle.
  //    (Uykuda esik hard'a yukseltilmisti; latch kesin hard eventten geldi.)
  if(accelHardWakePending) {
    accelHardWakePending = false;
    accelInt1Fired = false;
    accelReadReg(ACCEL_REG_TRANS_SRC);  // latch temizle
    if(now >= accelCooldownHardEnd && accelHardShakeCount > 0 &&
       mood != MOOD_ACCEL_EVENT && mood != MOOD_RANDOM_EVENT) {
      triggerAccelReaction(accelHardShakeReactions, accelHardShakeCount,
                            recentAccelHardHistory, recentAccelHardHistoryCount,
                            &accelCooldownHardEnd, ACCEL_COOLDOWN_HARD_MS);
    }
    return;
  }

  bool int1 = accelInt1Fired; accelInt1Fired = false;
  bool int2 = accelInt2Fired; accelInt2Fired = false;

  // ── Throttled okuma: her loop degil, 50ms'de bir I2C'ye git.
  //    Onceki surum her loop accel.read() yapiyordu; senkron I2C okumasi
  //    animasyon loop'unu yavaslatip frame timing'i bozuyordu.
  //    Periyodik okuma sadece freefall tespiti icindir; freefall listesi bosken
  //    yalnizca interrupt aninda oku (I2C trafigini sifira indir).
  static unsigned long lastMagReadMs = 0;
  static float lastMag = 1.0f;
  bool needImmediateRead = int1;  // transient siniflandirmasi icin taze deger sart
  bool wantPeriodicRead = (accelFreefallCount > 0);
  if(needImmediateRead || (wantPeriodicRead && now - lastMagReadMs >= 50)) {
    accel.read();
    lastMag = sqrt(accel.cx * accel.cx + accel.cy * accel.cy + accel.cz * accel.cz);
    lastMagReadMs = now;
  }
  // Dinamik (yercekimsiz) ivme buyuklugu tahmini
  float dynMag = fabs(lastMag - 1.0f);

  // ── INT kaynaklarini oku ve latch'leri temizle ──
  bool srcTransient = false;
  bool srcOrient    = false;
  if(int1 || int2) {
    uint8_t intSrc = accelReadReg(ACCEL_REG_INT_SOURCE);
    srcTransient = intSrc & 0x20;  // SRC_TRANS
    srcOrient    = intSrc & 0x10;  // SRC_LNDPRT
    if(srcTransient) accelReadReg(ACCEL_REG_TRANS_SRC);  // latch temizle
    if(srcOrient)    accelLastPlStatus = accelReadReg(ACCEL_REG_PL_STATUS);  // latch temizle + UI icin sakla
  }

  // Dokunma darbeleri sensorde transient uretir (tiklama = fiziksel darbe).
  // Dokunma aktifken veya son basistan <1sn gecmisse transient'i tap artefakti
  // say ve sallanma siniflandirmasini atla. Aksi halde WiFi/pil icin hizli
  // tiklarken accel animasyonlari araya girip loadAnimation ile loop'u
  // blokluyor, tap sayaci basislari kaciriyor ve 10-tap WiFi gesture'i
  // hic dolmuyordu. (Latch yukarida zaten temizlendi; event kaybolmaz, yutulur.)
  bool touchArtifactWindow = readTouchActive() || (now - touchStartTime < 1500UL);

  // Pickup pattern: her transient hareketi son-hareket zamanini gunceller
  bool wasInactive = (accelLastMotionTime == 0 ||
                      now - accelLastMotionTime >= accelPickupInactivityMs);
  if(srcTransient) accelLastMotionTime = now;

  // ══════════════════════════════════════════════════════════════════
  // ONCELIK 1: Sert Sallanma (transient + dinamik ivme >= hard esik)
  //   Uyku modlarini da uyandirir.
  // ══════════════════════════════════════════════════════════════════
  if(srcTransient && !touchArtifactWindow && dynMag >= accelHardThreshold &&
     now >= accelCooldownHardEnd && accelHardShakeCount > 0) {
    if(systemSleeping) wakeFromSleepMode();
    if(mood == MOOD_ACCEL_EVENT || mood == MOOD_RANDOM_EVENT) {
      enterMood(MOOD_IDLE);
    }
    triggerAccelReaction(accelHardShakeReactions, accelHardShakeCount,
                          recentAccelHardHistory, recentAccelHardHistoryCount,
                          &accelCooldownHardEnd, ACCEL_COOLDOWN_HARD_MS);
    return;
  }

  // Uyku modundaysa hard shake disinda hicbir sey isleme
  if(systemSleeping) return;

  // Devam eden event varsa bekle
  if(mood == MOOD_ACCEL_EVENT || mood == MOOD_RANDOM_EVENT || mood == MOOD_TEST_EVENT) return;

  // ══════════════════════════════════════════════════════════════════
  // ONCELIK 2: Serbest Dusme (yazilimsal: |g| < 0.2 belli sure)
  // ══════════════════════════════════════════════════════════════════
  if(lastMag < 0.2f) {
    if(!accelInFreefall) {
      accelInFreefall = true;
      accelFreefallStartMs = now;
    } else if(now - accelFreefallStartMs >= accelFreefallDurMs) {
      if(now >= accelCooldownFreefallEnd && accelFreefallCount > 0) {
        accelInFreefall = false;
        triggerAccelReaction(accelFreefallReactions, accelFreefallCount,
                              recentAccelFreefallHistory, recentAccelFreefallHistoryCount,
                              &accelCooldownFreefallEnd, ACCEL_COOLDOWN_FREEFALL_MS);
        return;
      }
    }
  } else {
    accelInFreefall = false;
  }

  // ══════════════════════════════════════════════════════════════════
  // ONCELIK 3: Hizlanma (transient + accel esigi <= dyn < hard esigi)
  // ══════════════════════════════════════════════════════════════════
  // NOT: lastMag < 0.35 (dusme suphesi) iken siniflandirma yapma — serbest dusmede
  // dynMag ~1.0g cikar ve dusme animasyonu yerine yanlislikla "hizlanma" oynardi.
  if(srcTransient && !touchArtifactWindow && lastMag >= 0.35f &&
     dynMag >= accelAccelThreshold && dynMag < accelHardThreshold &&
     now >= accelCooldownAccelEnd && accelAccelCount > 0) {
    triggerAccelReaction(accelAccelReactions, accelAccelCount,
                          recentAccelAccelHistory, recentAccelAccelHistoryCount,
                          &accelCooldownAccelEnd, ACCEL_COOLDOWN_ACCEL_MS);
    return;
  }

  // ══════════════════════════════════════════════════════════════════
  // ONCELIK 4: Devrilme / Orientation (PL, INT2)
  // ══════════════════════════════════════════════════════════════════
  if(srcOrient) {
    if(accelPickupMotionSeen) accelPickupOrientSeen = true;  // pickup step 3
    if(!touchArtifactWindow && now >= accelCooldownOrientEnd && accelOrientationCount > 0) {
      triggerAccelReaction(accelOrientationReactions, accelOrientationCount,
                            recentAccelOrientHistory, recentAccelOrientHistoryCount,
                            &accelCooldownOrientEnd, ACCEL_COOLDOWN_ORIENT_MS);
      return;
    }
  }

  // ══════════════════════════════════════════════════════════════════
  // ONCELIK 5: Elime Alindi (hareketsizlik -> hareket -> egme)
  // ══════════════════════════════════════════════════════════════════
  if(accelPickupCount > 0 && now >= accelCooldownPickupEnd) {
    if(wasInactive && srcTransient && !touchArtifactWindow && !accelPickupMotionSeen) {
      accelPickupMotionSeen = true;   // step 2: hareketsizlik sonrasi hareket
      accelPickupOrientSeen = false;
    }
    if(accelPickupMotionSeen && accelPickupOrientSeen && !touchArtifactWindow) {
      accelPickupMotionSeen = false;
      accelPickupOrientSeen = false;
      triggerAccelReaction(accelPickupReactions, accelPickupCount,
                            recentAccelPickupHistory, recentAccelPickupHistoryCount,
                            &accelCooldownPickupEnd, ACCEL_COOLDOWN_PICKUP_MS);
      return;
    }
    // Pickup penceresi: motion'dan sonra 3sn icinde egme gelmezse sifirla
    if(accelPickupMotionSeen && now - accelLastMotionTime > 3000UL) {
      accelPickupMotionSeen = false;
      accelPickupOrientSeen = false;
    }
  }

  // ══════════════════════════════════════════════════════════════════
  // ONCELIK 6: Yumusak Sallanma (transient + soft esigi, IDLE'da)
  // ══════════════════════════════════════════════════════════════════
  if(srcTransient && !touchArtifactWindow && mood == MOOD_IDLE && lastMag >= 0.35f &&
     dynMag >= accelSoftThreshold && dynMag < accelAccelThreshold &&
     now >= accelCooldownSoftEnd && accelSoftShakeCount > 0) {
    triggerAccelReaction(accelSoftShakeReactions, accelSoftShakeCount,
                          recentAccelSoftHistory, recentAccelSoftHistoryCount,
                          &accelCooldownSoftEnd, ACCEL_COOLDOWN_SOFT_MS);
  }
}

// setup() icinde loadConfig() sonrasinda cagrilis
uint8_t accelReadReg(uint8_t reg) {
  Wire.beginTransmission(ACCEL_I2C_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)ACCEL_I2C_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}

void accelWriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ACCEL_I2C_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

// Interrupt donanim konfigurasyonu. transientThresholdG: bu esigin ustundeki
// dinamik (yercekimi haric, high-pass) ivmeler INT1 uretir. Normal calismada
// en dusuk esik (soft) verilir; deep sleep oncesi hard esige yukseltilir ki
// sadece sert sallanma uyandirsin.
void applyAccelHardwareConfig(float transientThresholdG) {
  if(!accelReady) return;

  // Standby'a al (config registerlari sadece standby'da yazilabilir)
  uint8_t ctrl1 = accelReadReg(ACCEL_REG_CTRL1);
  accelWriteReg(ACCEL_REG_CTRL1, ctrl1 & ~0x01);

  // TRANSIENT: high-pass filtreli — yercekimini otomatik cikarir, "sallanma"
  // tespiti icin dogru mod. ELE (latch) + X+Y+Z eksenleri. Latch sayesinde
  // event pini SRC register okunana kadar HIGH tutar (deep sleep wake icin de ideal).
  accelWriteReg(ACCEL_REG_TRANS_CFG, 0x1E);  // ELE|ZTEFE|YTEFE|XTEFE
  uint8_t ths = (uint8_t)(transientThresholdG / 0.063f + 0.5f);
  if(ths < 1) ths = 1;
  if(ths > 127) ths = 127;
  accelWriteReg(ACCEL_REG_TRANS_THS, ths);
  accelWriteReg(ACCEL_REG_TRANS_COUNT, 0);   // debounce yok, ilk asimda tetikle

  // PL (orientation): enable + ~100ms debounce (50Hz'de 5 sample)
  accelWriteReg(ACCEL_REG_PL_CFG, 0x40);     // PL_EN
  accelWriteReg(ACCEL_REG_PL_COUNT, 5);

  // IPOL=1: interrupt pinleri ACTIVE HIGH (RISING attach + deep sleep
  // GPIO_HIGH wake ile uyumlu). Default active-low idi — onceki kurulumda
  // interrupt hic yakalanamiyordu.
  accelWriteReg(ACCEL_REG_CTRL3, 0x02);

  // Interrupt enable: TRANSIENT + LNDPRT (orientation)
  accelWriteReg(ACCEL_REG_CTRL4, 0x30);      // INT_EN_TRANS|INT_EN_LNDPRT

  // Routing: TRANSIENT -> INT1 (GPIO2, deep sleep wake pini), LNDPRT -> INT2 (GPIO10)
  accelWriteReg(ACCEL_REG_CTRL5, 0x20);

  // Aktif moda geri don
  accelWriteReg(ACCEL_REG_CTRL1, ctrl1 | 0x01);

  // Olasi bekleyen latch'leri temizle
  accelReadReg(ACCEL_REG_TRANS_SRC);
  accelReadReg(ACCEL_REG_PL_STATUS);
}

void setupAccelerometer() {
  Wire.begin(PIN_ACCEL_SDA, PIN_ACCEL_SCL);
  if(!accel.begin(Wire, 0x1C)) {
    Serial.println("MMA8452Q bulunamadi! Kablo/adres kontrolu yapiniz.");
    Serial.println("  SDA=GPIO20, SCL=GPIO21, SA0->GND (addr 0x1C)");
    accelReady = false;
    return;
  }
  accelReady = true;
  accel.setScale(SCALE_2G);   // 2G: hassas olcum
  accel.setDataRate(ODR_50);  // 50Hz: yeterli; daha yuksek = daha fazla pil

  // Interrupt motoru: transient (sallanma) + PL (orientation) + IPOL active-high.
  // NOT: Onceki surumde bu konfigurasyon eksikti — INT pinleri hic sinyal
  // uretmiyordu, accel animasyonlari hic tetiklenmiyordu.
  applyAccelHardwareConfig(accelSoftThreshold);

  // Interrupt pinleri: PULLDOWN — IPOL=1 active-high; idle'da LOW garanti.
  pinMode(PIN_ACCEL_INT1, INPUT_PULLDOWN);
  pinMode(PIN_ACCEL_INT2, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(PIN_ACCEL_INT1), accelISR_INT1, RISING);
  attachInterrupt(digitalPinToInterrupt(PIN_ACCEL_INT2), accelISR_INT2, RISING);

  // Boot bastirma: cihaz acilirken elde tutuluyor / masaya konuyor olabilir;
  // PL ilk yonelim tespitini, transient de bu hareketi sahte event olarak uretir.
  // Ilk 3 saniye tum kategorileri cooldown'da tut.
  unsigned long bootSuppress = millis() + 3000UL;
  accelCooldownSoftEnd     = bootSuppress;
  accelCooldownHardEnd     = bootSuppress;
  accelCooldownAccelEnd    = bootSuppress;
  accelCooldownFreefallEnd = bootSuppress;
  accelCooldownOrientEnd   = bootSuppress;
  accelCooldownPickupEnd   = bootSuppress;

  Serial.println("MMA8452Q hazir (GPIO20=SDA, GPIO21=SCL, INT1=GPIO2, INT2=GPIO10)");
  Serial.printf("  Transient esik: %.2fg, PL debounce: 100ms\n", accelSoftThreshold);
}

// ─── IVMEOLCER WEB API ─────────────────────────────────────

// POST /api/test-accel?type=soft_shake  vb.
void handleApiTestAccel() {
  lastWebActivityTime = millis();

  // name parametresi: UI'daki O AN isaretli (henuz kaydedilmemis olabilir)
  // animasyonu dogrudan test et. Eskiden test yalnizca cihazdaki KAYITLI
  // listeden calisiyordu; kullanici secim yapip kaydetmeden Test'e basinca
  // handler 404 donuyordu — kafa karistiran davranis buydu.
  String reqName = server.hasArg("name") ? sanitizeFaceName(server.arg("name")) : "";
  if(reqName.length() > 0) {
    if(!faceFileExists(reqName)) {
      server.send(404, "text/plain", "face not found");
      return;
    }
    String lst[1] = { reqName };
    String hist[RANDOM_HISTORY_SIZE];
    int hc = 0;
    unsigned long dummy = 0;
    bool ok = triggerAccelReaction(lst, 1, hist, hc, &dummy, 0);
    server.send(ok ? 200 : 500, "text/plain", ok ? "ok" : "load failed");
    return;
  }

  String type = server.hasArg("type") ? server.arg("type") : "";
  String* list = nullptr; int count = 0;
  String* hist = nullptr; int* histCnt = nullptr;

  if(type == "soft_shake") {
    list = accelSoftShakeReactions; count = accelSoftShakeCount;
    hist = recentAccelSoftHistory;  histCnt = &recentAccelSoftHistoryCount;
  } else if(type == "hard_shake") {
    list = accelHardShakeReactions; count = accelHardShakeCount;
    hist = recentAccelHardHistory;  histCnt = &recentAccelHardHistoryCount;
  } else if(type == "acceleration") {
    list = accelAccelReactions; count = accelAccelCount;
    hist = recentAccelAccelHistory; histCnt = &recentAccelAccelHistoryCount;
  } else if(type == "freefall") {
    list = accelFreefallReactions; count = accelFreefallCount;
    hist = recentAccelFreefallHistory; histCnt = &recentAccelFreefallHistoryCount;
  } else if(type == "orientation") {
    list = accelOrientationReactions; count = accelOrientationCount;
    hist = recentAccelOrientHistory; histCnt = &recentAccelOrientHistoryCount;
  } else if(type == "pickup") {
    list = accelPickupReactions; count = accelPickupCount;
    hist = recentAccelPickupHistory; histCnt = &recentAccelPickupHistoryCount;
  }

  if(!list || count == 0) {
    server.send(404, "text/plain", "saved list is empty — pick an animation (you can test without saving)");
    return;
  }
  // Cooldown'u atlayarak zorla test et
  unsigned long dummyCooldown = 0;
  bool ok = triggerAccelReaction(list, count, hist, *histCnt, &dummyCooldown, 0);
  server.send(ok ? 200 : 500, "text/plain", ok ? "ok" : "load failed");
}

// GET /api/accel/live — canli sensor verisi (UI'da 5Hz refresh icin)
void handleApiAccelLive() {
  lastWebActivityTime = millis();
  JsonDocument doc;
  doc["ready"] = accelReady;
  if(accelReady) {
    accel.read();
    doc["x"] = accel.cx;
    doc["y"] = accel.cy;
    doc["z"] = accel.cz;
    float mag = sqrt(accel.cx * accel.cx + accel.cy * accel.cy + accel.cz * accel.cz);
    doc["magnitude"] = mag;
    // readPL() PL_STATUS okur ve orientation latch'ini TEMIZLERDI —
    // gercek interrupt kaynagi kaybolurdu. handleAccelEvents'in sakladigi
    // son degeri donduruyoruz; canli panel bilgi amaclidir.
    doc["orientation"] = (int)accelLastPlStatus;
    doc["inFreefall"] = accelInFreefall;
  }
  // Cooldown kalan sureleri
  unsigned long now = millis();
  JsonObject cd = doc["cooldownRemaining"].to<JsonObject>();
  cd["softShake"]    = accelCooldownSoftEnd     > now ? (int)(accelCooldownSoftEnd     - now) : 0;
  cd["hardShake"]    = accelCooldownHardEnd     > now ? (int)(accelCooldownHardEnd     - now) : 0;
  cd["acceleration"] = accelCooldownAccelEnd    > now ? (int)(accelCooldownAccelEnd    - now) : 0;
  cd["freefall"]     = accelCooldownFreefallEnd > now ? (int)(accelCooldownFreefallEnd - now) : 0;
  cd["orientation"]  = accelCooldownOrientEnd   > now ? (int)(accelCooldownOrientEnd   - now) : 0;
  cd["pickup"]       = accelCooldownPickupEnd   > now ? (int)(accelCooldownPickupEnd   - now) : 0;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/faces", HTTP_GET, handleApiFaces);
  server.on("/api/preview", HTTP_GET, handleApiPreview);
  server.on("/api/setactive", HTTP_POST, handleApiSetActive);
  server.on("/api/delete", HTTP_POST, handleApiDelete);
  server.on("/api/deleteall", HTTP_POST, handleApiDeleteAll);
  server.on("/api/upload", HTTP_POST, handleUploadComplete, handleUpload);
  server.on("/api/config", HTTP_GET, handleApiConfig);
  server.on("/api/config", HTTP_POST, handleApiConfig);
  server.on("/api/config/export", HTTP_GET, handleApiConfigExport);
  server.on("/api/test", HTTP_POST, handleApiTest);
  server.on("/api/wifi/status", HTTP_GET, handleApiWifiStatus);
  server.on("/api/wifi/scan", HTTP_GET, handleApiWifiScan);
  server.on("/api/wifi/connect", HTTP_POST, handleApiWifiConnect);
  server.on("/api/sys", HTTP_GET, handleApiSys);
  server.on("/api/battery", HTTP_GET, handleApiBattery);
  server.on("/api/script", HTTP_GET, handleApiScript);
  server.on("/api/ping", HTTP_GET, handleApiPing);
  server.on("/api/restart", HTTP_POST, handleApiRestart);
  server.on("/api/sleeptest", HTTP_POST, handleApiSleepTest);
  // ivmeolcer test ve canli veri endpoint'leri
  server.on("/api/test-accel", HTTP_POST, handleApiTestAccel);
  server.on("/api/accel/live", HTTP_GET, handleApiAccelLive);
  
  server.onNotFound([]() {
    lastWebActivityTime = millis();
    server.send(404, "text/plain", "Not found");
  });
}

// ============================================================
// LITTLEFS INIT
// ============================================================
static bool mountMochiLittleFS(bool formatOnFail) {
  // partitions.csv'de bu bolumun:
  //   Name  : "storage" (label - LittleFS.begin son parametresi bu olmali)
  //   Type  : data
  //   SubType: spiffs (Espressif tablosunda LittleFS icin ayri subType yok;
  //                    LittleFS surucusuyle mount edilince fark etmez)
  // formatOnFail=false'la once mevcut yuzleri korumayi dene; basarisizsa
  // initLittleFS bizi format-on-fail moduyla tekrar cagiriyor.
  return LittleFS.begin(formatOnFail, "/littlefs", 10, "storage");
}

bool initLittleFS() {
  littlefsReady = false;
  littlefsFormattedThisBoot = false;
  littlefsStatus = "mounting";

  // Once format atmadan dene. Normal sketch yuklemelerinde yuzleri koruyan yol budur.
  if(mountMochiLittleFS(false) && LittleFS.totalBytes() > 0) {
    littlefsReady = true;
    littlefsStatus = "ready";
  } else {
    Serial.println("LittleFS mount olmadi veya toplam alan 0. Yeni partition sonrasi dosya sistemi formatlanacak.");
    littlefsStatus = "mount failed/0 KB; formatting";
    LittleFS.end();

    // Partition degistirdikten sonra dosya sistemi bos/formatlanmamis olur. 0/0 KB sorununu bu cozer.
    // Uyari: Bu yalnizca mount basarisizsa calisir; mevcut calisan yuzleri normal kod yuklemede silmez.
    if(mountMochiLittleFS(true) && LittleFS.totalBytes() > 0) {
      littlefsFormattedThisBoot = true;
      littlefsReady = true;
      littlefsStatus = "ready (formatted this boot)";
    } else {
      littlefsStatus = "format/mount failed - check partitions.csv/flash size";
    }
  }

  if(littlefsReady) {
    Serial.printf("LittleFS: %u/%u byte%s\n",
                  LittleFS.usedBytes(), LittleFS.totalBytes(),
                  littlefsFormattedThisBoot ? " (bu acilista formatlandi)" : "");
    if(!LittleFS.exists("/faces")) LittleFS.mkdir("/faces");
    return true;
  }

  Serial.printf("LittleFS HATA: %s\n", littlefsStatus.c_str());
  return false;
}

// ============================================================
// SETUP & LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  // KRITIK: Monitor bagli DEGILKEN (pilde calisirken) USB-CDC TX buffer'i dolar
  // ve her Serial.print varsayilan olarak ~100ms'ye kadar BLOKLAR — [DIAG] her
  // 5 sn'de, animasyon yukleme loglari her gecise ritmik takilmalar olarak
  // ekrana yansiyordu. Timeout 0: buffer doluysa yazma aninda dusurulur, loop
  // asla beklemez. Monitor bagliyken buffer surekli bosaldigi icin loglar
  // normal akmaya devam eder.
  // TESHIS: Native USB-CDC numaralandirmasi 1-2 sn surebilir; ilk printler
  // kaybolmasin diye sinirli bekleme. USB yokken (pil) en fazla 2 sn bekler.
  {
    unsigned long serialWaitStart = millis();
    while(!Serial && millis() - serialWaitStart < 2000UL) delay(10);
  }
  Serial.println("[BOOT] setup basladi");
  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  bool hadMochiDeepSleepMagic = (mochiDeepSleepMagic == MOCHI_DEEP_SLEEP_MAGIC);
  wokeFromMochiDeepSleep = hadMochiDeepSleepMagic &&
                           (wakeCause == ESP_SLEEP_WAKEUP_GPIO ||
                            wakeCause == ESP_SLEEP_WAKEUP_EXT0 ||
                            wakeCause == ESP_SLEEP_WAKEUP_EXT1);
  // Deep sleep'ten INT1 (hard shake) ile uyandiysa GPIO2 HIGH olacak.
  // digitalRead oncesinde PULLDOWN ile konfigure et: sensor konfigure edilmeden
  // once pin floating olabilir; pulldown olmadan yanlis HIGH okunup her boot'ta
  // hard-shake animasyonu oynardi. (IPOL=1 latch aktifse push-pull HIGH kazanir.)
  pinMode(PIN_ACCEL_INT1, INPUT_PULLDOWN);
  bool wokeByAccelHardShake = wokeFromMochiDeepSleep &&
                               (wakeCause == ESP_SLEEP_WAKEUP_GPIO) &&
                               digitalRead(PIN_ACCEL_INT1);
  // Magic degerini sebep ne olursa olsun temizle; manuel reset/USB reset sonrasi
  // eski deep-sleep bayragi kalip sonraki acilislari karistirmasin.
  if(hadMochiDeepSleepMagic) {
    mochiDeepSleepMagic = 0;
  }

  // OLED hafizasi resetten sonra son gosterilen frame'i kisa sure tutabilir.
  // Bu yuzden Serial beklemesi / LittleFS / config islerinden once ekrani hemen temizliyoruz;
  // boylece Hello there / Opening Event oncesinde eski yuz gorunmez.
  u8g2.begin();
  u8g2.begin();  // bu kart/ekran kombinasyonunda daha stabil baslatma icin korunuyor
  Serial.println("[BOOT] OLED init ok");
  u8g2.setBusClock(2000000);
  config.displayBrightnessPercent = DEFAULT_DISPLAY_BRIGHTNESS_PERCENT;
  applyDisplayBrightness();
  u8g2.clearBuffer();
  u8g2.sendBuffer();
  delay(40);

  Serial.println("\n=== MOCHI ===");
  Serial.println("[BUILD] Mochi-EventRefactor-Clean-UIFix2 - tab DOM/savebar fix");
  
  pinMode(PIN_TOUCH, INPUT);
  analogSetPinAttenuation(PIN_TOUCH, ADC_11db);  // dokunma ADC ile okunuyor (GPIO1 dijital hasarli)
  setupBatteryMonitor();
  
  // LittleFS: once format atmadan dener; partition degistiyse 0/0 KB'yi duzeltmek icin bir kez formatlar.
  initLittleFS();
  Serial.println("[BOOT] LittleFS ok");
  
  // Config
  loadConfig();
  if(ACCEL_FORCE_ENABLE_FOR_DIAG && !accelEnabled) {
    Serial.println("[ACCEL] Config'te kapali gorundu; DIAG icin runtime enable yapildi (config kaydedilmedi).");
    accelEnabled = true;
  }
  Serial.printf("[ACCEL-CONFIG] enabled=%d soft=%d hard=%d accel=%d orient=%d pickup=%d freefall=%d\n",
                (int)accelEnabled, accelSoftShakeCount, accelHardShakeCount,
                accelAccelCount, accelOrientationCount, accelPickupCount, accelFreefallCount);
  applyDisplayBrightness();
  Serial.printf("OLED parlaklik: %d%% (contrast=%d)\n", config.displayBrightnessPercent, displayBrightnessPercentToContrast(config.displayBrightnessPercent));
  Serial.printf("Default: %s, Current: %s\n", 
                config.defaultFace.c_str(), config.currentFace.c_str());

  // ivmeolcer — config yukle baslat ki hassasiyet degerleri hazir olsun

  // WebServer handler'larini bir kez burada baglıyoruz. Eskiden startWiFiAP
  // her cagrildiginda re-register oluyordu, .on() handler'lari stack ettigi icin
  // her toggle sonrasi /api/* istekleri kat kat handler tetikliyordu.
  // server.begin()/stop() handler'lari korur, sadece dinleme durumunu degistirir.
  setupWebServer();

  randomSeed(esp_random());

  // Normal acilista Opening Event/Hello goster. Deep sleep'ten dokunmayla uyanildiysa
  // cihaz yeniden baslar ama bu acilis animasyonunu atlar; varsa sadece uyanma animasyonu oynar.
  if(wokeFromMochiDeepSleep) {
    Serial.println("Deep sleep'ten dokunma ile uyandi - Opening/Hello atlandi");
    String wakeName = getRandomReactionWithHistory(sleepWakeEvents, sleepWakeEventCount, recentSleepWakeHistory, recentSleepWakeHistoryCount);
    if(wakeName.length() > 0) {
      playNamedAnimationBlocking(wakeName, 0, false);
    }
  } else {
    if(!playStartupOpeningAnimation()) {
      playHelloThereAnimation();
    }
  }
  
  // Yuzler sekmesinde aktif/default secilen yuzu yukle. Dosya yoksa sadece bir kez uyar, sonra fallback bekleme moduna gec.
  String startFace = config.currentFace;
  if(startFace.length() == 0 || !validateFaceFile(startFace)) {
    startFace = findFirstUsableFaceName();
    if(startFace.length() > 0) {
      config.currentFace = startFace;
      config.defaultFace = startFace;
      saveConfig();
      Serial.printf("Baslangicta aktif yuz otomatik secildi: %s\n", startFace.c_str());
    }
  }

  if(startFace.length() > 0 && loadAnimation(startFace, currentAnim)) {
    activeAnim = &currentAnim;
  } else {
    if(countFaceFiles() == 0) {
      Serial.println("/faces icinde animasyon yok - fallback yuz ciziliyor, arayuzden .bin yukleyebilirsin.");
    }
    enterFallbackWaitMode(startFace, "Baslangicta secili/default yuz bulunamadi.");
  }

  // ivmeolcer — yuz yuklenmesi bitti, simdi Wire/I2C baslat (GPIO20/21)
  // LittleFS okumalarindan SONRA cagrilmali; Wire.begin ESP32-C3 USB-CDC
  // pinlerini (20/21) I2C moduna alir, oncesinde LittleFS hazir olmali.
  Serial.println("[BOOT] ivmeolcer baslatiliyor...");
  setupAccelerometer();
  Serial.println("[BOOT] ivmeolcer asamasi bitti");
  
  // Otomatik WiFi STA bagla (eger ayarliysa)
  if(config.wifiAutoConnect && config.wifiSSID.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.wifiSSID.c_str(), config.wifiPassword.c_str());
    // Async, blocking olmasin. M-10: 90sn icinde baglanmazsa loop'tan kapatilacak.
    staAutoConnectInProgress = true;
    staConnectAttemptStart = millis();
  }
  
  mood = MOOD_IDLE;
  selectIdleNext();
  scheduleNextRandomEvent();
  lastInteractionTime = millis();
  if(wokeFromMochiDeepSleep) {
    ignoreTouchUntilRelease = true;
    touchStartTime = millis();
  }

  // Deep sleep'ten INT1 (sert sallanma) ile uyandiysa hard shake react oyna
  if(wokeByAccelHardShake && accelReady && accelHardShakeCount > 0) {
    Serial.println("Deep sleep'ten sert sallanma ile uyandi — hard shake react oynatiliyor");
    triggerAccelReaction(accelHardShakeReactions, accelHardShakeCount,
                          recentAccelHardHistory, recentAccelHardHistoryCount,
                          &accelCooldownHardEnd, ACCEL_COOLDOWN_HARD_MS);
  }
  
  Serial.println("Hazir!");
}

void loop() {
  // Uyku modundayken aktif loop calismasin: ADC/pil olcumu, animasyon, WiFi,
  // server, event ve bildirim yok. Sadece TTP223 dokunma pini ESP32 light sleep'ten uyandirir.
  if(systemSleeping) {
    enterMinimalLightSleepUntilTouch();
    return;
  }

  handleTouch();
  handleAccelEvents();  // ivmeolcer event'lerini isle

  // TESHIS: loop hizi + heap + durum. Loop <200/sn ise bir sey blokluyor
  // demektir (ISR firtinasi, I2C takilmasi, surekli dosya yukleme...).
  {
    static unsigned long diagWindowStart = 0;
    static uint32_t diagLoopCount = 0;
    diagLoopCount++;
    unsigned long dnow = millis();
    if(dnow - diagWindowStart >= 5000UL) {
      bool touchNowDiag = readTouchActive();
      Serial.printf("[DIAG] loop=%lu/sn heap=%u mood=%d wifi=%d accel=%d ready=%d en=%d intFallback=%d | raw=%d adc=%d prev=%d ign=%d tap=%d gecis=%lu\n",
                    (unsigned long)(diagLoopCount / 5), (unsigned)ESP.getFreeHeap(),
                    (int)mood, (int)wifiActive, (int)(accelEnabled && accelReady),
                    (int)accelReady, (int)accelEnabled, (int)accelInterruptsSuspended,
                    (int)touchNowDiag, touchAdcLast, (int)prevTouch, (int)ignoreTouchUntilRelease,
                    tapCount, (unsigned long)touchRawTransitions);
      touchRawTransitions = 0;
      diagWindowStart = dnow;
      diagLoopCount = 0;
    }
  }

  checkSleepMode();
  if(systemSleeping) {
    enterMinimalLightSleepUntilTouch();
    return;
  }

  // / ChatGPT-58: server.handleClient() display'den ONCE cagriliyor;
  // boylece gelen istekler bir sonraki ~10ms cizim turunu beklemek zorunda
  // kalmiyor, ayni iterasyonda isleniyor. Cizim sonunda da bir kez daha
  // cagiriliyor (asagida) ki cevap iceren request'ler ayni dongude bitebilsin.
  if(wifiActive) {
    server.handleClient();
  }

  updateAnimation();
  checkBatteryWarnings();
  
  // Pil/WiFi bildirimi ekran modunu ele gecirmez. Pil bilgisi mevcut
  // yuzun ustune overlay olarak drawCurrentFrame() icinde cizilir. Sure bitince
  // sadece flag kapanir ve bir sonraki frame dirty olur; default yuz reload/hard
  // recovery yok. Bu, pil gosterimi sonrasi OLED/animasyon bozulmalarini engeller.
  unsigned long now = millis();
  if(showingBatteryNotification && now - batteryNotificationStartTime >= BATTERY_NOTIFICATION_DURATION_MS) {
    showingBatteryNotification = false;
    lastDrawnAnim = nullptr;
    lastDrawnFrame = -999;
    markDisplayDirty();
    Serial.println("[DISPLAY] battery overlay kapandi");
  }
  if(showingWifiNotification && now - wifiNotificationStartTime >= WIFI_NOTIFICATION_DURATION_MS) {
    showingWifiNotification = false;
    lastDrawnWifi = !wifiActive;
    markDisplayDirty();
    Serial.println("[DISPLAY] wifi overlay kapandi");
  }
  drawCurrentFrame();
  
  // WiFi server tick (ikinci tur): cizim sonrasi gelen requestleri yakala.
  if(wifiActive) {
    server.handleClient();

    // Bagli istemci sayisini sadece durum/log icin takip et.
    int apClients = WiFi.softAPgetStationNum();
    if(apClients > 0) {
      lastAPClientSeenTime = now;
    }

    // (B karari): AP acik kalma suresi artik web etkinligine (lastWebActivityTime)
    // bagli. Tarayici acik tutuldukca veya istek geldikce sure resetlenir; arayuz
    // acikken kullanici kullaniyor varsayilir, AP kapanmaz. Ping/keep-alive bu
    // sayede islevsel hale gelir. Eskiden wifiActivatedTime baz aliniyordu, ki
    // aktivite olsa da hard timeout uyguluyordu (gereksiz copilik).
    unsigned long wifiTimeoutMs = (unsigned long)config.wifiTimeoutMinutes * 60UL * 1000UL;
    if(wifiTimeoutMs < 60000UL) wifiTimeoutMs = 60000UL;
    bool clientRecentlySeen = (lastAPClientSeenTime > 0 && now - lastAPClientSeenTime < wifiTimeoutMs);
    if(apClients <= 0 && !clientRecentlySeen && now - lastWebActivityTime > wifiTimeoutMs) {
      Serial.printf("WiFi web etkinligi yok - kapatiliyor (clients=%d, timeout=%lu ms)\n", apClients, wifiTimeoutMs);
      stopWiFiAP();
    }
  }

  // STA auto-connect izlencesi. Acilista wifiAutoConnect aktifse 90 sn
  // icinde baglanti olmazsa STA radyosunu kapatip pil kazanci sagliyoruz.
  // Bagli olunca durumu kapatip izlemeyi durduruyoruz; AP acilirsa zaten
  // WiFi.mode(WIFI_AP_STA) yeniden tetikler.
  if(staAutoConnectInProgress && !wifiActive) {
    if(WiFi.status() == WL_CONNECTED) {
      Serial.printf("STA bagli: %s\n", WiFi.localIP().toString().c_str());
      staAutoConnectInProgress = false;
    } else if(now - staConnectAttemptStart > STA_AUTO_CONNECT_TIMEOUT_MS) {
      Serial.println("STA baglanamadi (90sn timeout) - radyo kapatiliyor (pil korumasi)");
      WiFi.disconnect(true, true);
      WiFi.mode(WIFI_OFF);
      staAutoConnectInProgress = false;
    }
  }

  delay(5);
}
