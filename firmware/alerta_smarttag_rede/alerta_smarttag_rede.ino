// Alerta de proximidade com SmartTag (BLE) - ESP32
// Versao 6.1: acumulador de evidencias, silencio adaptativo e menos memoria para o Telegram.
//
// Como funciona:
//  - O callback do BLE so guarda cada leitura (MAC, RSSI, hora) numa fila.
//  - O loop() processa as leituras e decide o alerta. Cada leitura mais fraca que o
//    limiar soma "evidencia"; leituras boas subtraem. Passou de EVID_ALERTA: alerta.
//  - Se a tag some, o silencio tambem soma evidencia, mais rapido quanto mais perto do
//    limiar o sinal ja estava quando sumiu.
//  - Wi-Fi e Telegram rodam no proprio loop() (veja REDE_EM_TAREFA mais abaixo). O tempo em que
//    a varredura fica parada por causa de um envio lento nao conta como silencio da tag.
//  - Anti-spam: o primeiro "sinal interrompido" avisa na hora. Se outro vier em menos de 10 min,
//    a mensagem so sai se o silencio durar 45 s seguidos; se o sinal voltar antes, nada e enviado.
//  - A mensagem "ESP32 Online." informa o motivo do reinicio (energia, fonte fraca, erro, etc.).
//
// Comandos no Serial Monitor (115200 baud), digite e aperte Enter:
//   l  usa o sinal medio atual como limiar de alerta (tag no ponto onde deve alertar)
//   p  passeio: mede o sinal em varios pontos da casa e compara com o limiar
//   i  mostra o estado (tag, nivel, limiar, evidencia, Wi-Fi, memoria)
//   r  destrava da tag
//   t  envia uma mensagem de teste
//   h  ajuda
//
// Ajustes mais comuns:
//   LIMIAR_FIXO_DBM  nivel (dBm) do ponto onde o alerta deve disparar. Escolha um ponto ANTES
//                    do portao: o alerta leva uns 10 s depois de cruzar esse ponto, e esses
//                    10 s sao metros andados.
//   EVID_ALERTA      25 = alerta mais rapido (mais falsos alarmes) | 35 = padrao | 45 = mais cauteloso

#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <math.h>
#include <esp_system.h>
#include "segredos.h"

#if USAR_MQTT
#include <PubSubClient.h>
#endif

// ---------- Onde roda a rede (Wi-Fi, Telegram, MQTT) ----------
// 0 = no proprio loop(). Usa menos memoria, e a conexao segura do Telegram precisa de uns 50 KB livres.
//     A varredura do Bluetooth pausa durante cada envio (1 a 5 s); as leituras desse intervalo se perdem,
//     o que o alerta tolera.
// 1 = em uma tarefa separada. A varredura nunca pausa, mas gasta uns 8 KB de memoria a mais.
#ifndef REDE_EM_TAREFA
#define REDE_EM_TAREFA 0
#endif
const int PILHA_REDE = 8192;  // so vale com REDE_EM_TAREFA 1

// ---------- Limiar ----------
const float LIMIAR_FIXO_DBM = -68;  // valor que o "l" mostrou; use 0 para usar o limiar padrao do modelo
const float LIMIAR_M = 4.0;         // so vale quando LIMIAR_FIXO_DBM = 0
float rssi0 = -60.3;                // modelo log-distance (so usado no limiar padrao)
float nPerda = 1.40;

// ---------- Decisao do alerta ----------
const float EVID_ALERTA = 35.0;     // evidencia necessaria. Menor = alerta mais rapido e mais falso alarme
const float FOLGA_DB = 2.0;         // so soma evidencia quando a leitura fica mais fraca que (limiar - folga)
const float EVID_CAP_DB = 12.0;     // maximo que uma unica leitura pode somar
const float HISTERESE_DB = 5.0;     // nivel precisa passar de (limiar + histerese) para desfazer o alerta
const int TAM_JANELA = 5;           // mediana de 5 leituras (nivel exibido e usado nas faixas de silencio)

// ---------- Silencio (tag sumiu) ----------
const unsigned long GRACA_SILENCIO_MS = 3000;    // silencio normal ate aqui, sem somar nada
const unsigned long PASSO_SILENCIO_MS = 2000;    // a cada passo seguinte soma evidencia (intervalo do anuncio da tag)
const float SIL_PERTO_DB = 4.0;                  // ultimo nivel a menos de 4 dB do limiar
const float SIL_MEDIO_DB = 10.0;                 // ultimo nivel a menos de 10 dB do limiar
const float SIL_PERTO = 12.0;                    // soma por passo quando o ultimo nivel estava perto do limiar
const float SIL_MEDIO = 8.0;
const float SIL_LONGE = 4.0;                     // soma por passo quando o ultimo nivel estava bem acima
const unsigned long JANELA_ANTIFLAP_MS = 600000;   // um "sinal interrompido" ate 10 min depois de outro e tratado como oscilacao
const unsigned long CONFIRMA_SILENCIO_MS = 45000;  // nesse caso a mensagem so sai se o silencio durar mais que isso
const unsigned long SEM_SINAL_MS = 20000;        // limite absoluto de silencio (antes era 10 s)

// ---------- Travamento e troca de MAC ----------
const float MARGEM_TRAVA_DB = 3.0;               // trava na tag com sinal acima de (limiar + 3 dB)
const unsigned long TROCA_MS = 3000;             // MAC novo so e aceito apos este silencio do antigo
const float TROCA_DB = 12.0;                     // diferenca maxima de nivel para aceitar o MAC novo
const float TROCA_FOLGA_DB = 6.0;                // MAC novo precisa estar acima de (limiar - 6 dB)
const unsigned long AVISO_SEM_TAG_MS = 90000;    // sem tag travada por este tempo: avisa que NAO monitora
const unsigned long REPETE_SEM_TAG_MS = 1800000; // e repete o aviso a cada 30 min

// ---------- Notificacoes ----------
const unsigned long REPETE_ALERTA_MS = 120000;
const unsigned long ESTADO_MS = 10000;
const int FILA_MAX = 6;
const int TAM_MSG = 140;
const int MAX_TENTATIVAS = 10;
const unsigned long ESPERA_TENTATIVA_MS = 10000;

// ---------- Leituras vindas do BLE ----------
struct Leitura {
  unsigned long t;
  int rssi;
  char mac[18];
};
const int RING_TAM = 32;
Leitura ring[RING_TAM];
volatile int ringIni = 0;
volatile int ringFim = 0;
volatile unsigned long ringDescartadas = 0;
portMUX_TYPE muxRing = portMUX_INITIALIZER_UNLOCKED;

void empilha(const char *mac, int rssi, unsigned long t) {
  portENTER_CRITICAL(&muxRing);
  int prox = (ringFim + 1) % RING_TAM;
  if (prox == ringIni) {  // cheia: descarta a mais antiga
    ringIni = (ringIni + 1) % RING_TAM;
    unsigned long n = ringDescartadas;
    ringDescartadas = n + 1;
  }
  ring[ringFim].t = t;
  ring[ringFim].rssi = rssi;
  strncpy(ring[ringFim].mac, mac, sizeof(ring[ringFim].mac) - 1);
  ring[ringFim].mac[sizeof(ring[ringFim].mac) - 1] = '\0';
  ringFim = prox;
  portEXIT_CRITICAL(&muxRing);
}

bool desempilha(Leitura &l) {
  bool ok = false;
  portENTER_CRITICAL(&muxRing);
  if (ringIni != ringFim) {
    l = ring[ringIni];
    ringIni = (ringIni + 1) % RING_TAM;
    ok = true;
  }
  portEXIT_CRITICAL(&muxRing);
  return ok;
}

void limpaRing() {
  portENTER_CRITICAL(&muxRing);
  ringIni = ringFim;
  portEXIT_CRITICAL(&muxRing);
}

void finalizaPasseio();

// ---------- Estado (so o loop() mexe nestas variaveis) ----------
BLEScan *scan;
char macTag[18] = "";  // MAC da tag travada (vazio = ainda nao travou)
volatile bool alertando = false;
bool alertaPorSilencio = false;  // true = o alerta atual veio do silencio da tag (e nao do sinal fraco)

const int HIST_TAM = 10;
int hist[HIST_TAM];
int qtdHist = 0;
int posHist = 0;
volatile float nivel = -100.0;  // mediana das ultimas leituras
float limiarRssi = -68.0;
float evid = 0.0;               // evidencia acumulada de saida
unsigned long ultimaLeitura = 0;
unsigned long proxPasso = 0;    // 0 = silencio ainda nao comecou
unsigned long tInicioProcura = 0;
unsigned long ultimoAvisoSemTag = 0;
bool avisouSemTag = false;
unsigned long ultimoAlerta = 0;
bool teveAlertaSilencio = false;       // ja houve um "sinal interrompido" desde que a placa ligou
unsigned long ultimoAlertaSilencio = 0;
bool msgSilencioPendente = false;      // alerta de silencio ativo, mensagem ainda aguardando confirmacao
unsigned long msgSilencioDesde = 0;
const char *motivoReinicio = "";
unsigned long ultimaDicaProcura = 0;

// Passeio (comando p)
const int PASSEIO_MAX = 90;
int passeio[PASSEIO_MAX];
int nPasseio = 0;
bool passeando = false;

// ---------- Fila de mensagens (preenchida pelo loop, esvaziada pela tarefa de rede) ----------
char fila[FILA_MAX][TAM_MSG];
int filaIni = 0;
int filaFim = 0;
portMUX_TYPE muxFila = portMUX_INITIALIZER_UNLOCKED;
bool feitoTelegram = false;
bool feitoMqtt = false;
int tentativas = 0;
unsigned long proximaTentativa = 0;

#if USAR_MQTT
WiFiClient redeMqtt;
PubSubClient mqtt(redeMqtt);
unsigned long proximaConexaoMqtt = 0;
#endif

void enfileira(const char *msg) {
  bool ok = false;
  portENTER_CRITICAL(&muxFila);
  int prox = (filaFim + 1) % FILA_MAX;
  if (prox != filaIni) {
    strncpy(fila[filaFim], msg, TAM_MSG - 1);
    fila[filaFim][TAM_MSG - 1] = '\0';
    filaFim = prox;
    ok = true;
  }
  portEXIT_CRITICAL(&muxFila);
  if (!ok) Serial.println("Fila cheia. Mensagem descartada.");
}

bool pegaDaFila(char *destino) {
  bool tem = false;
  portENTER_CRITICAL(&muxFila);
  if (filaIni != filaFim) {
    strcpy(destino, fila[filaIni]);
    tem = true;
  }
  portEXIT_CRITICAL(&muxFila);
  return tem;
}

void removeDaFila() {
  portENTER_CRITICAL(&muxFila);
  if (filaIni != filaFim) filaIni = (filaIni + 1) % FILA_MAX;
  portEXIT_CRITICAL(&muxFila);
}

// ---------- Funcoes auxiliares ----------
float distanciaCom(float rssi, float r0, float n) {
  if (n == 0) return 0.0;
  return powf(10.0f, (r0 - rssi) / (10.0f * n));
}

float distanciaM(float rssi) {
  return distanciaCom(rssi, rssi0, nPerda);
}

float limiarPadrao() {
  return rssi0 - (10.0f * nPerda * log10f(LIMIAR_M));
}

void alerta(const char *motivo) {
  Serial.printf(">>> ALERTA DISPARADO: %s <<<\n", motivo);
  char msg[TAM_MSG];
  snprintf(msg, sizeof(msg), "ALERTA: %s", motivo);
  ultimoAlerta = millis();
  enfileira(msg);
}

void alertaFim(const char *texto) {
  Serial.printf("<<< ESTADO NORMALIZADO: %s >>>\n", texto);
  char msg[TAM_MSG];
  snprintf(msg, sizeof(msg), "Normalizado: %s", texto);
  enfileira(msg);
}

bool temFD5A(BLEAdvertisedDevice &d) {
  for (int i = 0; i < d.getServiceDataCount(); i++) {
    if (d.getServiceDataUUID(i).toString().indexOf("fd5a") >= 0) return true;
  }
  return false;
}

// ---------- Historico de leituras ----------
void resetHistorico() {
  qtdHist = 0;
  posHist = 0;
  evid = 0.0;
  proxPasso = 0;
}

void adiciona(int r) {
  hist[posHist] = r;
  posHist = (posHist + 1) % HIST_TAM;
  if (qtdHist < HIST_TAM) qtdHist++;
}

int recente(int i) {  // 0 = a leitura mais nova
  return hist[(posHist - 1 - i + 2 * HIST_TAM) % HIST_TAM];
}

float mediana(int n) {
  if (n > qtdHist) n = qtdHist;
  if (n <= 0) return -100.0;
  int t[HIST_TAM];
  for (int i = 0; i < n; i++) t[i] = recente(i);
  for (int i = 1; i < n; i++) {  // ordenacao por insercao
    int v = t[i];
    int j = i - 1;
    while (j >= 0 && t[j] > v) {
      t[j + 1] = t[j];
      j--;
    }
    t[j + 1] = v;
  }
  if (n % 2 == 1) return (float)t[n / 2];
  return (t[n / 2 - 1] + t[n / 2]) / 2.0f;
}

float media(int n) {
  if (n > qtdHist) n = qtdHist;
  if (n <= 0) return -100.0;
  float s = 0;
  for (int i = 0; i < n; i++) s += recente(i);
  return s / n;
}

// ---------- Decisao ----------
// Texto do alerta de silencio. Se o sinal sumiu quando ja estava perto do limiar, e quase certo que
// a pessoa saiu (o sinal cai de repente atras do portao); se sumiu com o sinal ainda forte, e perda de sinal.
void alertaSilencio() {
  if (nivel - limiarRssi < SIL_MEDIO_DB) alerta("Idoso afastou-se do perímetro seguro (Portão). O sinal da tag sumiu ao se afastar.");
  else alerta("Sinal interrompido (Tag fora de alcance/parede).");
}

void disparaAlerta(bool porSilencio) {
  alertando = true;
  alertaPorSilencio = porSilencio;
  proxPasso = 0;
  if (porSilencio) {
    bool repetido = teveAlertaSilencio && (millis() - ultimoAlertaSilencio < JANELA_ANTIFLAP_MS);
    teveAlertaSilencio = true;
    ultimoAlertaSilencio = millis();
    if (repetido) {
      // Oscilacao: o sinal ja foi e voltou ha pouco. Espera confirmar antes de mandar mensagem.
      msgSilencioPendente = true;
      msgSilencioDesde = millis();
      Serial.printf(">>> Silencio repetido em menos de %lu min: so aviso se durar mais %lu s. <<<\n", JANELA_ANTIFLAP_MS / 60000UL,
                    CONFIRMA_SILENCIO_MS / 1000UL);
    } else {
      alertaSilencio();
    }
  } else alerta("Idoso afastou-se do perímetro seguro (Portão).");
}

void executarReset() {
  macTag[0] = '\0';
  resetHistorico();
  limpaRing();
  alertando = false;
  alertaPorSilencio = false;
  msgSilencioPendente = false;
  avisouSemTag = false;
  tInicioProcura = millis();
  Serial.println(">>> Sistema destravado e buffers limpos. <<<");
}

bool aceitaTrocaMac(const Leitura &l) {
  if ((long)(l.t - ultimaLeitura) < (long)TROCA_MS) return false;
  if (alertando) return l.rssi > limiarRssi + HISTERESE_DB;  // voltou para dentro, com MAC novo
  return fabsf(l.rssi - nivel) <= TROCA_DB && l.rssi > limiarRssi - TROCA_FOLGA_DB;
}

void processaLeitura(const Leitura &l) {
  if (macTag[0] == '\0') {
    // Procurando: so trava em tag com sinal bem acima do limiar (dentro da area segura).
    if (l.rssi < limiarRssi + MARGEM_TRAVA_DB) {
      if (millis() - ultimaDicaProcura > 5000) {
        ultimaDicaProcura = millis();
        Serial.printf("Procurando: vi uma tag FD5A (%s) com %d dBm; preciso de pelo menos %.0f dBm para travar.\n", l.mac, l.rssi,
                      limiarRssi + MARGEM_TRAVA_DB);
      }
      return;
    }
    strcpy(macTag, l.mac);
    resetHistorico();
    alertando = false;
    alertaPorSilencio = false;
    msgSilencioPendente = false;
    ultimaLeitura = l.t;
    Serial.printf("TRAVADO na tag %s (%d dBm)\n", l.mac, l.rssi);
    enfileira("Monitorando a SmartTag.");
  } else if (strcmp(l.mac, macTag) != 0) {
    if (!aceitaTrocaMac(l)) return;
    strcpy(macTag, l.mac);
    Serial.printf("MAC da tag mudou. Seguindo %s (%d dBm)\n", l.mac, l.rssi);
  }

  // Depois de um silencio longo, as leituras antigas nao valem mais.
  if ((long)(l.t - ultimaLeitura) > 10000) qtdHist = 0;
  ultimaLeitura = l.t;
  proxPasso = 0;

  // Silencio repetido que ainda nao tinha gerado mensagem: o sinal voltou, entao era oscilacao. Cancela sem avisar.
  if (msgSilencioPendente) {
    msgSilencioPendente = false;
    alertando = false;
    alertaPorSilencio = false;
    evid = 0.0;
    Serial.println("<<< Sinal voltou antes da confirmacao: era oscilacao, nenhuma mensagem enviada. >>>");
  }

  adiciona(l.rssi);
  nivel = mediana(TAM_JANELA);

  float d = (limiarRssi - FOLGA_DB) - l.rssi;  // positivo = mais fraco que o limiar
  if (d > EVID_CAP_DB) d = EVID_CAP_DB;
  evid += d;
  if (evid < 0) evid = 0;
  if (evid > EVID_ALERTA + EVID_CAP_DB) evid = EVID_ALERTA + EVID_CAP_DB;

  Serial.printf("Bruto: %d dBm | Nivel: %.1f dBm | folga: %+.1f dB | evidencia: %.0f/%.0f%s\n", l.rssi, nivel, nivel - limiarRssi, evid, EVID_ALERTA,
                alertando ? " | ALERTA ATIVO" : "");

  if (passeando && nPasseio < PASSEIO_MAX) {
    passeio[nPasseio++] = (int)lroundf(nivel);
    if (nPasseio >= PASSEIO_MAX) finalizaPasseio();
  }

  if (!alertando && evid >= EVID_ALERTA) {
    disparaAlerta(false);
  } else if (alertando && nivel > limiarRssi + HISTERESE_DB) {
    bool foiSilencio = alertaPorSilencio;
    alertando = false;
    alertaPorSilencio = false;
    evid = 0.0;
    if (foiSilencio && msgSilencioPendente) {
      msgSilencioPendente = false;  // o aviso nem chegou a sair, entao nao ha o que normalizar
      Serial.println("<<< Oscilacao de sinal terminou antes da confirmacao: nenhuma mensagem enviada. >>>");
    } else {
      alertaFim(foiSilencio ? "Sinal da tag restaurado." : "Tag retornou ao perímetro seguro.");
    }
  }
}

// Desconta do silencio o tempo em que a varredura ficou parada por causa da rede.
void compensaTempoCego(unsigned long cego) {
  unsigned long agora = millis();
  if (macTag[0] == '\0') return;
  ultimaLeitura += cego;
  if ((long)(ultimaLeitura - agora) > 0) ultimaLeitura = agora;
  if (proxPasso != 0) proxPasso += cego;
}

// Chamada a cada volta do loop: o silencio da tag tambem e evidencia de saida.
void verificaSilencio(unsigned long agora) {
  if (macTag[0] == '\0' || alertando) return;
  unsigned long silencio = agora - ultimaLeitura;
  if (silencio > SEM_SINAL_MS) {
    disparaAlerta(true);
    return;
  }
  if (silencio <= GRACA_SILENCIO_MS) return;
  if (proxPasso == 0) proxPasso = ultimaLeitura + GRACA_SILENCIO_MS;
  while ((long)(agora - proxPasso) >= 0) {
    float margem = nivel - limiarRssi;
    evid += (margem < SIL_PERTO_DB) ? SIL_PERTO : (margem < SIL_MEDIO_DB) ? SIL_MEDIO : SIL_LONGE;
    Serial.printf("Sem leitura ha %lu s | evidencia: %.0f/%.0f\n", (agora - ultimaLeitura) / 1000UL, evid, EVID_ALERTA);
    proxPasso += PASSO_SILENCIO_MS;
    if (evid >= EVID_ALERTA) {
      disparaAlerta(true);
      return;
    }
  }
}

void repetirAlerta() {
  if (!alertando) return;
  if (msgSilencioPendente && millis() - msgSilencioDesde >= CONFIRMA_SILENCIO_MS) {
    msgSilencioPendente = false;  // o silencio durou: agora sim vale avisar
    alertaSilencio();
    return;
  }
  if (msgSilencioPendente) return;  // ainda aguardando confirmar: nada de repeticao antes disso
  if (millis() - ultimoAlerta < REPETE_ALERTA_MS) return;
  ultimoAlerta = millis();
  enfileira("ALERTA (repetição): Tag fora da área segura ou sem sinal.");
}

// Se nenhuma tag estiver travada, o sistema NAO esta monitorando: o cuidador precisa saber.
void avisoSemTag(unsigned long agora) {
  if (macTag[0] != '\0') return;
  if (agora - tInicioProcura < AVISO_SEM_TAG_MS) return;
  if (avisouSemTag && agora - ultimoAvisoSemTag < REPETE_SEM_TAG_MS) return;
  avisouSemTag = true;
  ultimoAvisoSemTag = agora;
  enfileira("Atenção: nenhuma SmartTag encontrada. O monitoramento NÃO está ativo.");
}

// ---------- Passeio (comando p) ----------
void finalizaPasseio() {
  passeando = false;
  if (nPasseio < 5) {
    Serial.println("Passeio: poucas leituras. Ande pela casa por 1 a 2 minutos e digite p de novo para encerrar.");
    return;
  }
  int t[PASSEIO_MAX];
  for (int i = 0; i < nPasseio; i++) t[i] = passeio[i];
  for (int i = 1; i < nPasseio; i++) {
    int v = t[i];
    int j = i - 1;
    while (j >= 0 && t[j] > v) {
      t[j + 1] = t[j];
      j--;
    }
    t[j + 1] = v;
  }
  int minimo = t[0];
  int p10 = t[nPasseio / 10];
  int mediano = t[nPasseio / 2];
  int maximo = t[nPasseio - 1];
  Serial.printf("=== Passeio: %d leituras de nivel | pior %d | 10%% piores abaixo de %d | mediano %d | melhor %d dBm ===\n", nPasseio, minimo, p10, mediano,
                maximo);
  float folga = (float)p10 - limiarRssi;
  Serial.printf("Limiar atual: %.1f dBm. Folga dos pontos mais fracos de dentro de casa: %+.1f dB.\n", limiarRssi, folga);
  if (folga >= 8) Serial.println("Boa folga: o limiar fica bem abaixo do sinal normal de dentro de casa.");
  else if (folga >= 4) Serial.println("Folga justa: alguns alarmes falsos podem aparecer nos pontos mais fracos.");
  else Serial.println("Folga insuficiente: alarmes falsos provaveis nos pontos mais fracos. Aproxime a placa desses pontos ou ajuste a antena.");
}

// ---------- Rede (roda em tarefa propria) ----------
#if USAR_TELEGRAM
String urlEncode(const char *s) {
  const char *hex = "0123456789ABCDEF";
  String r;
  for (; *s; s++) {
    uint8_t c = (uint8_t)*s;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r += (char)c;
    else {
      r += '%';
      r += hex[c >> 4];
      r += hex[c & 15];
    }
  }
  return r;
}

bool enviarTelegram(const char *msg) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure cliente;
  cliente.setInsecure();
  HTTPClient http;
  String url = String("https://api.telegram.org/bot") + TELEGRAM_TOKEN + "/sendMessage";
  if (!http.begin(cliente, url)) return false;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String corpo = String("chat_id=") + TELEGRAM_CHAT_ID + "&text=" + urlEncode(msg);
  int codigo = http.POST(corpo);
  http.end();
  if (codigo == 200) Serial.println("Telegram: mensagem enviada.");
  else Serial.printf("Telegram: falhou (HTTP %d). Memoria livre %u, maior bloco %u.\n", codigo, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  return codigo == 200;
}
#endif

#if USAR_MQTT
bool publicarMqtt(const char *sufixo, const char *msg) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!mqtt.connected()) {
    if (millis() < proximaConexaoMqtt) return false;
    char id[40];
    snprintf(id, sizeof(id), "esp32-smarttag-%08lX", (unsigned long)ESP.getEfuseMac());
    mqtt.setServer(MQTT_SERVIDOR, MQTT_PORTA);
    mqtt.setSocketTimeout(2);
    if (!mqtt.connect(id)) {
      proximaConexaoMqtt = millis() + 30000;
      return false;
    }
  }
  char topico[96];
  snprintf(topico, sizeof(topico), "%s/%s", MQTT_PREFIXO, sufixo);
  return mqtt.publish(topico, msg);
}

void publicarEstado() {
  static unsigned long ultimo = 0;
  if (millis() - ultimo < ESTADO_MS) return;
  ultimo = millis();
  if (WiFi.status() != WL_CONNECTED) return;
  char json[140];
  if (macTag[0] == '\0') {
    snprintf(json, sizeof(json), "{\"travado\":0}");
  } else {
    float n = nivel;
    snprintf(json, sizeof(json), "{\"travado\":1,\"rssi\":%.1f,\"dist_m\":%.2f,\"alerta\":%d,\"sem_sinal\":%d}", n, distanciaM(n), alertando ? 1 : 0,
             (alertando && alertaPorSilencio) ? 1 : 0);
  }
  publicarMqtt("estado", json);
}
#endif

void processarFila() {
  if (millis() < proximaTentativa) return;
  if (WiFi.status() != WL_CONNECTED) return;  // sem Wi-Fi a fila espera (nao gasta tentativas)
  char msg[TAM_MSG];
  if (!pegaDaFila(msg)) return;

  bool ok = true;
#if USAR_TELEGRAM
  if (!feitoTelegram) feitoTelegram = enviarTelegram(msg);
  ok = ok && feitoTelegram;
#endif
#if USAR_MQTT
  if (!feitoMqtt) {
    bool r = publicarMqtt("alerta", msg);
    feitoMqtt = r || USAR_TELEGRAM;
  }
  ok = ok && feitoMqtt;
#endif

  if (ok) {
    removeDaFila();
    feitoTelegram = false;
    feitoMqtt = false;
    tentativas = 0;
  } else {
    tentativas++;
    if (tentativas >= MAX_TENTATIVAS) {
      Serial.println("Mensagem descartada depois de varias tentativas.");
      removeDaFila();
      feitoTelegram = false;
      feitoMqtt = false;
      tentativas = 0;
    } else {
      proximaTentativa = millis() + ESPERA_TENTATIVA_MS;
    }
  }
}

const char *nomeMotivoReinicio() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "ligada na energia";
    case ESP_RST_EXT: return "botao de reset";
    case ESP_RST_SW: return "reinicio por software";
    case ESP_RST_PANIC: return "erro do programa (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "travou (watchdog)";
    case ESP_RST_BROWNOUT: return "queda de tensao (fonte fraca)";
    case ESP_RST_DEEPSLEEP: return "saiu do sono profundo";
    default: return "motivo desconhecido";
  }
}

void cuidarWifi() {
  static bool estavaConectado = false;
  static bool avisouInicio = false;
  static unsigned long ultimaTentativa = 0;

  bool conectado = (WiFi.status() == WL_CONNECTED);
  if (conectado != estavaConectado) {
    estavaConectado = conectado;
    if (conectado) Serial.printf("Wi-Fi conectado. IP %s\n", WiFi.localIP().toString().c_str());
    else Serial.println("Wi-Fi caiu.");
  }
  if (conectado && !avisouInicio) {
    avisouInicio = true;
    char online[TAM_MSG];
    snprintf(online, sizeof(online), "ESP32 Online. (Reinício: %s)", motivoReinicio);
    enfileira(online);
  }
  if (!conectado && millis() - ultimaTentativa > 20000) {
    ultimaTentativa = millis();
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_SENHA);
  }
}

void servicosRede() {
  cuidarWifi();
#if USAR_MQTT
  if (mqtt.connected()) mqtt.loop();
  publicarEstado();
#endif
  processarFila();
}

#if REDE_EM_TAREFA
TaskHandle_t hRede = nullptr;

void tarefaRede(void *) {
  for (;;) {
    servicosRede();
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
#endif

// ---------- Callback BLE: so guarda a leitura ----------
class MeuCallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) {
    if (!temFD5A(d)) return;
    char mac[18];
    strncpy(mac, d.getAddress().toString().c_str(), sizeof(mac) - 1);
    mac[sizeof(mac) - 1] = '\0';
    empilha(mac, d.getRSSI(), millis());
  }
};

// ---------- Comandos ----------
void ajuda() {
  Serial.println("Comandos (digite e aperte Enter):");
  Serial.println("  l  usa o sinal medio atual como limiar de alerta");
  Serial.println("  p  passeio: mede o sinal dentro de casa (ande por 1 a 2 min e digite p de novo)");
  Serial.println("  i  mostra o estado");
  Serial.println("  r  destrava da tag");
  Serial.println("  t  envia mensagem de teste");
  Serial.println("  h  ajuda");
}

void tratarComandos() {
  if (!Serial.available()) return;
  String linha = Serial.readStringUntil('\n');
  linha.trim();
  if (linha.length() == 0) return;

  char c = linha[0];
  if (c == 'l' || c == 'L') {
    if (macTag[0] == '\0' || qtdHist < TAM_JANELA) {
      Serial.printf("Espere a tag travar e ter %d leituras (agora %d).\n", TAM_JANELA, qtdHist);
    } else {
      limiarRssi = media(HIST_TAM);
      evid = 0.0;
      alertando = false;
      alertaPorSilencio = false;
      msgSilencioPendente = false;
      Serial.printf("Limiar definido em %.1f dBm (media das ultimas %d leituras).\n", limiarRssi, qtdHist < HIST_TAM ? qtdHist : HIST_TAM);
      Serial.printf("Para fixar no codigo: const float LIMIAR_FIXO_DBM = %.1f;\n", limiarRssi);
    }
  } else if (c == 'p' || c == 'P') {
    if (!passeando) {
      if (macTag[0] == '\0') {
        Serial.println("Trave na tag primeiro.");
      } else {
        passeando = true;
        nPasseio = 0;
        Serial.println("Passeio iniciado. Ande com a tag pelos lugares normais da casa. Digite p de novo para encerrar.");
      }
    } else {
      finalizaPasseio();
    }
  } else if (c == 'i' || c == 'I') {
    unsigned long agora = millis();
    Serial.printf("Tag: %s | nivel %.1f dBm | limiar %.1f dBm | evidencia %.0f/%.0f | alerta %s | ultima leitura ha %lu s\n",
                  macTag[0] ? macTag : "(nenhuma)", (float)nivel, limiarRssi, evid, EVID_ALERTA, alertando ? "SIM" : "nao",
                  macTag[0] ? (agora - ultimaLeitura) / 1000UL : 0UL);
    Serial.printf("Wi-Fi: %s | memoria livre %u (minima %u, maior bloco %u) | leituras descartadas %lu\n",
                  WiFi.status() == WL_CONNECTED ? "conectado" : "desconectado", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap(), (unsigned long)ringDescartadas);
#if REDE_EM_TAREFA
    Serial.printf("Pilha livre (menor ja vista): loop %u bytes | rede %u bytes\n", (unsigned)uxTaskGetStackHighWaterMark(NULL),
                  hRede ? (unsigned)uxTaskGetStackHighWaterMark(hRede) : 0u);
#else
    Serial.printf("Pilha livre do loop (menor ja vista): %u bytes\n", (unsigned)uxTaskGetStackHighWaterMark(NULL));
#endif
  } else if (c == 'r' || c == 'R') {
    executarReset();
  } else if (c == 't' || c == 'T') {
    Serial.println("Mensagem de teste enfileirada.");
    enfileira("Mensagem de Teste.");
  } else if (c == 'h' || c == 'H') {
    ajuda();
  } else {
    Serial.println("Comando nao reconhecido. Digite h para ajuda.");
  }
}

void setup() {
  Serial.begin(115200);
  motivoReinicio = nomeMotivoReinicio();
  Serial.printf("Motivo do ultimo reinicio: %s\n", motivoReinicio);
  limiarRssi = (LIMIAR_FIXO_DBM < 0) ? LIMIAR_FIXO_DBM : limiarPadrao();
  resetHistorico();
  tInicioProcura = millis();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_SENHA);

  BLEDevice::init("");
  scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new MeuCallback(), true);  // true = receber todos os anuncios
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);

#if REDE_EM_TAREFA
  xTaskCreatePinnedToCore(tarefaRede, "rede", PILHA_REDE, nullptr, 1, &hRede, 1);
#endif

  Serial.printf("Limiar: %.1f dBm. Aguardando a SmartTag aparecer na area segura (sinal acima de %.0f dBm).\n", limiarRssi, limiarRssi + MARGEM_TRAVA_DB);
  ajuda();
}

void loop() {
  scan->start(1, false);  // janela de 1 s; os anuncios chegam ao callback e vao para a fila
  scan->clearResults();   // libera a memoria da lista de dispositivos vistos
  Leitura l;
  while (desempilha(l)) processaLeitura(l);
  unsigned long agora = millis();
  verificaSilencio(agora);
  repetirAlerta();
  avisoSemTag(agora);
  tratarComandos();
#if !REDE_EM_TAREFA
  // O envio pode segurar o loop por varios segundos (DNS ou TLS lentos), e a varredura fica parada nesse tempo.
  // Esse tempo "cego" nao e silencio da tag: sem este ajuste, um envio lento gerava um falso "sinal interrompido",
  // cuja mensagem causava outro envio lento, e assim por diante.
  unsigned long antesRede = millis();
  servicosRede();
  unsigned long cego = millis() - antesRede;
  if (cego > 300) compensaTempoCego(cego);
#endif
  delay(10);
}
