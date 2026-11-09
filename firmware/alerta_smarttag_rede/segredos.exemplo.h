// segredos.exemplo.h - modelo das configuracoes pessoais (rede Wi-Fi, Telegram, MQTT).
//
// COMO USAR: copie este arquivo para "segredos.h" (na mesma pasta do .ino) e preencha.
//
// ATENCAO: depois de preencher, NAO suba este arquivo para o GitHub.
// Quem tem o token do bot pode mandar mensagens como se fosse o seu bot.
// O "segredos.h" ja esta no .gitignore deste repositorio.

#pragma once

// ---------- Wi-Fi (precisa ser rede de 2,4 GHz; o ESP32 nao usa 5 GHz) ----------
#define WIFI_SSID  "NOME_DA_SUA_REDE"
#define WIFI_SENHA "SENHA_DA_SUA_REDE"

// Nome do sistema (reservado; as mensagens atuais nao usam este prefixo)
#define NOME_SISTEMA "Alerta de proximidade"

// ---------- Telegram (1 = ligado, 0 = desligado) ----------
#define USAR_TELEGRAM 1
#define TELEGRAM_TOKEN   "COLE_AQUI_O_TOKEN_DO_BOTFATHER"
#define TELEGRAM_CHAT_ID "COLE_AQUI_O_CHAT_ID"

// ---------- MQTT (1 = ligado, 0 = desligado) ----------
// O broker publico e aberto: qualquer pessoa pode ler as mensagens.
// Troque o prefixo por algo unico seu e nao coloque dados pessoais nas mensagens.
#define USAR_MQTT 0
#define MQTT_SERVIDOR "broker.hivemq.com"
#define MQTT_PORTA    1883
#define MQTT_PREFIXO  "iot-troque-este-nome/alerta-proximidade"
