/*
  ============================================================
      TOMADA INTELIGENTE PARA SISTEMA DE HIDROPONIA
      ESP32 + Wi-Fi + WebServer + DHT22 + Sensor de nível
  ============================================================

  PINOS:
    RELAY_PIN  = 23
    DHT_PIN    = 4
    LEVEL_PIN  = 5

  ATENÇÃO:
    O ESP32 trabalha somente com baixa tensão.
    A ligação da bomba à rede elétrica 127/220 V deve ser
    feita por um adulto/profissional qualificado, dentro de
    uma caixa apropriada e com as proteções elétricas adequadas.

  ============================================================
*/

#include <WiFi.h>
#include "HydroSecrets.h"
#include <WebServer.h>
#include <Preferences.h>
#include <DHT.h>
#include <time.h>
#include <esp_system.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// ============================================================
// CONFIGURAÇÕES
// ============================================================

// -------- Wi-Fi --------
// Preencha localmente. As credenciais que estavam no firmware anterior
// foram removidas porque foram compartilhadas em texto aberto.

// -------- Login do painel --------
// -------- Pinos --------
#define RELAY_PIN 23
#define DHT_PIN 4
#define LEVEL_PIN 5
#define BUZZER_PIN 18
#define DHT_TYPE DHT22

// Habilite a nuvem após provisionar o backend. O token nunca vai para a UI.
// Copie a CA raiz adequada ao certificado TLS do seu domínio Vercel.
// Sem CA configurada, a comunicação remota fica desabilitada (falha segura).

// ============================================================
// OBJETOS
// ============================================================

WebServer server(80);
Preferences preferences;
DHT dht(DHT_PIN, DHT_TYPE);

// ============================================================
// CONSTANTES
// ============================================================

const uint8_t SLOT_COUNT = 8;
const uint8_t HISTORY_SIZE = 12;

// Tempo máximo do modo manual: 30 minutos
const uint32_t MANUAL_TIMEOUT_MS = 30UL * 60UL * 1000UL;

// Intervalos
const uint32_t DHT_INTERVAL_MS = 2500;
const uint32_t WIFI_RETRY_INTERVAL_MS = 10000;
const uint32_t WIFI_TIMEOUT_MS = 30000;
const uint32_t LEVEL_DEBOUNCE_MS = 250;

// Sensor de nível
// Ajuste caso seu sensor trabalhe invertido.
const int LOW_LEVEL_READING = LOW;

// Relé:
// false = bomba desligada
// true  = bomba ligada
//
// Se o seu módulo for RELAY ACTIVE LOW, deixe true.
// Se for ACTIVE HIGH, coloque false.
const bool RELAY_ACTIVE_LOW = true;

// ============================================================
// ESTRUTURAS
// ============================================================

struct ScheduleSlot {
  bool enabled;
  uint8_t startHour;
  uint8_t startMinute;
  uint8_t stopHour;
  uint8_t stopMinute;
};

struct HistoryEvent {
  time_t timestamp;
  bool timeValid;
  char text[80];
};

struct RemoteCommand {
  char id[37];
  char action[16];
  char payload[700];
};

struct RemoteResult {
  char id[37];
  bool ok;
  char error[128];
};

// ============================================================
// VARIÁVEIS
// ============================================================

ScheduleSlot slots[SLOT_COUNT];

HistoryEvent history[HISTORY_SIZE];

uint8_t historyHead = 0;
uint8_t historyCount = 0;

// Bomba
bool pumpOn = false;

bool manualMode = false;
bool manualRequest = false;

uint32_t manualStartedAt = 0;

// Estatísticas
uint32_t pumpCycles = 0;
uint64_t totalPumpMillis = 0;
uint32_t pumpStartedAt = 0;
uint32_t lastMetricsSaveAt = 0;

// DHT
float temperatureC = NAN;
float humidityPct = NAN;
bool dhtOk = false;

uint32_t lastDhtRead = 0;

// Sensor de nível
bool waterLow = false;
bool levelCandidateLow = false;
uint32_t levelCandidateSince = 0;
bool buzzerState = false;
uint32_t buzzerPhaseSince = 0;

// Wi-Fi
uint32_t wifiStartedAt = 0;
uint32_t lastWifiAttempt = 0;
bool apMode = false;

// Sessão web
String sessionToken = "";
uint32_t sessionLastActivity = 0;

const uint32_t SESSION_TIMEOUT_MS = 8UL * 60UL * 60UL * 1000UL;

bool parseHHMM(const String& value, uint8_t& hour, uint8_t& minute);
bool processAction(const char* action, JsonVariantConst payload, String& error);
void serviceBle();
void serviceCloudCommands();
void checkpointPumpTime();

// BLE: serviço de leitura/controle local, independente de Wi-Fi/internet.
static const char* BLE_DEVICE_NAME = "Hidroponia-ESP32";
static const char* BLE_SERVICE_UUID = "7b4a0001-7f3a-4b69-9f33-3a4f8c2c1000";
static const char* BLE_STATUS_UUID  = "7b4a0002-7f3a-4b69-9f33-3a4f8c2c1000";
static const char* BLE_COMMAND_UUID = "7b4a0003-7f3a-4b69-9f33-3a4f8c2c1000";
static const char* BLE_ACK_UUID     = "7b4a0004-7f3a-4b69-9f33-3a4f8c2c1000";
BLECharacteristic* bleStatusCharacteristic = nullptr;
BLECharacteristic* bleAckCharacteristic = nullptr;
volatile bool bleConnected = false;
volatile bool bleCommandPending = false;
char bleCommandBuffer[900] = {0};
portMUX_TYPE bleMux = portMUX_INITIALIZER_UNLOCKED;

QueueHandle_t cloudCommandQueue = nullptr;
QueueHandle_t cloudResultQueue = nullptr;
TaskHandle_t cloudTaskHandle = nullptr;
volatile bool cloudOnline = false;
uint32_t lastLocalRequestAt = 0;

// ============================================================
// HTML
// ============================================================

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#080d22">
<title>Hidroponia · Adib Maker</title>
<style>
:root{color-scheme:dark;--bg:#080d20;--panel:#0e1832;--panel2:#111d3b;--line:#20335e;--ink:#eff3ff;--muted:#8998ba;--purple:#675cff;--cyan:#2acbe0;--green:#29d19a;--red:#ff6683;--amber:#efbd67}
*{box-sizing:border-box}html{scroll-behavior:smooth;scroll-padding-top:20px}body{margin:0;min-height:100vh;background:radial-gradient(ellipse at 90% 0,#172555 0,transparent 37%),radial-gradient(ellipse at 35% 74%,#121a40 0,transparent 40%),var(--bg);color:var(--ink);font:14px/1.5 Arial,sans-serif}body:before{content:"";position:fixed;inset:0;z-index:-1;pointer-events:none;opacity:.17;background-image:linear-gradient(#243460 1px,transparent 1px),linear-gradient(90deg,#243460 1px,transparent 1px);background-size:52px 52px;mask-image:linear-gradient(90deg,#000,transparent 70%)}button,input{font:inherit}button{cursor:pointer}a{color:inherit;text-decoration:none}.layout{min-height:100vh}.sidebar{position:fixed;inset:0 auto 0 0;width:238px;padding:20px 14px 15px;display:flex;flex-direction:column;overflow:hidden;background:linear-gradient(180deg,#090f25,#080c1d);border-right:1px solid #1a2a50;z-index:5}.brand{display:flex;align-items:center;gap:11px;padding:2px 7px 23px;border-bottom:1px solid #1b2b50}.brand-mark{width:39px;height:39px;border-radius:13px;display:grid;place-items:center;background:#112347;border:1px solid #314c91;color:#73ddbc;box-shadow:0 0 22px #514bff30}.brand-mark svg{width:26px;fill:none;stroke:currentColor;stroke-width:1.7;stroke-linecap:round;stroke-linejoin:round}.brand b{display:block;font-size:.77rem}.brand small{display:block;margin-top:3px;font-size:.59rem;color:#8190b3;letter-spacing:.08em}.side-nav{display:grid;gap:6px;padding-top:20px}.side-nav a{display:flex;align-items:center;gap:12px;padding:10px 12px;border:1px solid transparent;border-radius:10px;color:#97a5c7;font-size:.8rem}.side-nav a:hover,.side-nav a.active{color:#f4f5ff;background:linear-gradient(100deg,#2b247e,#162654);border-color:#5149c9}.side-nav .icon{width:20px;text-align:center;font-size:1.1rem;color:#9288ff}.sidebar-art{height:200px;margin:20px -14px 0;position:relative;overflow:hidden;background:radial-gradient(ellipse at 50% 80%,#3c4dcc4d,transparent 62%)}.grow-light{position:absolute;left:6%;right:5%;bottom:75px;height:3px;background:#ac79ff;box-shadow:0 0 12px 5px #654bff9c}.grow-pipe{position:absolute;left:0;right:-18px;bottom:38px;height:23px;border:2px solid #3d50a0;border-left:0;border-radius:0 16px 16px 0;background:linear-gradient(#15254d,#0b1532);box-shadow:0 0 19px #5551ff65}.plant{position:absolute;bottom:59px;left:46%;width:20px;height:52px;border-radius:100% 0 100% 0;transform:rotate(-37deg);background:linear-gradient(135deg,#83f381,#187b85);box-shadow:0 0 17px #2fc58b70}.plant.two{left:35%;height:42px;transform:rotate(35deg) scaleX(-1);background:linear-gradient(135deg,#4fe0ae,#125578)}.plant-stem{position:absolute;left:45%;bottom:53px;width:2px;height:58px;background:#42bb91}.side-status{margin-top:auto;padding:11px 12px;border:1px solid #20335e;border-radius:10px;background:#101b38;font-size:.73rem;color:#52dcb3}.side-status small{display:block;margin-top:4px;color:#8190b2;font-size:.59rem}.school-label{padding:13px 4px 8px;color:#7584a9;font: .58rem/1.8 monospace;letter-spacing:.09em}.school-label b{color:#b8c4e2;font-weight:500}.workspace{margin-left:238px;padding:0 clamp(16px,3vw,42px) 30px;max-width:1700px}.topbar{height:82px;margin-bottom:21px;padding:0 18px;display:flex;justify-content:space-between;align-items:center;border:1px solid #1a2a50;border-top:0;border-radius:0 0 14px 14px;background:#090f24e8;box-shadow:0 14px 35px #02040b66}.topbar h1{margin:0;font-size:1.27rem;letter-spacing:-.04em}.eyebrow{margin:0 0 4px;color:#8d86ff;font: .6rem monospace;letter-spacing:.13em}.top-right{display:flex;align-items:center;gap:9px}.pill{padding:7px 10px;border-radius:99px;border:1px solid #215044;background:#102a2c;color:#5adab5;font-size:.66rem}.top-clock{text-align:right;color:#eef2ff;font: .75rem monospace}.top-clock small{display:block;color:#7e8eaf;font-size:.58rem}.content{max-width:1420px;margin:auto}.grid{display:grid;grid-template-columns:1.2fr repeat(3,minmax(0,1fr));gap:12px}.card{background:linear-gradient(145deg,#0e1935,#0a132b);border:1px solid var(--line);border-radius:13px;box-shadow:0 16px 40px #02051050;padding:17px}.pump-card{position:relative;overflow:hidden;min-height:202px;display:flex;flex-direction:column;justify-content:space-between;border-color:#354988;background:radial-gradient(ellipse at 94% 8%,#3a4fd333,transparent 38%),linear-gradient(145deg,#111e40,#0b1530)}.pump-card:after{content:"";position:absolute;right:-80px;top:24px;width:170px;height:170px;border:1px solid #3e5192;border-radius:50%;box-shadow:0 0 0 17px #3b4a8c18,0 0 0 35px #3b4a8c12}.card h2{font-size:.92rem;margin:0 0 12px}.pump-card h2{font-size:1.45rem;margin:0 0 2px;letter-spacing:-.04em}.muted{color:var(--muted);font-size:.72rem}.pump-emblem{position:absolute;right:17px;top:17px;width:39px;height:39px;display:grid;place-items:center;border-radius:12px;background:#182c5c;border:1px solid #31529d;color:#58c8e9;font-size:1.15rem;box-shadow:0 0 22px #2773df40}.controls{position:relative;z-index:1;display:flex;gap:6px;flex-wrap:wrap;margin-top:14px}.controls button{min-height:32px;padding:7px 10px;border:1px solid transparent;border-radius:8px;color:#fff;font-size:.67rem;font-weight:bold}.btn-on{background:#128d74}.btn-off{background:#922b4b}.btn-auto{background:#5746da}.controls button:hover{filter:brightness(1.12)}.controls button:disabled{opacity:.55;cursor:wait}.pump-meta{border-top:1px solid #293965;margin-top:9px;padding-top:8px;color:#9aa9cb;font:.58rem monospace;position:relative;z-index:1}.metric-card{min-height:202px;position:relative;overflow:hidden}.metric-card:after{content:"";position:absolute;right:-32px;bottom:-55px;width:130px;height:96px;border:1px solid #30477b70;border-radius:50%;transform:rotate(-20deg)}.metric-label{color:#9eacd0;font-size:.73rem}.metric-icon{display:inline-grid;place-items:center;width:25px;height:25px;margin-right:6px;border-radius:8px;background:#172b5b;color:#5fc7eb}.metric-value{display:block;margin:16px 0 4px;font-size:1.8rem;letter-spacing:-.05em}.metric-foot{font-size:.62rem;color:#8090b4}.low{color:#ff8298!important}.safe{color:#58ddb4!important}.schedule-stats{display:grid;grid-template-columns:minmax(0,1.65fr) minmax(270px,.85fr);gap:12px;margin-top:12px;align-items:stretch}.section-title{display:flex;justify-content:space-between;align-items:flex-start;gap:10px;margin-bottom:14px}.section-title h2{font-size:1.04rem;margin:0 0 3px}.section-title p{margin:0;color:#8190b2;font-size:.65rem}.section-mark{width:30px;height:30px;display:grid;place-items:center;border-radius:9px;background:#1b2059;border:1px solid #393e92;color:#a197ff;font-size:1rem}.schedule{display:grid;grid-template-columns:minmax(105px,1fr) minmax(85px,.8fr) 18px minmax(85px,.8fr);align-items:center;gap:8px;padding:5px 6px;border-top:1px solid #1b2a4e}.schedule label{display:flex;align-items:center;gap:7px;font-size:.68rem;color:#c7d3ed}.schedule input[type=checkbox]{accent-color:#685cff;width:14px;height:14px;margin:0}.schedule input[type=time]{width:100%;min-width:0;height:28px;padding:3px 6px;border:1px solid #26385e;border-radius:6px;background:#091128;color:#e6ecff;color-scheme:dark;font: .66rem monospace}.schedule-arrow{color:#6276a0;text-align:center}.schedule-save{width:100%;margin:11px 0 0;min-height:35px;border:0;border-radius:8px;background:linear-gradient(100deg,#6354ff,#3a50ec);color:white;font-weight:bold;font-size:.68rem}.stats p{display:grid;grid-template-columns:26px 1fr auto;gap:8px;align-items:center;margin:0;padding:11px 0;border-top:1px solid #1a2a4d;color:#93a3c6;font-size:.67rem}.stats p span{color:#a193ff;text-align:center}.stats strong{max-width:150px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:#e5ebff;font: .68rem monospace;text-align:right}.history{max-height:300px;overflow-y:auto}.history-item{display:flex;gap:13px;padding:10px 2px;border-bottom:1px solid #1b2b50;color:#c4d0eb;font-size:.7rem}.history-item:last-child{border:0}.history-item time{flex:none;color:#8797ba;font: .62rem monospace}.logout-card{margin-top:12px;padding:11px;display:flex;justify-content:space-between;align-items:center;color:#8291b4;font-size:.65rem}.logout-card button{border:1px solid #26365d;border-radius:7px;background:#111c38;color:#ced7ed;padding:7px 12px;font-size:.66rem}.toast{position:fixed;z-index:9;right:18px;top:14px;padding:11px 15px;background:#17294a;border:1px solid #34517b;border-radius:9px;color:#edf2ff;opacity:0;transform:translateY(-5px);transition:.18s;pointer-events:none}.toast.visible{opacity:1;transform:translateY(0)}.toast.error{background:#351a31;border-color:#8b395a}.button:disabled{opacity:.55}.section-link{scroll-margin-top:18px}.diagnostic-lines{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:8px;margin-top:12px}.diagnostic-lines p{margin:0;padding:11px;border-radius:9px;background:#0b142b;border:1px solid #1b2b50;color:#8e9fc4;font-size:.64rem}.diagnostic-lines strong{display:block;margin-top:4px;color:#dce6ff;font-size:.7rem}.footer{padding:15px 2px;border-top:1px solid #1b2a4c;margin-top:16px;display:flex;justify-content:space-between;color:#7080a5;font:.56rem monospace;letter-spacing:.08em}
@media(max-width:1050px){.sidebar{width:205px}.workspace{margin-left:205px;padding-inline:18px}.grid{grid-template-columns:repeat(3,minmax(0,1fr))}.pump-card{grid-column:span 1}.schedule-stats{grid-template-columns:minmax(0,1.3fr) minmax(245px,.9fr)}}
@media(max-width:760px){.sidebar{width:62px;padding:13px 7px}.brand{justify-content:center;padding:1px 0 13px}.brand-text,.school-label,.sidebar-art,.side-status small{display:none}.brand-mark{width:40px;height:40px}.side-nav{gap:7px;padding-top:14px}.side-nav a{justify-content:center;height:42px;padding:0;font-size:0}.side-nav .icon{font-size:1.15rem}.side-status{padding:10px 2px;text-align:center;font-size:0}.side-status:before{content:"●";font-size:.7rem;color:var(--green)}.workspace{margin-left:62px;padding-inline:11px}.topbar{height:70px;padding-inline:12px}.topbar h1{font-size:1.05rem}.topbar .eyebrow{font-size:.51rem}.top-right{gap:6px}.top-right .pill{font-size:0;padding:9px}.top-right .pill:before{content:"●";font-size:.6rem}.top-clock{font-size:.65rem}.grid{grid-template-columns:repeat(2,minmax(0,1fr));gap:8px}.pump-card,.metric-card{min-height:178px;padding:13px}.pump-card h2{font-size:1.12rem}.pump-emblem{width:31px;height:31px;right:11px;top:11px}.controls{gap:4px}.controls button{padding:6px 7px;font-size:.59rem}.schedule-stats{grid-template-columns:1fr;gap:8px}.card{padding:13px}.stats p{padding:9px 0}.schedule{grid-template-columns:minmax(90px,1fr) minmax(70px,.8fr) 12px minmax(70px,.8fr);gap:4px}.schedule label{font-size:.59rem;gap:4px}.schedule input[type=time]{font-size:.57rem;padding:2px 4px}.diagnostic-lines{grid-template-columns:repeat(2,minmax(0,1fr))}}
@media(max-width:430px){.sidebar{position:fixed;inset:auto 0 0;width:auto;height:61px;padding:4px 8px;background:#090f25f5;border:0;border-top:1px solid #23345e}.brand,.side-status{display:none}.side-nav{padding:0;display:flex;justify-content:space-around;align-items:center;height:100%}.side-nav a{flex:1;height:48px;border-radius:8px}.workspace{margin:0;padding:0 9px 82px}.topbar{margin-bottom:12px}.grid{gap:7px}.pump-card,.metric-card{min-height:164px;padding:11px}.metric-value{font-size:1.48rem;margin-top:13px}.metric-label{font-size:.63rem}.section-title h2{font-size:.94rem}.schedule{grid-template-columns:minmax(84px,1fr) minmax(63px,.8fr) 10px minmax(63px,.8fr);gap:3px}.schedule label{font-size:.55rem}.schedule input[type=time]{font-size:.53rem;height:26px}.footer{font-size:.5rem;flex-direction:column;gap:5px}}

.schedule-head{display:grid;grid-template-columns:minmax(105px,1fr) minmax(85px,.8fr) 18px minmax(85px,.8fr);gap:8px;padding:0 6px 7px;color:#7c8db4;font:.56rem monospace;text-transform:uppercase;letter-spacing:.08em}.metric-value.ok{color:var(--green)}.metric-value.danger{color:var(--red)}
</style>
</head>
<body>
<div class="layout">
<aside class="sidebar">
  <div class="brand"><span class="brand-mark"><svg viewBox="0 0 32 32"><path d="M16 27V16m0 3c-7 0-10-4-10-10 7 0 10 4 10 10Zm0-3c0-7 4-10 10-10 0 7-4 10-10 10Z"/></svg></span><span class="brand-text"><b>Sistema de Hidroponia</b><small>TOMADA INTELIGENTE · ESP32</small></span></div>
  <nav class="side-nav"><a class="active" href="#overview"><span class="icon">⌂</span>Painel</a><a href="#schedule"><span class="icon">◷</span>Horários</a><a href="#stats"><span class="icon">▥</span>Estatísticas</a><a href="#history-panel"><span class="icon">▤</span>Histórico</a><a href="#connections"><span class="icon">⌘</span>Conexões</a></nav>
  <div class="sidebar-art" aria-hidden="true"><span class="plant"></span><span class="plant two"></span><span class="plant-stem"></span><span class="grow-light"></span><span class="grow-pipe"></span></div>
  <div class="side-status" id="sideConnection">Dispositivo conectado<small>ESP32 · rede local</small></div>
  <div class="school-label">ESCOLA ESTADUAL<br><b>ADIB MIGUEL HADDAD</b></div>
</aside>
<div class="workspace">
<header class="topbar"><div><p class="eyebrow">CULTIVO CONECTADO · ESP32</p><h1>Visão geral</h1></div><div class="top-right"><span class="pill">Wi-Fi local</span><div class="top-clock" id="topClock">--:--<small>HORÁRIO DO DISPOSITIVO</small></div></div></header>
<div id="toast" class="toast" role="status" aria-live="polite"></div>
<main class="content" id="overview">
<section class="grid">
  <article class="card pump-card" id="control"><div><p class="eyebrow">ESTADO DA BOMBA</p><h2 id="pump" class="metric-value">--</h2><div id="mode" class="muted">Modo aguardando</div></div><span class="pump-emblem" aria-hidden="true">≈</span><div class="controls"><button class="btn-on" onclick="control('on')">▶&nbsp; Ligar</button><button class="btn-off" onclick="control('off')">⏻&nbsp; Desligar</button><button class="btn-auto" onclick="control('auto')">Automático</button></div><div class="pump-meta">Proteção ativa · nível de água tem prioridade</div></article>
  <article class="card metric-card"><span class="metric-label"><i class="metric-icon">♨</i> Temperatura</span><strong id="temp" class="metric-value">--</strong><small class="metric-foot">Sensor DHT22 · leitura atual</small></article>
  <article class="card metric-card"><span class="metric-label"><i class="metric-icon">◌</i> Umidade</span><strong id="hum" class="metric-value">--</strong><small class="metric-foot">Umidade relativa do ar</small></article>
  <article class="card metric-card"><span class="metric-label"><i class="metric-icon">≋</i> Nível da água</span><strong id="level" class="metric-value">--</strong><small class="metric-foot">Reservatório · chave boia</small></article>
</section>
<div class="schedule-stats">
  <section class="card section-link" id="schedule"><div class="section-title"><div><p class="eyebrow">AUTOMAÇÃO LOCAL</p><h2>Horários automáticos</h2><p>Os períodos continuam ativos sem o painel aberto.</p></div><span class="section-mark">◷</span></div><div class="schedule-head"><span>Período</span><span>Ligar</span><span></span><span>Desligar</span></div><div id="schedules"></div><button class="schedule-save btn-auto" onclick="saveSchedules()">▣&nbsp; Salvar horários</button></section>
  <section class="card stats section-link" id="stats"><div class="section-title"><div><p class="eyebrow">RESUMO DE OPERAÇÃO</p><h2>Estatísticas e conexão</h2></div><span class="section-mark">▥</span></div><p><span>◉</span>Ciclos da bomba<strong id="cycles">--</strong></p><p><span>◷</span>Tempo total ligado<strong id="total">--</strong></p><p><span>▦</span>Horário atual<strong id="now">--</strong></p><p><span>◎</span>IP do dispositivo<strong id="ip">--</strong></p><div class="diagnostic-lines" id="connections"><p>Wi-Fi / AP<strong id="wifi">--</strong></p><p>Intensidade<strong id="rssi">--</strong></p><p>Bluetooth LE<strong id="ble">--</strong></p><p>Serviço remoto<strong id="cloud">--</strong></p><p>Horário NTP<strong id="ntp">--</strong></p></div></section>
</div>
<section class="card section-link" id="history-panel" style="margin-top:12px"><div class="section-title"><div><p class="eyebrow">REGISTROS DO DISPOSITIVO</p><h2>Histórico de eventos</h2></div><span class="section-mark">▤</span></div><div id="history" class="history">Carregando…</div></section>
<section class="card logout-card"><span>Escola Estadual Adib Miguel Haddad · Projeto Maker</span><button onclick="logout()">Sair do painel&nbsp; ↗</button></section>
<footer class="footer"><span>TECNOLOGIA A SERVIÇO DO CULTIVO</span><span>CONTROLE AUTÔNOMO · SEM DEPENDÊNCIA DA NUVEM</span></footer>
</main></div></div>
<script>

let schedulesDirty = false;
let updateRunning = false;
let toastTimer = 0;

function toast(message, error = false) {
  const node = document.getElementById("toast");
  node.textContent = message;
  node.className = "toast visible" + (error ? " error" : "");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => node.className = "toast", 3200);
}

async function api(url, options = {}) {

  const response = await fetch(url, options);

  if (response.status === 401) {
    location.href = "/login";
    return null;
  }

  return await response.json();
}

async function control(action) {
  const controls = [...document.querySelectorAll('button[onclick^="control("]')];
  controls.forEach(button => button.disabled = true);
  try {
  const body = new URLSearchParams();

  body.append("action", action);

  const result = await api("/api/control", {
    method: "POST",
    headers: {
      "Content-Type": "application/x-www-form-urlencoded"
    },
    body
  });

  if (result && result.ok) toast("Comando confirmado pelo ESP32.");
  else if (result) toast(result.error || "O ESP32 recusou o comando.", true);
  update();
  } catch (error) { toast("Falha de comunicação com o ESP32.", true); }
  finally { controls.forEach(button => button.disabled = false); }
}

async function saveSchedules() {
  try {
  const body = new URLSearchParams();

  for (let i = 0; i < 8; i++) {

    const enabled =
      document.getElementById("en" + i).checked;

    const on =
      document.getElementById("on" + i).value;

    const off =
      document.getElementById("off" + i).value;

    if (enabled) {
      body.append("en" + i, "1");
    }

    body.append("on" + i, on);
    body.append("off" + i, off);
  }

  const result = await api("/api/schedules", {
    method: "POST",
    headers: {
      "Content-Type": "application/x-www-form-urlencoded"
    },
    body
  });

  if (result && result.ok) { schedulesDirty = false; toast("Horários gravados no ESP32."); }
  else if (result) toast(result.error || "Falha ao gravar os horários.", true);
  update();
  } catch (error) { toast("Não foi possível salvar os horários.", true); }
}

function renderSchedules(slots) {

  const container =
    document.getElementById("schedules");

  container.innerHTML = "";

  slots.forEach((slot, i) => {

    const row =
      document.createElement("div");

    row.className = "schedule";

    row.innerHTML = `
      <label>
        <input
          type="checkbox"
          id="en${i}"
          ${slot.en ? "checked" : ""}
        >
        Período ${String(i + 1).padStart(2, "0")}
      </label>

      <input
        type="time"
        id="on${i}"
        value="${slot.on}"
      >

      <span class="schedule-arrow" aria-hidden="true">→</span>

      <input
        type="time"
        id="off${i}"
        value="${slot.off}"
      >
    `;

    container.appendChild(row);
    row.querySelectorAll("input").forEach(input => input.addEventListener("input", () => { schedulesDirty = true; }));
  });
}

async function update() {
  if (updateRunning) return;
  updateRunning = true;
  try {
  const data =
    await api("/api/status");

  if (!data) return;

  document.getElementById("pump").textContent =
    data.pump ? "LIGADA" : "DESLIGADA";

  document.getElementById("pump").className =
    "metric-value " + (data.pump ? "ok" : "");

  document.getElementById("mode").textContent =
    data.manual
      ? "Modo manual"
      : "Modo automático";

  document.getElementById("temp").textContent =
    data.temp === null
      ? "--"
      : data.temp.toFixed(1) + " °C";

  document.getElementById("hum").textContent =
    data.hum === null
      ? "--"
      : data.hum.toFixed(1) + " %";

  document.getElementById("level").textContent =
    data.low
      ? "BAIXO"
      : "NORMAL";

  document.getElementById("level").className =
    "metric-value " + (data.low ? "danger" : "ok");

  document.getElementById("cycles").textContent =
    data.cycles;

  document.getElementById("total").textContent =
    data.totalMin + " min";

  document.getElementById("now").textContent =
    data.now;

  const clockNode = document.getElementById("topClock");
  if (data.now && data.now !== "--:--:--") {
    clockNode.textContent = data.now.slice(-8, -3);
    const clockDate = document.createElement("small");
    clockDate.textContent = data.now.slice(0, 10);
    clockNode.appendChild(clockDate);
  }

  document.getElementById("ip").textContent =
    data.ip;

  document.getElementById("wifi").textContent =
    data.wifi ? (data.apMode ? "AP de emergência ativo" : "Conectado") : "Desconectado";
  document.getElementById("sideConnection").textContent =
    data.wifi ? "Dispositivo conectado" : "Reconectando dispositivo";
  document.getElementById("rssi").textContent =
    data.rssi === null ? "--" : data.rssi + " dBm";
  document.getElementById("ble").textContent =
    data.ble ? "Cliente conectado" : "Anunciando Hidroponia-ESP32";
  document.getElementById("cloud").textContent =
    data.cloud ? "Comunicando" : "Desconectado ou não provisionado";
  document.getElementById("ntp").textContent =
    data.timeSynced ? "Sincronizado · UTC−3" : "Aguardando sincronização";

  if (!schedulesDirty) renderSchedules(data.slots);

  const history =
    document.getElementById("history");

  history.innerHTML = "";

  data.history.forEach(event => {

    const div =
      document.createElement("div");

    div.className = "history-item";

    div.textContent =
      event.time + " — " + event.text;

    history.appendChild(div);
  });
  } catch (error) {
    toast("Sem resposta do ESP32. Confira /ping e a rede local.", true);
  } finally { updateRunning = false; }
}

async function logout() {

  await api("/api/logout", {
    method: "POST"
  });

  location.href = "/login";
}

update();

setInterval(update, 3000);

</script>

</body>
</html>
)rawliteral";

// ============================================================
// HTML LOGIN
// ============================================================

const char LOGIN_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#080d22">
<title>Acesso · Hidroponia</title>
<style>
*{box-sizing:border-box}body{margin:0;min-height:100vh;display:grid;place-items:center;padding:20px;background:radial-gradient(ellipse at 15% 80%,#2526746b,transparent 35%),radial-gradient(ellipse at 88% 23%,#07546a55,transparent 32%),linear-gradient(135deg,#060a1c,#0a1128 57%,#06091a);color:#f1f4ff;font:14px/1.5 Arial,sans-serif;overflow:hidden}body:before{content:"";position:fixed;inset:0;pointer-events:none;opacity:.32;background:repeating-linear-gradient(0deg,transparent 0 80px,#24356a 81px,#24356a 82px,transparent 83px 143px),repeating-linear-gradient(90deg,transparent 0 90px,#182b5a 91px,#182b5a 92px,transparent 93px 173px);mask-image:linear-gradient(90deg,#000,transparent 36%,transparent 64%,#000)}body:after{content:"";position:fixed;inset:0;z-index:0;pointer-events:none;opacity:.32;background:radial-gradient(ellipse at 13% 69%,#46c274 0 2%,transparent 2.4%),radial-gradient(ellipse at 19% 61%,#38b984 0 2.3%,transparent 2.6%),radial-gradient(ellipse at 30% 73%,#59cf7e 0 2.5%,transparent 2.8%),linear-gradient(172deg,transparent 0 68%,#7c54ff 69%,#1b3078 70%,transparent 72%),linear-gradient(180deg,transparent 0 79%,#1a2d63 80% 85%,transparent 86%),linear-gradient(90deg,transparent 0 83%,#2c478a 84% 85%,transparent 86%)}.login-brand,.login-card,.slogan,.ambient-bottom{z-index:1}.login-brand{position:fixed;left:30px;top:25px;display:flex;align-items:center;gap:10px;color:#c7d5f7;font-size:.75rem;letter-spacing:.1em}.brand-mark{width:35px;height:35px;border-radius:11px;display:grid;place-items:center;border:1px solid #314b8c;background:#112347;color:#72ddb7}.brand-mark svg{width:24px;fill:none;stroke:currentColor;stroke-width:1.7;stroke-linecap:round;stroke-linejoin:round}.login-brand small{display:block;color:#7888ad;font-size:.58rem;margin-top:2px}.login-card{width:min(100%,405px);position:relative;padding:32px;background:linear-gradient(145deg,#0a122bee,#091027f4);border:1px solid #394c88;border-radius:18px;box-shadow:0 30px 100px #02040cb0,0 0 50px #5148ff20;backdrop-filter:blur(13px)}.login-card:before{content:"";position:absolute;inset:-1px;border-radius:19px;padding:1px;background:linear-gradient(130deg,#7564ff,#243a73 43%,#2dcbd666);mask:linear-gradient(#000 0 0) content-box,linear-gradient(#000 0 0);mask-composite:exclude;pointer-events:none}.brand-mark.large{width:58px;height:58px;margin:0 auto 13px;border-radius:17px;box-shadow:0 0 25px #4c58ff38}.brand-mark.large svg{width:37px}.login-card h1{text-align:center;font-size:1.9rem;letter-spacing:-.05em;margin:0}.subtitle{text-align:center;color:#8594b7;margin:2px 0 23px;font-size:.82rem}.eyebrow{text-align:center;color:#918aff;font: .6rem monospace;letter-spacing:.14em;margin:0 0 5px}.field-label{display:block;color:#adbad8;font-size:.72rem;font-weight:bold;margin:13px 0 6px}.field{height:47px;display:flex;align-items:center;gap:10px;padding:0 12px;border:1px solid #293b65;border-radius:10px;background:#091128}.field:focus-within{border-color:#786cff;box-shadow:0 0 0 3px #665cff1c}.field svg{width:19px;height:19px;fill:none;stroke:#bac7e4;stroke-width:1.6;stroke-linecap:round;stroke-linejoin:round}.field input{width:100%;min-width:0;background:transparent;border:0;outline:0;color:#f2f5ff;font-size:.8rem}.field input::placeholder{color:#657496}.show-pass{border:0;background:none;color:#91a0c2;font-size:.64rem;padding:6px}.login-button{height:47px;width:100%;margin-top:19px;border:0;border-radius:10px;background:linear-gradient(105deg,#6556ff,#3551ff);box-shadow:0 8px 22px #544aff44;color:white;font-weight:bold;font-size:.83rem}.login-button:hover{filter:brightness(1.1)}#error{min-height:20px;margin:9px 0 0;color:#ff8199;font-size:.73rem;text-align:center}.login-foot{border-top:1px solid #202e51;padding-top:15px;margin:12px 0 0;text-align:center;color:#7d8db2;font-size:.64rem}.login-foot b{color:#37d59d}.slogan{position:fixed;right:6%;top:10%;color:#9689ff;font-size:.88rem;font-style:italic;line-height:1.35;transform:rotate(-5deg)}.slogan strong{color:#5ad0d6}.ambient-bottom{position:fixed;left:7%;right:7%;bottom:7%;height:1px;background:linear-gradient(90deg,transparent,#4c43c6,transparent);box-shadow:0 0 22px #6351ff}.ambient-bottom:before{content:"";position:absolute;left:14%;right:14%;top:-6px;height:12px;background:radial-gradient(ellipse,#2fc995 0 3px,transparent 4px) left center/22% 12px no-repeat,radial-gradient(ellipse,#7668ff 0 3px,transparent 4px) right center/22% 12px no-repeat}
@media(max-width:520px){.login-brand{left:15px;top:14px;font-size:.63rem}.login-brand small{font-size:.52rem}.login-card{padding:27px 22px;margin-top:24px}.slogan{right:8%;top:12%;font-size:.68rem}.ambient-bottom{bottom:4%}}
</style>
</head>
<body>
<div class="login-brand"><span class="brand-mark"><svg viewBox="0 0 32 32"><path d="M16 27V16m0 3c-7 0-10-4-10-10 7 0 10 4 10 10Zm0-3c0-7 4-10 10-10 0 7-4 10-10 10Z"/></svg></span><span>SISTEMA DE HIDROPONIA<small>TOMADA INTELIGENTE · ESP32</small></span></div>
<div class="slogan">Tecnologia<br>que faz a natureza<br><strong>produzir mais.</strong></div>
<form class="login-card" onsubmit="event.preventDefault();login()"><span class="brand-mark large"><svg viewBox="0 0 32 32"><path d="M16 27V16m0 3c-7 0-10-4-10-10 7 0 10 4 10 10Zm0-3c0-7 4-10 10-10 0 7-4 10-10 10Z"/></svg></span><p class="eyebrow">ACESSO AO SISTEMA</p><h1>Hidroponia</h1><p class="subtitle">Sistema inteligente de cultivo</p>
<label class="field-label" for="user">Usuário</label><div class="field"><svg viewBox="0 0 24 24"><circle cx="12" cy="8" r="3.2"/><path d="M5.5 20c.6-3.4 2.7-5.2 6.5-5.2s5.9 1.8 6.5 5.2"/></svg><input id="user" placeholder="Seu usuário" autocomplete="username" required></div>
<label class="field-label" for="password">Senha</label><div class="field"><svg viewBox="0 0 24 24"><rect x="5.5" y="10" width="13" height="10" rx="2"/><path d="M8.5 10V7.5a3.5 3.5 0 0 1 7 0V10"/></svg><input id="password" type="password" placeholder="Sua senha" autocomplete="current-password" required><button class="show-pass" type="button" id="togglePassword">Mostrar</button></div>
<button id="loginButton" class="login-button" type="submit">Entrar&nbsp; →</button><div id="error" aria-live="polite"></div><p class="login-foot"><b>●</b>&nbsp; Acesso local protegido · ESP32</p></form><div class="ambient-bottom" aria-hidden="true"></div>
<script>

async function login() {
  const button = document.getElementById("loginButton");
  const error = document.getElementById("error");
  button.disabled = true;
  button.textContent = "Validando acesso…";
  error.textContent = "";
  try {
    const body = new URLSearchParams();
    body.append("user", document.getElementById("user").value);
    body.append("password", document.getElementById("password").value);
    const response = await fetch("/api/login", {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body
    });
    const data = await response.json();
    if (data.ok) location.href = "/";
    else error.textContent = data.error || "Usuário ou senha inválidos.";
  } catch (e) {
    error.textContent = "Não foi possível falar com o ESP32. Confira sua conexão.";
  } finally {
    button.disabled = false;
    button.textContent = "Entrar  →";
  }
}

document.getElementById("togglePassword").addEventListener("click", () => {
  const input = document.getElementById("password");
  const visible = input.type === "password";
  input.type = visible ? "text" : "password";
  document.getElementById("togglePassword").textContent = visible ? "Ocultar" : "Mostrar";
});

</script>

</body>

</html>
)rawliteral";

// ============================================================
// FUNÇÕES AUXILIARES
// ============================================================

String uint64Text(uint64_t value) {

  if (value == 0) {
    return "0";
  }

  char buffer[32];

  snprintf(
    buffer,
    sizeof(buffer),
    "%llu",
    (unsigned long long)value
  );

  return String(buffer);
}

// ------------------------------------------------------------

String jsonQuote(const String& value) {

  String out;

  out.reserve(value.length() + 4);

  out += '"';

  for (size_t i = 0; i < value.length(); ++i) {

    char c = value[i];

    if (c == '"' || c == '\\') {
      out += '\\';
    }

    out += c;
  }

  out += '"';

  return out;
}

// ------------------------------------------------------------

String slotTime(
  uint8_t hour,
  uint8_t minute
) {

  char buffer[6];

  snprintf(
    buffer,
    sizeof(buffer),
    "%02u:%02u",
    hour,
    minute
  );

  return String(buffer);
}

// ------------------------------------------------------------

bool timeIsValid() {
  return time(nullptr) > 1700000000;
}

// ------------------------------------------------------------

String currentTimeText() {

  if (!timeIsValid()) {
    return "--:--:--";
  }

  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  char buffer[20];

  strftime(
    buffer,
    sizeof(buffer),
    "%d/%m/%Y %H:%M:%S",
    &timeinfo
  );

  return String(buffer);
}

// ------------------------------------------------------------

uint32_t currentMinutes() {

  if (!timeIsValid()) {
    return 0;
  }

  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  return
    (uint32_t)timeinfo.tm_hour * 60UL +
    timeinfo.tm_min;
}

// ============================================================
// HISTÓRICO
// ============================================================

void addHistory(const char* text) {

  HistoryEvent& event =
    history[historyHead];

  event.timestamp = time(nullptr);
  event.timeValid = timeIsValid();

  strncpy(
    event.text,
    text,
    sizeof(event.text) - 1
  );

  event.text[
    sizeof(event.text) - 1
  ] = '\0';

  historyHead =
    (historyHead + 1) % HISTORY_SIZE;

  if (historyCount < HISTORY_SIZE) {
    historyCount++;
  }
}

// ------------------------------------------------------------

String eventTime(
  const HistoryEvent& event
) {

  if (!event.timeValid) return "--:--:--";
  struct tm local;
  localtime_r(&event.timestamp, &local);
  char buffer[12];
  strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
  return String(buffer);
}

// ============================================================
// RELÉ
// ============================================================

void relayWrite(bool state) {

  pumpOn = state;

  bool output;

  if (RELAY_ACTIVE_LOW) {
    output = !state;
  } else {
    output = state;
  }

  digitalWrite(
    RELAY_PIN,
    output ? HIGH : LOW
  );
}

// ============================================================
// ESTATÍSTICAS DA BOMBA
// ============================================================

uint64_t liveTotalPumpMillis() {

  uint64_t total =
    totalPumpMillis;

  if (pumpOn) {

    total +=
      (uint32_t)(millis() - pumpStartedAt);
  }

  return total;
}

// ============================================================

void setPump(bool state) {

  // Barreira final comum a HTTP, BLE, nuvem e automação.
  if (state && waterLow) {
    state = false;
  }

  if (state == pumpOn) {
    return;
  }

  if (state) {

    pumpStartedAt =
      millis();

    pumpCycles++;

    relayWrite(true);

    addHistory("Bomba ligada");

  } else {

    if (pumpOn) {

      totalPumpMillis +=
        (uint32_t)(millis() - pumpStartedAt);
    }

    relayWrite(false);

    addHistory("Bomba desligada");
  }

  preferences.begin("hidro", false);
  preferences.putUInt("cycles", pumpCycles);
  preferences.putULong64("totalMs", totalPumpMillis);
  preferences.end();
}

void checkpointPumpTime() {
  const uint32_t now = millis();
  if (!pumpOn || (uint32_t)(now - lastMetricsSaveAt) < 60000UL) return;
  totalPumpMillis += (uint32_t)(now - pumpStartedAt);
  pumpStartedAt = now;
  lastMetricsSaveAt = now;
  preferences.begin("hidro", false);
  preferences.putULong64("totalMs", totalPumpMillis);
  preferences.end();
}

// ============================================================
// MEMÓRIA
// ============================================================

void saveSlots() {

  preferences.begin(
    "hidro",
    false
  );

  for (uint8_t i = 0;
       i < SLOT_COUNT;
       i++) {

    String key =
      "slot" + String(i);

    preferences.putBool(
      (key + "en").c_str(),
      slots[i].enabled
    );

    preferences.putUChar(
      (key + "sh").c_str(),
      slots[i].startHour
    );

    preferences.putUChar(
      (key + "sm").c_str(),
      slots[i].startMinute
    );

    preferences.putUChar(
      (key + "eh").c_str(),
      slots[i].stopHour
    );

    preferences.putUChar(
      (key + "em").c_str(),
      slots[i].stopMinute
    );
  }

  preferences.end();
}

// ------------------------------------------------------------

void loadConfiguration() {

  preferences.begin(
    "hidro",
    true
  );

  for (uint8_t i = 0;
       i < SLOT_COUNT;
       i++) {

    String key =
      "slot" + String(i);

    slots[i].enabled =
      preferences.getBool(
        (key + "en").c_str(),
        false
      );

    slots[i].startHour =
      preferences.getUChar(
        (key + "sh").c_str(),
        8
      );

    slots[i].startMinute =
      preferences.getUChar(
        (key + "sm").c_str(),
        0
      );

    slots[i].stopHour =
      preferences.getUChar(
        (key + "eh").c_str(),
        8
      );

    slots[i].stopMinute =
      preferences.getUChar(
        (key + "em").c_str(),
        15
      );
  }

  pumpCycles = preferences.getUInt("cycles", 0);
  totalPumpMillis = preferences.getULong64("totalMs", 0);

  preferences.end();
}

// ============================================================
// SENSOR DE NÍVEL
// ============================================================

void updateLevel() {

  bool reading =
    digitalRead(LEVEL_PIN)
    == LOW_LEVEL_READING;

  if (reading != levelCandidateLow) {

    levelCandidateLow = reading;

    levelCandidateSince =
      millis();

    return;
  }

  if (
    levelCandidateLow != waterLow &&
    millis() - levelCandidateSince >=
      LEVEL_DEBOUNCE_MS
  ) {

    waterLow =
      levelCandidateLow;

    if (waterLow) {

      addHistory(
        "Nivel de agua baixo"
      );

      // Segurança:
      // não permitir bomba funcionando
      // com nível baixo.
      setPump(false);

    } else {

      addHistory(
        "Nivel de agua normal"
      );
    }
  }
}

// Alarme intermitente não-bloqueante: 250 ms ligado, 750 ms desligado.
void serviceBuzzer() {
  const uint32_t now = millis();
  if (!waterLow) {
    buzzerState = false;
    buzzerPhaseSince = now;
    digitalWrite(BUZZER_PIN, LOW);
    return;
  }

  const uint32_t phaseDuration = buzzerState ? 250UL : 750UL;
  if ((uint32_t)(now - buzzerPhaseSince) >= phaseDuration) {
    buzzerState = !buzzerState;
    buzzerPhaseSince = now;
    digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
  }
}

// ============================================================
// DHT22
// ============================================================

void updateDht() {

  if (
    millis() - lastDhtRead <
    DHT_INTERVAL_MS
  ) {
    return;
  }

  lastDhtRead =
    millis();

  float h =
    dht.readHumidity();

  float t =
    dht.readTemperature();

  if (
    isnan(h) ||
    isnan(t)
  ) {

    dhtOk = false;

    return;
  }

  humidityPct =
    h;

  temperatureC =
    t;

  dhtOk = true;
}

// ============================================================
// HORÁRIOS
// ============================================================

bool scheduleWantsPump() {

  if (!timeIsValid()) {
    return false;
  }

  uint32_t now =
    currentMinutes();

  for (uint8_t i = 0;
       i < SLOT_COUNT;
       i++) {

    if (!slots[i].enabled) {
      continue;
    }

    uint32_t start =
      slots[i].startHour * 60UL +
      slots[i].startMinute;

    uint32_t stop =
      slots[i].stopHour * 60UL +
      slots[i].stopMinute;

    // Horário normal: 08:00 -> 09:00
    if (start < stop) {

      if (
        now >= start &&
        now < stop
      ) {
        return true;
      }

    }

    // Horário atravessando meia-noite:
    // 23:00 -> 01:00
    else if (start > stop) {

      if (
        now >= start ||
        now < stop
      ) {
        return true;
      }

    }

    // start == stop:
    // não liga, evitando intervalo de 24h.
  }

  return false;
}

// ============================================================
// CONTROLE DA BOMBA
// ============================================================

void serviceControl() {

  // Nível baixo sempre tem prioridade.
  if (waterLow) {

    if (pumpOn) {
      setPump(false);
    }

    return;
  }

  // ----------------------------------------------------------
  // MODO MANUAL
  // ----------------------------------------------------------

  if (manualMode) {

    if (
      millis() - manualStartedAt >=
      MANUAL_TIMEOUT_MS
    ) {

      manualMode = false;
      manualRequest = false;

      addHistory(
        "Modo manual expirou"
      );

    } else {

      setPump(
        manualRequest
      );

      return;
    }
  }

  // ----------------------------------------------------------
  // MODO AUTOMÁTICO
  // ----------------------------------------------------------

  bool wanted =
    scheduleWantsPump();

  setPump(wanted);
}

// ============================================================
// WI-FI
// ============================================================

void startAccessPoint() {

  if (apMode) {
    return;
  }

  apMode = true;

  // AP+STA mantém a porta local disponível e segue tentando reconectar
  // à rede salva, sem transformar a nuvem no controlador do cultivo.
  WiFi.mode(WIFI_AP_STA);

  bool result =
    WiFi.softAP(
      "Hidroponia-ESP32",
      AP_PASSWORD
    );

  Serial.println();
  Serial.println(
    "================================"
  );

  Serial.println(
    "MODO AP DO ESP32"
  );

  Serial.println(
    "================================"
  );

  if (result) {

    Serial.print(
      "Rede: Hidroponia-ESP32"
    );

    Serial.println();

    Serial.print(
      "IP do AP: "
    );

    Serial.println(
      WiFi.softAPIP()
    );

  } else {

    Serial.println(
      "Erro ao iniciar AP."
    );
  }
}

// ------------------------------------------------------------

void serviceWiFi() {

  static wl_status_t lastStatus =
    WL_NO_SHIELD;

  wl_status_t status =
    WiFi.status();

  if (status != lastStatus) {

    lastStatus =
      status;

    Serial.print(
      "Status Wi-Fi: "
    );

    Serial.println(
      (int)status
    );
  }

  // Conectado
  if (status == WL_CONNECTED) {
    static bool printed =
      false;

    if (!printed) {

      printed = true;

      Serial.println();
      Serial.println(
        "================================"
      );

      Serial.println(
        "WI-FI CONECTADO!"
      );

      Serial.print(
        "IP do ESP32: "
      );

      Serial.println(
        WiFi.localIP()
      );

      Serial.print(
        "Gateway: "
      );

      Serial.println(
        WiFi.gatewayIP()
      );

      Serial.print(
        "Mascara: "
      );

      Serial.println(
        WiFi.subnetMask()
      );
      Serial.print("RSSI: ");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm");
      if (apMode) Serial.println("AP de emergência continua ativo para acesso local.");

      Serial.println(
        "================================"
      );
    }

    return;
  }

  static bool loggedDisconnected = false;
  if (!loggedDisconnected) {
    Serial.println("Wi-Fi desconectado; automação local permanece ativa.");
    loggedDisconnected = true;
  }
  if (!apMode && (uint32_t)(millis() - wifiStartedAt) >= WIFI_TIMEOUT_MS) startAccessPoint();
  if ((uint32_t)(millis() - lastWifiAttempt) >= WIFI_RETRY_INTERVAL_MS) {
    lastWifiAttempt = millis();
    Serial.println("Nova tentativa de conexão Wi-Fi.");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

// ============================================================
// SESSÃO
// ============================================================

String makeToken() {

  uint32_t a =
    esp_random();

  uint32_t b =
    esp_random();

  char buffer[40];

  snprintf(
    buffer,
    sizeof(buffer),
    "%08lx%08lx",
    (unsigned long)a,
    (unsigned long)b
  );

  return String(buffer);
}

// ------------------------------------------------------------

bool requireAuth(
  bool redirect = false
) {

  if (
    sessionToken.length() == 0
  ) {

    if (redirect) {

      server.sendHeader(
        "Location",
        "/login"
      );

      server.send(
        302,
        "text/plain",
        ""
      );
    } else {

      server.send(
        401,
        "application/json",
        "{\"ok\":false,\"error\":\"Nao autenticado\"}"
      );
    }

    return false;
  }

  if (
    millis() - sessionLastActivity >
    SESSION_TIMEOUT_MS
  ) {

    sessionToken = "";

    if (redirect) {

      server.sendHeader(
        "Location",
        "/login"
      );

      server.send(
        302,
        "text/plain",
        ""
      );

    } else {

      server.send(
        401,
        "application/json",
        "{\"ok\":false,\"error\":\"Sessao expirada\"}"
      );
    }

    return false;
  }

  String cookie =
    server.header("Cookie");

  String expected =
    "HIDROSESSID=" +
    sessionToken;

  if (
    cookie.indexOf(expected) == -1
  ) {

    if (redirect) {

      server.sendHeader(
        "Location",
        "/login"
      );

      server.send(
        302,
        "text/plain",
        ""
      );

    } else {

      server.send(
        401,
        "application/json",
        "{\"ok\":false,\"error\":\"Nao autenticado\"}"
      );
    }

    return false;
  }

  sessionLastActivity =
    millis();

  return true;
}

// ============================================================
// PÁGINA DE LOGIN
// ============================================================

void handleLoginPage() {

  server.send_P(
    200,
    "text/html; charset=utf-8",
    LOGIN_HTML
  );
}

// ============================================================
// LOGIN
// ============================================================

void handleLogin() {

  String user =
    server.arg("user");

  String password =
    server.arg("password");

  if (
    user == WEB_USER &&
    password == WEB_PASSWORD
  ) {

    sessionToken =
      makeToken();

    sessionLastActivity =
      millis();

    server.sendHeader(
      "Set-Cookie",
      "HIDROSESSID=" +
      sessionToken +
      "; HttpOnly; SameSite=Strict; Path=/; Max-Age=28800"
    );

    server.send(
      200,
      "application/json",
      "{\"ok\":true}"
    );

  } else {

    server.send(
      401,
      "application/json",
      "{\"ok\":false,\"error\":\"Credenciais invalidas\"}"
    );
  }
}

// ============================================================
// LOGOUT
// ============================================================

void handleLogout() {

  if (!requireAuth()) {
    return;
  }

  sessionToken =
    "";

  sessionLastActivity =
    0;

  server.sendHeader(
    "Set-Cookie",
    "HIDROSESSID=deleted; HttpOnly; SameSite=Strict; Path=/; Max-Age=0"
  );

  server.send(
    200,
    "application/json",
    "{\"ok\":true}"
  );
}

// ============================================================
// STATUS
// ============================================================

void handleStatus() {

  if (!requireAuth()) {
    return;
  }

  String json;

  json.reserve(3000);

  json +=
    F("{\"ok\":true,\"pump\":");

  json +=
    pumpOn
      ? F("true")
      : F("false");

  json +=
    F(",\"low\":");

  json +=
    waterLow
      ? F("true")
      : F("false");

  json +=
    F(",\"manual\":");

  json +=
    manualMode
      ? F("true")
      : F("false");

  uint32_t remaining = 0;

  if (
    manualMode &&
    (uint32_t)(
      millis() - manualStartedAt
    ) < MANUAL_TIMEOUT_MS
  ) {

    remaining =
      (
        MANUAL_TIMEOUT_MS -
        (
          millis() -
          manualStartedAt
        )
      ) / 1000UL;
  }

  json +=
    F(",\"manualRemaining\":");

  json +=
    remaining;

  json +=
    F(",\"temp\":");

  if (dhtOk) {
    json +=
      String(
        temperatureC,
        1
      );
  } else {
    json +=
      F("null");
  }

  json +=
    F(",\"hum\":");

  if (dhtOk) {
    json +=
      String(
        humidityPct,
        1
      );
  } else {
    json +=
      F("null");
  }

  json +=
    F(",\"cycles\":");

  json +=
    pumpCycles;

  json +=
    F(",\"totalMin\":");

  json +=
    uint64Text(
      liveTotalPumpMillis() /
      60000ULL
    );

  json +=
    F(",\"now\":");

  json +=
    jsonQuote(
      currentTimeText()
    );

  json +=
    F(",\"wifi\":");

  json +=
    WiFi.status() == WL_CONNECTED
      ? F("true")
      : F("false");

  json += F(",\"apMode\":"); json += apMode ? F("true") : F("false");
  json += F(",\"rssi\":"); json += WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : F("null");
  json += F(",\"timeSynced\":"); json += timeIsValid() ? F("true") : F("false");
  json += F(",\"dhtOk\":"); json += dhtOk ? F("true") : F("false");
  json += F(",\"cloud\":"); json += cloudOnline ? F("true") : F("false");
  json += F(",\"ble\":"); json += bleConnected ? F("true") : F("false");

  json +=
    F(",\"ip\":");

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    json +=
      jsonQuote(
        WiFi.localIP().toString()
      );

  } else {

    json +=
      jsonQuote(
        WiFi.softAPIP().toString()
      );
  }

  // ----------------------------------------------------------
  // HORÁRIOS
  // ----------------------------------------------------------

  json +=
    F(",\"slots\":[");

  for (
    uint8_t i = 0;
    i < SLOT_COUNT;
    i++
  ) {

    if (i) {
      json += ',';
    }

    json +=
      F("{\"en\":");

    json +=
      slots[i].enabled
        ? F("true")
        : F("false");

    json +=
      F(",\"on\":");

    json +=
      jsonQuote(
        slotTime(
          slots[i].startHour,
          slots[i].startMinute
        )
      );

    json +=
      F(",\"off\":");

    json +=
      jsonQuote(
        slotTime(
          slots[i].stopHour,
          slots[i].stopMinute
        )
      );

    json +=
      '}';
  }

  json +=
    F("],\"history\":[");

  uint8_t first =
    historyCount < HISTORY_SIZE
      ? 0
      : historyHead;

  for (
    uint8_t i = 0;
    i < historyCount;
    i++
  ) {

    if (i) {
      json += ',';
    }

    const HistoryEvent& event =
      history[
        (first + i) %
        HISTORY_SIZE
      ];

    json +=
      F("{\"time\":");

    json +=
      jsonQuote(
        eventTime(event)
      );

    json +=
      F(",\"text\":");

    json +=
      jsonQuote(
        String(event.text)
      );

    json +=
      '}';
  }

  json +=
    F("]}");

  server.sendHeader(
    "Cache-Control",
    "no-store"
  );

  server.send(
    200,
    "application/json; charset=utf-8",
    json
  );
}

// ============================================================
// PARSER HH:MM
// ============================================================

bool parseHHMM(
  const String& value,
  uint8_t& hour,
  uint8_t& minute
) {

  if (
    value.length() != 5 ||
    value[2] != ':' ||
    !isDigit(value[0]) ||
    !isDigit(value[1]) ||
    !isDigit(value[3]) ||
    !isDigit(value[4])
  ) {

    return false;
  }

  hour =
    (uint8_t)value
      .substring(0, 2)
      .toInt();

  minute =
    (uint8_t)value
      .substring(3, 5)
      .toInt();

  return
    hour <= 23 &&
    minute <= 59;
}

// ============================================================
// HORÁRIOS - API
// ============================================================

void handleSchedules() {

  if (!requireAuth()) {
    return;
  }

  ScheduleSlot candidate[
    SLOT_COUNT
  ];

  memcpy(
    candidate,
    slots,
    sizeof(slots)
  );

  for (
    uint8_t i = 0;
    i < SLOT_COUNT;
    i++
  ) {

    uint8_t h1;
    uint8_t m1;

    uint8_t h2;
    uint8_t m2;

    String onKey =
      "on" + String(i);

    String offKey =
      "off" + String(i);

    String enKey =
      "en" + String(i);

    if (
      !parseHHMM(
        server.arg(onKey),
        h1,
        m1
      ) ||
      !parseHHMM(
        server.arg(offKey),
        h2,
        m2
      )
    ) {

      server.send(
        400,
        "application/json",
        "{\"ok\":false,\"error\":\"Horario invalido\"}"
      );

      return;
    }

    candidate[i].enabled =
      server.hasArg(enKey);

    candidate[i].startHour =
      h1;

    candidate[i].startMinute =
      m1;

    candidate[i].stopHour =
      h2;

    candidate[i].stopMinute =
      m2;
  }

  memcpy(
    slots,
    candidate,
    sizeof(slots)
  );

  saveSlots();

  addHistory(
    "Horarios atualizados"
  );

  server.send(
    200,
    "application/json",
    "{\"ok\":true}"
  );
}

// ============================================================
// CONTROLE - API
// ============================================================

void handleControl() {

  if (!requireAuth()) {
    return;
  }

  String action =
    server.arg("action");
  String error;
  const bool ok = processAction(action.c_str(), JsonVariantConst(), error);
  if (!ok) {
    server.send(409, "application/json", String("{\"ok\":false,\"error\":") + jsonQuote(error) + "}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true,\"confirmed\":true}");
}

// Toda interface chama este executor; o nível baixo é verificado novamente
// dentro de setPump(), que é a barreira final antes de escrever o GPIO.
bool processAction(const char* action, JsonVariantConst payload, String& error) {
  if (strcmp(action, "on") == 0) {
    if (waterLow) { error = "Nivel baixo: comando recusado pelo ESP32"; return false; }
    manualMode = true; manualRequest = true; manualStartedAt = millis();
    addHistory("Modo manual solicitado");
  } else if (strcmp(action, "off") == 0) {
    manualMode = true; manualRequest = false; manualStartedAt = millis();
    addHistory("Modo manual: desligar solicitado");
  } else if (strcmp(action, "auto") == 0) {
    manualMode = false; manualRequest = false;
    addHistory("Modo automatico ativado");
  } else if (strcmp(action, "schedules") == 0) {
    if (!payload.is<JsonArrayConst>() || payload.as<JsonArrayConst>().size() != SLOT_COUNT) { error = "A agenda precisa conter exatamente oito horarios"; return false; }
    ScheduleSlot candidate[SLOT_COUNT];
    for (uint8_t i = 0; i < SLOT_COUNT; ++i) {
      JsonObjectConst item = payload[i].as<JsonObjectConst>();
      uint8_t sh, sm, eh, em;
      if (!item["enabled"].is<bool>() || !parseHHMM(item["start"].as<String>(), sh, sm) || !parseHHMM(item["stop"].as<String>(), eh, em)) { error = "Formato de horario invalido"; return false; }
      candidate[i] = {item["enabled"].as<bool>(), sh, sm, eh, em};
    }
    memcpy(slots, candidate, sizeof(slots)); saveSlots(); addHistory("Programacao atualizada");
  } else { error = "Comando desconhecido"; return false; }
  serviceControl();
  return true;
}

class HydroBleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { bleConnected = true; Serial.println("BLE: cliente conectado."); }
  void onDisconnect(BLEServer* server) override { bleConnected = false; Serial.println("BLE: cliente desconectado; anunciando novamente."); server->startAdvertising(); }
};

class HydroBleCommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    String value = characteristic->getValue();
    const size_t valueLength = value.length();
    if (valueLength == 0 || valueLength >= sizeof(bleCommandBuffer)) return;
    portENTER_CRITICAL(&bleMux);
    memcpy(bleCommandBuffer, value.c_str(), valueLength); bleCommandBuffer[valueLength] = '\0'; bleCommandPending = true;
    portEXIT_CRITICAL(&bleMux);
  }
};

void startBle() {
  BLEDevice::init(BLE_DEVICE_NAME);
  BLEDevice::setMTU(517);
  BLEServer* bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new HydroBleServerCallbacks());
  BLEService* service = bleServer->createService(BLE_SERVICE_UUID);
  bleStatusCharacteristic = service->createCharacteristic(BLE_STATUS_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  bleStatusCharacteristic->addDescriptor(new BLE2902());
  BLECharacteristic* command = service->createCharacteristic(BLE_COMMAND_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  command->setCallbacks(new HydroBleCommandCallbacks());
  bleAckCharacteristic = service->createCharacteristic(BLE_ACK_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  bleAckCharacteristic->addDescriptor(new BLE2902());
  bleAckCharacteristic->setValue("{\"ready\":true}");
  service->start();
  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID); advertising->setScanResponse(true); advertising->start();
  Serial.println("BLE iniciado: Hidroponia-ESP32.");
}

String buildStatusJson() {
  StaticJsonDocument<4096> doc;
  doc["pump"] = pumpOn; doc["low"] = waterLow; doc["manual"] = manualMode;
  doc["temp"] = dhtOk ? temperatureC : NAN; doc["hum"] = dhtOk ? humidityPct : NAN; doc["dhtOk"] = dhtOk;
  doc["cycles"] = pumpCycles; doc["totalMin"] = (uint32_t)(liveTotalPumpMillis() / 60000ULL);
  doc["wifi"] = WiFi.status() == WL_CONNECTED; doc["apMode"] = apMode; doc["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0; doc["timeSynced"] = timeIsValid(); doc["now"] = currentTimeText(); doc["cloud"] = cloudOnline; doc["ble"] = bleConnected;
  // Agenda em tuplas compactas para caber no limite de atributo GATT.
  JsonArray schedule = doc.createNestedArray("slots");
  for (uint8_t i = 0; i < SLOT_COUNT; ++i) { JsonArray s = schedule.createNestedArray(); s.add(slots[i].enabled); s.add(slotTime(slots[i].startHour, slots[i].startMinute)); s.add(slotTime(slots[i].stopHour, slots[i].stopMinute)); }
  String output; serializeJson(doc, output); return output;
}

void serviceBle() {
  if (bleStatusCharacteristic != nullptr) {
    static uint32_t lastStatus = 0;
    if ((uint32_t)(millis() - lastStatus) >= 1000) { lastStatus = millis(); String status = buildStatusJson(); bleStatusCharacteristic->setValue(status.c_str()); }
  }
  if (!bleCommandPending) return;
  char request[sizeof(bleCommandBuffer)];
  portENTER_CRITICAL(&bleMux); strncpy(request, bleCommandBuffer, sizeof(request) - 1); request[sizeof(request) - 1] = '\0'; bleCommandPending = false; portEXIT_CRITICAL(&bleMux);
  DynamicJsonDocument doc(1024); DeserializationError parseError = deserializeJson(doc, request);
  String response; String failure;
  if (parseError || !doc["id"].is<const char*>() || !doc["action"].is<const char*>()) response = "{\"ok\":false,\"error\":\"JSON ou campos invalidos\"}";
  else {
    const String id = doc["id"].as<String>(); const String action = doc["action"].as<String>();
    preferences.begin("hidro", true); String lastBleId = preferences.getString("lastBle", ""); preferences.end();
    bool ok;
    if (lastBleId == id) ok = true; // reenvio BLE: confirma sem executar de novo
    else {
      ok = processAction(action.c_str(), doc["payload"].as<JsonVariantConst>(), failure);
      if (ok) { preferences.begin("hidro", false); preferences.putString("lastBle", id); preferences.end(); }
    }
    StaticJsonDocument<256> ack; ack["id"] = id; ack["ok"] = ok; ack["executed"] = ok; if (!ok) ack["error"] = failure;
    serializeJson(ack, response); Serial.printf("BLE comando %s: %s\n", action.c_str(), ok ? "confirmado" : "recusado");
  }
  if (bleAckCharacteristic != nullptr) { bleAckCharacteristic->setValue(response.c_str()); bleAckCharacteristic->notify(); }
}

bool cloudConfigured() {
  return String(CLOUD_BASE_URL).startsWith("https://") && strcmp(CLOUD_DEVICE_TOKEN, "CONFIGURE_TOKEN_DO_DISPOSITIVO") != 0 && strlen(CLOUD_DEVICE_TOKEN) > 20 && strlen(CLOUD_ROOT_CA) > 100;
}

bool cloudRequest(const char* method, const char* path, const String& body, String& responseBody) {
  WiFiClientSecure client; client.setCACert(CLOUD_ROOT_CA);
  HTTPClient http; http.setTimeout(2500);
  const String url = String(CLOUD_BASE_URL) + path;
  if (!http.begin(client, url)) return false;
  http.addHeader("Authorization", String("Bearer ") + CLOUD_DEVICE_TOKEN);
  http.addHeader("X-Device-ID", CLOUD_DEVICE_ID);
  if (strcmp(method, "GET") != 0) http.addHeader("Content-Type", "application/json");
  int code = strcmp(method, "GET") == 0 ? http.GET() : http.sendRequest(method, body);
  responseBody = http.getString(); http.end();
  return code >= 200 && code < 300;
}

String buildTelemetryJson() {
  StaticJsonDocument<4096> doc;
  doc["pump"] = pumpOn; doc["low"] = waterLow; doc["manual"] = manualMode;
  if (dhtOk) { doc["temp"] = temperatureC; doc["hum"] = humidityPct; } else { doc["temp"] = nullptr; doc["hum"] = nullptr; }
  doc["dhtOk"] = dhtOk; doc["cycles"] = pumpCycles; doc["totalMin"] = (uint32_t)(liveTotalPumpMillis() / 60000ULL);
  doc["wifi"] = WiFi.status() == WL_CONNECTED; doc["apMode"] = apMode; doc["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0; doc["timeSynced"] = timeIsValid(); doc["now"] = currentTimeText();
  JsonArray schedule = doc.createNestedArray("slots");
  for (uint8_t i = 0; i < SLOT_COUNT; ++i) { JsonObject s = schedule.createNestedObject(); s["enabled"] = slots[i].enabled; s["start"] = slotTime(slots[i].startHour, slots[i].startMinute); s["stop"] = slotTime(slots[i].stopHour, slots[i].stopMinute); }
  JsonArray events = doc.createNestedArray("history"); const uint8_t first = historyCount < HISTORY_SIZE ? 0 : historyHead;
  for (uint8_t i = 0; i < historyCount; ++i) { const HistoryEvent& event = history[(first + i) % HISTORY_SIZE]; JsonObject e = events.createNestedObject(); e["time"] = eventTime(event); e["text"] = event.text; }
  String body; serializeJson(doc, body); return body;
}

void cloudWorker(void*) {
  uint32_t lastTelemetry = 0, lastCommandPoll = 0;
  for (;;) {
    if (cloudConfigured() && WiFi.status() == WL_CONNECTED) {
      String result;
      RemoteResult ack;
      while (cloudResultQueue && xQueueReceive(cloudResultQueue, &ack, 0) == pdTRUE) {
        StaticJsonDocument<256> body; body["id"] = ack.id; body["stage"] = "executed"; body["ok"] = ack.ok; if (!ack.ok) body["error"] = ack.error;
        String serialized; serializeJson(body, serialized); cloudOnline = cloudRequest("POST", "/api/device/ack", serialized, result);
      }
      const uint32_t now = millis();
      if ((uint32_t)(now - lastTelemetry) >= 15000UL) { lastTelemetry = now; cloudOnline = cloudRequest("POST", "/api/device/telemetry", buildTelemetryJson(), result); }
      if ((uint32_t)(now - lastCommandPoll) >= 4000UL) {
        lastCommandPoll = now;
        if (cloudRequest("GET", "/api/device/commands", "", result)) {
          DynamicJsonDocument response(1400); if (deserializeJson(response, result) == DeserializationError::Ok && !response["command"].isNull()) {
            JsonObjectConst command = response["command"].as<JsonObjectConst>(); RemoteCommand item{};
            strlcpy(item.id, command["id"] | "", sizeof(item.id)); strlcpy(item.action, command["action"] | "", sizeof(item.action));
            serializeJson(command["payload"], item.payload, sizeof(item.payload));
            if (item.id[0] && cloudCommandQueue && xQueueSend(cloudCommandQueue, &item, 0) == pdTRUE) {
              StaticJsonDocument<192> received; received["id"] = item.id; received["stage"] = "received"; received["ok"] = true; String serialized; serializeJson(received, serialized);
              cloudOnline = cloudRequest("POST", "/api/device/ack", serialized, result);
            }
          }
          cloudOnline = true;
        } else cloudOnline = false;
      }
    } else cloudOnline = false;
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

void serviceCloudCommands() {
  if (!cloudCommandQueue) return;
  RemoteCommand command;
  if (xQueueReceive(cloudCommandQueue, &command, 0) != pdTRUE) return;
  RemoteResult result{}; strlcpy(result.id, command.id, sizeof(result.id));
  preferences.begin("hidro", true); String lastId = preferences.getString("lastCmd", ""); preferences.end();
  if (lastId == command.id) { result.ok = true; }
  else {
    DynamicJsonDocument payload(1024); DeserializationError parseError = deserializeJson(payload, command.payload);
    String error; result.ok = !parseError && processAction(command.action, payload.as<JsonVariantConst>(), error);
    if (!result.ok) strlcpy(result.error, parseError ? "Payload remoto invalido" : error.c_str(), sizeof(result.error));
    preferences.begin("hidro", false); preferences.putString("lastCmd", command.id); preferences.end();
  }
  if (cloudResultQueue) xQueueSend(cloudResultQueue, &result, 0);
  Serial.printf("Nuvem comando %s: %s\n", command.action, result.ok ? "executado" : "recusado");
}

// ============================================================
// ROTAS DO SERVIDOR
// ============================================================

void setupRoutes() {

  const char* headerKeys[] =
    {"Cookie"};

  server.collectHeaders(
    headerKeys,
    1
  );

  server.on(
    "/login",
    HTTP_GET,
    handleLoginPage
  );

  server.on(
    "/api/login",
    HTTP_POST,
    handleLogin
  );

  server.on(
    "/api/logout",
    HTTP_POST,
    handleLogout
  );

  server.on(
    "/",
    HTTP_GET,
    []() {

      if (
        requireAuth(true)
      ) {

        server.send_P(
          200,
          "text/html; charset=utf-8",
          INDEX_HTML
        );
      }
    }
  );

  server.on(
    "/api/status",
    HTTP_GET,
    handleStatus
  );

  server.on(
    "/api/control",
    HTTP_POST,
    handleControl
  );

  server.on(
    "/api/schedules",
    HTTP_POST,
    handleSchedules
  );

  server.on("/ping", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain; charset=utf-8", "pong");
  });

  server.on("/teste", HTTP_GET, []() {
    String body = "<!doctype html><meta charset=utf-8><title>Teste ESP32</title><h1>Servidor HTTP ativo</h1><p>Wi-Fi: ";
    body += WiFi.status() == WL_CONNECTED ? "conectado" : (apMode ? "AP de emergencia" : "desconectado");
    body += "</p><p>IP: "; body += WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    body += "</p><p>Servidor iniciou e recebeu esta requisicao.</p>";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", body);
  });

  server.onNotFound(
    []() {

      if (
        requireAuth(true)
      ) {

        server.send(
          404,
          "text/plain",
          "Nao encontrado"
        );
      }
    }
  );

  server.begin();

  Serial.println(
    "Servidor HTTP iniciado."
  );
}

// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  Serial.println();
  Serial.println(
    "================================"
  );

  Serial.println(
    " SISTEMA DE HIDROPONIA - ESP32"
  );

  Serial.println(
    "================================"
  );

  // ----------------------------------------------------------
  // GPIO
  // ----------------------------------------------------------

  pinMode(
    RELAY_PIN,
    OUTPUT
  );

  // Fail-safe:
  // bomba começa desligada.
  relayWrite(false);

  pinMode(
    LEVEL_PIN,
    INPUT_PULLUP
  );

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  levelCandidateLow =
    (
      digitalRead(LEVEL_PIN) ==
      LOW_LEVEL_READING
    );
  // Inicializa bloqueado se a boia já indicar nível baixo.
  waterLow = levelCandidateLow;

  levelCandidateSince =
    millis();

  // ----------------------------------------------------------
  // CONFIGURAÇÃO
  // ----------------------------------------------------------

  loadConfiguration();

  // ----------------------------------------------------------
  // DHT
  // ----------------------------------------------------------

  dht.begin();

  // ----------------------------------------------------------
  // WI-FI
  // ----------------------------------------------------------

  WiFi.persistent(false);

  WiFi.mode(
    WIFI_STA
  );

  WiFi.setAutoReconnect(
    true
  );

  Serial.print(
    "Conectando ao Wi-Fi: "
  );

  Serial.println(
    WIFI_SSID
  );

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );

  wifiStartedAt =
    millis();

  lastWifiAttempt =
    millis();

  // ----------------------------------------------------------
  // NTP
  // ----------------------------------------------------------

  configTzTime("BRT3", "pool.ntp.org", "time.nist.gov", "a.st1.ntp.br");

  // ----------------------------------------------------------
  // SERVIDOR
  // ----------------------------------------------------------

  setupRoutes();

  startBle();
  cloudCommandQueue = xQueueCreate(2, sizeof(RemoteCommand));
  cloudResultQueue = xQueueCreate(4, sizeof(RemoteResult));
  if (cloudCommandQueue && cloudResultQueue) {
    xTaskCreatePinnedToCore(cloudWorker, "hydro-cloud", 10000, nullptr, 1, &cloudTaskHandle, 0);
  }

  Serial.println();
  Serial.println(
    "Sistema iniciado."
  );

  Serial.println(
    "Aguardando conexao Wi-Fi..."
  );
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  // Wi-Fi
  serviceWiFi();

  // Sensores
  updateLevel();
  serviceBuzzer();

  updateDht();

  // Controle
  serviceControl();
  checkpointPumpTime();
  serviceBle();
  serviceCloudCommands();

  // Servidor
  server.handleClient();

}
