// Teste no PC do firmware: roda o .ino de verdade com tempo simulado e leituras sinteticas de BLE.
// Uso:  harness [-v] <cenario> [semente]      (um cenario por processo, estado global limpo)
//       harness replay <arquivo_de_leituras> <T_segundos>   (imprime so os eventos)
#include "Arduino.h"
#include <random>
#include <fstream>
#include <sstream>
#include <map>

uint64_t g_ms = 0;
uint64_t g_base = 0;
std::vector<Cmd> g_cmds;
size_t g_nextCmd = 0;
bool g_atLineStart = true;
bool g_quiet = false;
std::vector<std::string> g_serialLog;
std::vector<Pkt> g_pkts;
size_t g_nextPkt = 0;
std::function<void()> g_pumpNet;
std::function<void()> g_delayHook;
int g_scanCalls = 0, g_clearCalls = 0;
TaskFunction_t g_taskFn = nullptr;
void *g_taskArg = nullptr;
int g_taskStack = 0;
std::function<bool(uint64_t)> g_wifiUp = [](uint64_t t) { return t >= g_base + 3000; };
std::function<bool(uint64_t)> g_mqttUp = [](uint64_t) { return true; };
std::function<int(uint64_t)> g_httpCode = [](uint64_t) { return 200; };
std::vector<SentMsg> g_sent;
uint64_t g_postMs = 0;
std::vector<std::string> g_mqttPub;
HardwareSerial Serial;
WiFiClass WiFi;

#include INO_FILE

// ------------------------------------------------------------------ utilidades de teste
static std::mt19937 rng(7);
static const char *FD5A = "0000fd5a-0000-1000-8000-00805f9b34fb";
static const char *A = "aa:aa:aa:aa:aa:01", *B = "bb:bb:bb:bb:bb:02", *C = "cc:cc:cc:cc:cc:03";

static double fase(const char *mac, double t0, double t1, double mean, double sigma = 4.5, double perda = 0.0, double per = 2.0,
                   const char *uuid = FD5A) {
  std::normal_distribution<double> nd(0, 1);
  std::uniform_real_distribution<double> jit(-0.1, 0.1), u01(0, 1);
  double ultimo = t0;
  for (double t = t0; t < t1; t += per + jit(rng)) {
    if (perda > 0 && u01(rng) < perda) continue;
    int r = (int)lround(mean + sigma * nd(rng));
    g_pkts.push_back({g_base + (uint64_t)(t * 1000), mac, r, uuid});
    ultimo = t;
  }
  return ultimo;
}
static void rampa(const char *mac, double t0, double t1, double de, double ate, double sigma = 4.5, double per = 2.0) {
  std::normal_distribution<double> nd(0, 1);
  std::uniform_real_distribution<double> jit(-0.1, 0.1);
  for (double t = t0; t < t1; t += per + jit(rng)) {
    double m = de + (ate - de) * (t - t0) / (t1 - t0);
    g_pkts.push_back({g_base + (uint64_t)(t * 1000), mac, (int)lround(m + sigma * nd(rng)), FD5A});
  }
}
static void cmd(double t, const char *s) { g_cmds.push_back({g_base + (uint64_t)(t * 1000), s}); }

struct Ev {
  double t;     // instante do fato (leitura que disparou, ou instante do silencio)
  double tdet;  // instante em que o firmware percebeu
  std::string kind, extra;
};
static std::vector<Ev> g_ev;
static double g_ultimoPacote = 0;
static double rel(uint64_t ms) { return (double)(int64_t)(ms - g_base) / 1000.0; }

static bool prevAlert = false;
static std::string prevMac;
static void observa() {
  bool a = alertando;
  if (a && !prevAlert) {
    double t;
    if (alertaPorSilencio) {
      t = rel(g_ms);
    } else {
      MILLIS_T dif = millis() - ultimaLeitura;
      t = rel(g_ms - (uint64_t)dif);
    }
    g_ev.push_back({t, rel(g_ms), alertaPorSilencio ? "ALERTA_SIL" : "ALERTA_SAIDA", ""});
  } else if (!a && prevAlert) {
    g_ev.push_back({rel(g_ms), rel(g_ms), "NORMALIZA", ""});
  }
  prevAlert = a;
  std::string m = macTag;
  if (m != prevMac) {
    const char *k = prevMac.empty() ? "TRAVA" : (m.empty() ? "DESTRAVA" : "TROCA");
    g_ev.push_back({rel(g_ms), rel(g_ms), k, m});
    prevMac = m;
  }
}

static void roda(double fim_s) {
  std::sort(g_pkts.begin(), g_pkts.end(), [](const Pkt &a, const Pkt &b) { return a.t < b.t; });
  std::stable_sort(g_cmds.begin(), g_cmds.end(), [](const Cmd &a, const Cmd &b) { return a.t < b.t; });
  g_ms = g_base;
  setup();
  g_pumpNet = [] {
    if (!g_taskFn) return;
    try {
      g_taskFn(g_taskArg);
    } catch (TaskYield &) {
    }
  };
  uint64_t fim = g_base + (uint64_t)(fim_s * 1000);
  while (g_ms < fim) {
    loop();
    observa();
  }
}

static int g_fail = 0;
#define CHECK(cond, ...)                 \
  do {                                   \
    if (!(cond)) {                       \
      g_fail++;                          \
      printf("  FALHOU: " __VA_ARGS__);  \
      printf("\n");                      \
    }                                    \
  } while (0)

static int contaEv(const char *k) {
  int n = 0;
  for (auto &e : g_ev) n += e.kind == k;
  return n;
}
static const Ev *primeiro(const char *k, double apos = -1e18) {
  for (auto &e : g_ev)
    if (e.kind == k && e.t >= apos) return &e;
  return nullptr;
}
static int contaEnv(const char *txt) {
  int n = 0;
  for (auto &s : g_sent) n += s.text == txt;
  return n;
}
static int contaEnvPre(const char *pre) {
  int n = 0;
  for (auto &s : g_sent) n += s.text.rfind(pre, 0) == 0;
  return n;
}
static int contaLog(const char *trecho) {
  int n = 0;
  for (auto &s : g_serialLog) n += s.find(trecho) != std::string::npos;
  return n;
}
static void listaEv() {
  for (auto &e : g_ev) printf("    ev %-12s t=%8.1f det=%8.1f %s\n", e.kind.c_str(), e.t, e.tdet, e.extra.c_str());
}

static const char *MSG_SAIDA = "ALERTA: Idoso afastou-se do perímetro seguro (Portão).";
static const char *MSG_SIL = "ALERTA: Sinal interrompido (Tag fora de alcance/parede).";
static const char *MSG_SIL_PERTO = "ALERTA: Idoso afastou-se do perímetro seguro (Portão). O sinal da tag sumiu ao se afastar.";
static int contaSil() { return contaEnv(MSG_SIL) + contaEnv(MSG_SIL_PERTO); }
static const char *MSG_VOLTOU = "Normalizado: Tag retornou ao perímetro seguro.";
static const char *MSG_REST = "Normalizado: Sinal da tag restaurado.";

// ------------------------------------------------------------------ cenarios
struct Scn {
  double fim;
  std::function<void()> build;
  std::function<void()> check;
};
static std::map<std::string, Scn> cenarios;

static void defineCenarios() {
  cenarios["dentro_estavel"] = {1800,
    [] { fase(A, 0, 1800, -58); },
    [] {
      CHECK(contaEv("TRAVA") == 1, "esperava 1 trava, vi %d", contaEv("TRAVA"));
      CHECK(contaEv("ALERTA_SAIDA") + contaEv("ALERTA_SIL") == 0, "alerta falso: %d", contaEv("ALERTA_SAIDA") + contaEv("ALERTA_SIL"));
      CHECK(contaEnvPre("ESP32 Online.") == 1, "mensagem inicial");
      CHECK(contaEnv("Monitorando a SmartTag.") == 1, "mensagem de monitoramento");
      CHECK(g_sent.size() == 2, "mensagens enviadas: %zu (esperava 2)", g_sent.size());
      CHECK(g_scanCalls == g_clearCalls, "clearResults a cada janela (%d x %d)", g_scanCalls, g_clearCalls);
    }};

  cenarios["saida_gradual"] = {330,
    [] {
      fase(A, 0, 120, -58);
      rampa(A, 120, 150, -58, -92);
      fase(A, 150, 260, -92);
      rampa(A, 260, 290, -92, -55);
      fase(A, 290, 330, -55);
    },
    [] {
      double cruza = 120 + 30.0 * 10.0 / 34.0;
      const Ev *e = primeiro("ALERTA_SAIDA", 100);
      CHECK(e != nullptr, "nao alertou na saida");
      if (e) {
        double atraso = e->t - cruza;
        printf("  atraso apos cruzar o limiar: %.1f s\n", atraso);
        CHECK(atraso > -5 && atraso < 20, "atraso fora do esperado: %.1f", atraso);
      }
      CHECK(contaEnv(MSG_SAIDA) == 1, "mensagem de saida enviada %d vezes", contaEnv(MSG_SAIDA));
      CHECK(contaEv("ALERTA_SIL") == 0, "alerta por silencio indevido");
      CHECK(contaEv("NORMALIZA") == 1, "normalizacoes: %d", contaEv("NORMALIZA"));
      CHECK(contaEnv(MSG_VOLTOU) == 1, "mensagem de retorno");
      const Ev *n = primeiro("NORMALIZA");
      if (n) CHECK(n->t > 262 && n->t < 300, "normalizou em %.1f", n->t);
    }};

  // corte brusco com leituras constantes: o tempo ate o alerta depende de quao perto do limiar estava
  struct Corte { const char *nome; double nivel; double min, max; };
  static const Corte cortes[] = {{"corte_perto", -66, 6.5, 8.6}, {"corte_medio", -60, 10.5, 12.6}, {"corte_longe", -45, 18.5, 21.0}};
  for (auto &c : cortes) {
    Corte cc = c;
    cenarios[cc.nome] = {420,
      [cc] {
        fase(A, 0, 20, -55, 0.0);                       // trava com a tag perto
        g_ultimoPacote = fase(A, 20, 120, cc.nivel, 0.0);
        fase(A, 300, 430, -55, 0.0);
      },
      [cc] {
        const Ev *e = primeiro("ALERTA_SIL");
        CHECK(e != nullptr, "nao alertou apos o corte");
        if (e) {
          double sil = e->tdet - g_ultimoPacote;
          printf("  silencio ate o alerta: ~%.1f s\n", sil);
          CHECK(sil >= cc.min - 1.5 && sil <= cc.max + 1.5, "silencio %.1f fora de [%.1f, %.1f]", sil, cc.min, cc.max);
        }
        CHECK(contaSil() == 1, "mensagem de silencio %d vezes", contaSil());
        CHECK((cc.nivel - -68.0 < 10.0) ? contaEnv(MSG_SIL_PERTO) == 1 : contaEnv(MSG_SIL) == 1, "texto do alerta de silencio errado para o nivel %.0f", cc.nivel);
        CHECK(contaEv("NORMALIZA") == 1, "normalizacoes %d", contaEv("NORMALIZA"));
        CHECK(contaEnv(MSG_REST) == 1, "mensagem de sinal restaurado");
      }};
  }

  // troca de MAC por rotacao com a tag dentro (nivel -62, margem 6 dB), varias lacunas
  struct Troca { const char *nome; double lacuna; bool esperaAlerta; };
  static const Troca trocas[] = {{"troca_lacuna_2", 2.2, false}, {"troca_lacuna_4", 4.0, false}, {"troca_lacuna_6", 6.0, false},
                                 {"troca_lacuna_8", 8.0, false}, {"troca_lacuna_10", 10.0, false}, {"troca_lacuna_14", 14.0, true}};
  for (auto &tr : trocas) {
    Troca t = tr;
    cenarios[t.nome] = {400,
      [t] {
        double ult = fase(A, 0, 100, -62, 0.0);
        fase(B, ult + t.lacuna, 400, -62, 0.0);
      },
      [t] {
        CHECK(contaEv("TROCA") == 1, "troca de MAC: %d", contaEv("TROCA"));
        int alertas = contaEv("ALERTA_SIL") + contaEv("ALERTA_SAIDA");
        if (t.esperaAlerta) {
          CHECK(alertas == 1, "esperava alerta de silencio (lacuna %.0f s), vi %d", t.lacuna, alertas);
          CHECK(contaEv("NORMALIZA") == 1, "deveria normalizar com o MAC novo");
          CHECK(contaEnv(MSG_REST) == 1, "mensagem de sinal restaurado");
        } else {
          CHECK(alertas == 0, "alerta falso na troca de MAC (lacuna %.0f s): %d", t.lacuna, alertas);
        }
      }};
  }

  // depois de alerta por silencio, o MAC novo volta fraco (fora) e depois forte (dentro)
  cenarios["volta_mac_novo_fraco"] = {400,
    [] {
      fase(A, 0, 100, -62, 0.0);
      fase(B, 200, 260, -66, 0.0);   // abaixo de limiar + histerese (-63): continua em alerta
      fase(B, 260, 400, -55, 0.0);   // forte: normaliza
    },
    [] {
      CHECK(contaEv("ALERTA_SIL") == 1, "alerta de silencio");
      const Ev *n = primeiro("NORMALIZA");
      CHECK(n != nullptr, "nao normalizou");
      if (n) CHECK(n->t > 259 && n->t < 266, "normalizou em %.1f (esperava ~260)", n->t);
      CHECK(contaEv("TROCA") == 1, "troca %d", contaEv("TROCA"));
    }};

  cenarios["so_tag_fraca"] = {2100,
    [] {
      fase(C, 0, 2000, -85, 4.0);
      fase(A, 2000, 2100, -55);
    },
    [] {
      CHECK(contaEv("TRAVA") == 1, "deveria travar so no fim");
      const Ev *t = primeiro("TRAVA");
      if (t) CHECK(t->t > 1999 && t->t < 2005, "travou em %.1f", t->t);
      int n = contaEnvPre("Atenção: nenhuma SmartTag encontrada");
      CHECK(n == 2, "avisos de sem tag: %d (esperava 2: ~90 s e ~1890 s)", n);
      CHECK(contaLog("Procurando: vi uma tag FD5A") > 100, "dicas de procura no serial: %d", contaLog("Procurando: vi uma tag FD5A"));
      bool ordem = g_sent.size() >= 3 && g_sent[1].text.rfind("Atenção", 0) == 0;
      CHECK(ordem, "ordem das mensagens");
    }};

  cenarios["vizinho_forte"] = {300,
    [] {
      fase(A, 0, 300, -60);
      fase(C, 100, 200, -45, 2.0, 0.0, 1.7);
    },
    [] {
      CHECK(contaEv("TRAVA") == 1 && contaEv("TROCA") == 0, "trocou para outra tag (troca=%d)", contaEv("TROCA"));
      CHECK(contaEv("ALERTA_SAIDA") + contaEv("ALERTA_SIL") == 0, "alerta indevido");
    }};

  cenarios["comandos"] = {330,
    [] {
      fase(A, 0, 70, -58);
      fase(A, 70, 330, -48);   // depois do "l" a tag fica 10 dB acima do novo limiar
      cmd(4.5, "l");      // cedo demais
      cmd(40, "i");
      cmd(60, "l");       // com leituras suficientes
      cmd(61, "abc");
      cmd(62, "h");
      cmd(63, "t");
      cmd(100, "p");
      cmd(190, "p");
      cmd(200, "r");
      cmd(250, "L");
      cmd(300, "");       // linha vazia
    },
    [] {
      CHECK(contaLog("Espere a tag travar") >= 1, "aviso de l cedo demais");
      CHECK(contaLog("Limiar definido em") >= 2, "l deveria definir o limiar (vezes: %d)", contaLog("Limiar definido em"));
      CHECK(contaLog("Comando nao reconhecido") == 1, "comando invalido");
      CHECK(contaLog("Comandos (digite") >= 2, "ajuda");
      CHECK(contaEnv("Mensagem de Teste.") == 1, "mensagem de teste");
      CHECK(contaLog("=== Passeio:") == 1, "resumo do passeio");
      CHECK(contaLog("Tag: aa:aa") >= 1, "comando i");
      CHECK(contaEv("DESTRAVA") == 1, "r deveria destravar");
      CHECK(contaEv("TRAVA") == 2, "deveria travar de novo sozinho (travas %d)", contaEv("TRAVA"));
      CHECK(contaEnv("Monitorando a SmartTag.") == 2, "monitorando duas vezes");
      CHECK(contaEv("ALERTA_SAIDA") + contaEv("ALERTA_SIL") == 0, "alerta indevido");
      CHECK(limiarRssi > -75 && limiarRssi < -45, "limiar apos l: %.1f", limiarRssi);
    }};

  cenarios["telegram_403"] = {700,
    [] {
      g_httpCode = [](uint64_t) { return 403; };
      fase(A, 0, 100, -58);
      fase(A, 100, 700, -90);
    },
    [] {
      const Ev *e = primeiro("ALERTA_SAIDA");
      CHECK(e != nullptr, "sem alerta");
      int tent = 0;
      for (auto &s : g_sent) tent += s.text == MSG_SAIDA;
      CHECK(tent == 10, "tentativas do alerta: %d (esperava 10)", tent);
      CHECK(contaLog("Mensagem descartada") >= 1, "descarte registrado");
      // espacamento de 10 s entre tentativas
      uint64_t ant = 0;
      for (auto &s : g_sent)
        if (s.text == MSG_SAIDA) {
          if (ant) CHECK(s.t - ant >= 9900 && s.t - ant <= 11600, "espaco %llu ms", (unsigned long long)(s.t - ant));
          ant = s.t;
        }
    }};

  cenarios["wifi_cai"] = {700,
    [] {
      g_wifiUp = [](uint64_t t) { return t >= g_base + 3000 && !(t >= g_base + 100000 && t < g_base + 400000); };
      fase(A, 0, 120, -58);
      fase(A, 120, 700, -90);
    },
    [] {
      const Ev *e = primeiro("ALERTA_SAIDA");
      CHECK(e != nullptr, "sem alerta");
      bool tentouComWifiCaido = false;
      for (auto &s : g_sent)
        if (s.t >= g_base + 100000 && s.t < g_base + 400000) tentouComWifiCaido = true;
      CHECK(!tentouComWifiCaido, "tentou enviar com o Wi-Fi caido");
      int ok = 0;
      double primeiroEnvio = -1;
      for (auto &s : g_sent)
        if (s.text == MSG_SAIDA && s.code == 200) {
          ok++;
          if (primeiroEnvio < 0) primeiroEnvio = rel(s.t);
        }
      printf("  alerta entregue ao voltar o Wi-Fi em t=%.1f s\n", primeiroEnvio);
      CHECK(ok == 1, "alerta entregue %d vezes", ok);
      CHECK(primeiroEnvio >= 400 && primeiroEnvio < 403, "entrega em %.1f", primeiroEnvio);
      CHECK(contaLog("Fila cheia") == 0, "fila encheu");
    }};

  cenarios["perda_e_rajadas"] = {3600,
    [] {
      // dentro com margem de ~12 dB, 15% de perda aleatoria e uma lacuna de 6 s a cada 5 min
      fase(A, 0, 3600, -56, 4.5, 0.15);
      for (double t = 300; t < 3600; t += 300) {
        for (auto &p : g_pkts)
          if (p.t >= g_base + (uint64_t)(t * 1000) && p.t < g_base + (uint64_t)((t + 6) * 1000)) p.rssi = 9999;
      }
      g_pkts.erase(std::remove_if(g_pkts.begin(), g_pkts.end(), [](const Pkt &p) { return p.rssi == 9999; }), g_pkts.end());
    },
    [] {
      CHECK(contaEv("ALERTA_SAIDA") + contaEv("ALERTA_SIL") == 0, "alertas falsos: %d", contaEv("ALERTA_SAIDA") + contaEv("ALERTA_SIL"));
    }};

  // contador de 32 bits do millis() dando a volta (so vale no build de 32 bits): troca de MAC e silencio atravessando a volta
  struct Volta { const char *nome; double voltaEm; };
  static const Volta voltas[] = {{"rollover_troca", 101.0}, {"rollover_silencio", 165.0}};
  for (auto &v : voltas) {
    Volta vv = v;
    cenarios[vv.nome] = {500,
      [vv] {
        g_base = 4294967296ULL - (uint64_t)(vv.voltaEm * 1000);
        fase(A, 0, 98, -62, 0.0);
        fase(B, 100.5, 160, -62, 0.0);   // troca de MAC perto do instante 101 s
        fase(B, 300, 540, -55, 0.0);     // some e volta (alerta de silencio no meio)
      },
      [vv] {
        CHECK(contaEv("TROCA") == 1, "troca de MAC: %d", contaEv("TROCA"));
        CHECK(contaEv("ALERTA_SIL") == 1, "alerta de silencio: %d", contaEv("ALERTA_SIL"));
        CHECK(contaEv("NORMALIZA") == 1, "normalizacao: %d", contaEv("NORMALIZA"));
        const Ev *e = primeiro("ALERTA_SIL");
        if (e) CHECK(e->t > 168 && e->t < 175, "alerta de silencio em %.1f (esperava ~171)", e->t);
        CHECK(contaSil() == 1, "mensagem de silencio");
      }};
  }

  cenarios["rede_falhando_bloqueia"] = {3600,
    [] {
      g_httpCode = [](uint64_t) { return -1; };
      g_postMs = 4000;  // cada tentativa ocupa o loop por 4 s (tempo de conexao esgotando)
      fase(A, 0, 3600, -55, 4.5, 0.03);
      for (double t = 30; t < 3600; t += 25) cmd(t, "t");  // mantem sempre uma mensagem pendente
    },
    [] {
      int al = contaEv("ALERTA_SAIDA") + contaEv("ALERTA_SIL");
      printf("  tentativas de envio: %zu | alertas falsos: %d\n", g_sent.size(), al);
      CHECK(al == 0, "alertas falsos com a rede travando o loop: %d", al);
      CHECK(g_sent.size() > 100, "o teste deveria ter muitas tentativas (%zu)", g_sent.size());
    }};

  cenarios["envio_lento_realimenta"] = {1800,
    [] {
      g_httpCode = [](uint64_t) { return 200; };
      g_postMs = 25000;  // cada envio segura o loop por 25 s (DNS/TLS lentos)
      fase(A, 0, 1800, -55, 4.5, 0.03);
      cmd(30, "t");  // uma unica mensagem de teste: o resto precisa ser so efeito colateral
    },
    [] {
      int al = contaEv("ALERTA_SAIDA") + contaEv("ALERTA_SIL");
      printf("  envios: %zu | alertas falsos: %d\n", g_sent.size(), al);
      CHECK(al == 0, "alertas falsos causados por envio lento: %d", al);
    }};

  // saida real (corte brusco em t=120 s) enquanto os envios demoram 25 s: o alerta ainda tem que sair
  cenarios["saida_com_envio_lento"] = {600,
    [] {
      g_httpCode = [](uint64_t) { return 200; };
      g_postMs = 8000;  // pior caso realista com os tempos de espera de 3 s + 5 s
      fase(A, 0, 120, -55, 4.5, 0.02);
      cmd(30, "t");
      cmd(114, "t");  // este envio segura o loop de 114 s ate 122 s, atravessando o corte em 120 s
    },
    [] {
      const Ev *e = primeiro("ALERTA_SIL", 100);
      CHECK(e != nullptr, "nao alertou a saida real durante envios lentos");
      if (e) {
        printf("  alerta por silencio em t=%.1f s (corte em 120 s)\n", e->t);
        CHECK(e->t < 165, "alerta tardio demais: %.1f", e->t);
      }
    }};

  // tag parada no limite do portao, com quedas de sinal de 12 s a cada ~100 s, durante 30 min
  cenarios["oscila_no_limite"] = {1800,
    [] {
      fase(A, 0, 20, -55, 3.0, 0.0);
      for (double t = 20; t < 1790; t += 100) {
        fase(A, t, t + 88, -66, 3.0, 0.05);
        // de t+88 a t+100: silencio
      }
    },
    [] {
      int msgSil = contaSil();
      int msgRest = contaEnvPre("Normalizado: Sinal da tag");
      int total = (int)g_sent.size();
      printf("  alertas de silencio (estado): %d | mensagens de 'sinal interrompido': %d | 'restaurado': %d | total de mensagens: %d\n",
             contaEv("ALERTA_SIL"), msgSil, msgRest, total);
      CHECK(msgSil <= 3, "mensagens de sinal interrompido demais: %d", msgSil);
      CHECK(total <= 12, "mensagens demais: %d", total);
    }};

  // oscilacao seguida de saida real: o aviso da saida ainda precisa chegar
  cenarios["oscila_e_sai"] = {900,
    [] {
      fase(A, 0, 20, -55, 3.0, 0.0);
      fase(A, 20, 108, -66, 3.0, 0.02);   // silencio 108..120: oscilacao
      fase(A, 120, 300, -66, 3.0, 0.02);
      // a partir de 300 s a tag some de vez (saida real)
    },
    [] {
      const Ev *e = nullptr;
      for (auto &x : g_ev) if (x.kind == "ALERTA_SIL" && x.t > 250) { e = &x; break; }
      CHECK(e != nullptr, "nao detectou a saida real");
      double tmsg = -1;
      int idx = 0;
      for (auto &sm : g_sent) { idx++; if ((sm.text == MSG_SIL || sm.text == MSG_SIL_PERTO) && sm.t / 1000.0 > 250) { tmsg = sm.t / 1000.0; break; } }
      printf("  estado de alerta em t=%.1f s | mensagem em t=%.1f s\n", e ? e->t : -1.0, tmsg);
      CHECK(tmsg > 0 && tmsg < 300 + 90, "mensagem da saida real atrasou ou nao saiu (%.1f)", tmsg);
    }};

  cenarios["mqtt"] = {400,
    [] {
      fase(A, 0, 100, -58);
      fase(A, 100, 300, -90);
      fase(A, 300, 400, -55);
    },
    [] {
      bool temEstado = false, temAlerta = false, jsonOk = false;
      for (auto &s : g_mqttPub) {
        if (s.rfind("teste/alerta/estado ", 0) == 0) {
          temEstado = true;
          if (s.find("\"rssi\"") != std::string::npos && s.find("\"dist_m\"") != std::string::npos && s.find("\"sem_sinal\"") != std::string::npos) jsonOk = true;
        }
        if (s.rfind("teste/alerta/alerta ", 0) == 0) temAlerta = true;
      }
      CHECK(temEstado && jsonOk, "estado MQTT com as chaves antigas");
      CHECK(temAlerta, "alerta publicado no MQTT");
      CHECK(contaEnv(MSG_SAIDA) == 1, "Telegram junto com MQTT");
    }};

  cenarios["mqtt_fora"] = {400,
    [] {
      g_mqttUp = [](uint64_t) { return false; };
      fase(A, 0, 100, -58);
      fase(A, 100, 300, -90);
    },
    [] {
      CHECK(contaEnv(MSG_SAIDA) == 1, "Telegram deve funcionar com MQTT fora (%d)", contaEnv(MSG_SAIDA));
    }};
}

// ------------------------------------------------------------------ reproducao de leituras (Monte Carlo)
static int replay(const char *arquivo, double T) {
  g_quiet = true;
  std::ifstream in(arquivo);
  if (!in) {
    fprintf(stderr, "sem arquivo %s\n", arquivo);
    return 2;
  }
  double t;
  int r;
  std::string mac;
  while (in >> t >> r >> mac) g_pkts.push_back({(uint64_t)llround(t * 1000), mac, r, FD5A});
  roda(T);
  for (auto &e : g_ev) {
    if (e.kind == "ALERTA_SAIDA" || e.kind == "ALERTA_SIL") printf("EVT %.3f %s\n", e.t, e.kind == "ALERTA_SAIDA" ? "saida" : "sem_sinal");
    if (e.kind == "TRAVA") printf("TRAVA %.3f\n", e.t);
  }
  printf("FIM\n");
  return 0;
}

int main(int argc, char **argv) {
  if (argc >= 4 && std::string(argv[1]) == "replay") return replay(argv[2], atof(argv[3]));
  bool verbose = false;
  int ia = 1;
  if (argc > ia && std::string(argv[ia]) == "-v") {
    verbose = true;
    ia++;
  }
  if (argc <= ia) {
    fprintf(stderr, "uso: %s [-v] cenario [semente]\n", argv[0]);
    return 2;
  }
  std::string nome = argv[ia];
  if (argc > ia + 1) rng.seed((unsigned)atoi(argv[ia + 1]));
  defineCenarios();
  auto it = cenarios.find(nome);
  if (it == cenarios.end()) {
    fprintf(stderr, "cenario desconhecido: %s\n", nome.c_str());
    return 2;
  }
  g_quiet = !verbose;
  it->second.build();
  roda(it->second.fim);
  printf("== %s\n", nome.c_str());
  if (verbose) listaEv();
  it->second.check();
  printf("%s %s\n", g_fail ? "FALHA" : "OK", nome.c_str());
  return g_fail ? 1 : 0;
}
