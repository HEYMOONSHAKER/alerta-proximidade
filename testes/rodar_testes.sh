#!/bin/bash
# Roda o firmware (o .ino de verdade) no PC, com tempo simulado e leituras de BLE sinteticas.
# Precisa de g++ (C++17). Uso:  ./rodar_testes.sh            (todos os cenarios)
#                               ./rodar_testes.sh corte_medio saida_gradual   (so alguns)
set -u
cd "$(dirname "$0")"
FW=../firmware/alerta_smarttag_rede
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

# O firmware inclui "segredos.h". Para o teste basta o modelo, copiado para a pasta temporaria,
# com o prefixo MQTT que os cenarios esperam (nada do seu segredos.h real e usado ou alterado).
cp "$FW/alerta_smarttag_rede.ino" "$B/"
sed 's#^\(\#define MQTT_PREFIXO\).*#\1  "teste/alerta"#' "$FW/segredos.exemplo.h" > "$B/segredos.h"
# Versao com contador de tempo de 32 bits, como no ESP32 (testa a volta do contador a cada ~49 dias)
sed -e 's/unsigned long/uint32_t/g' -e 's/(long)/(int32_t)/g' "$B/alerta_smarttag_rede.ino" > "$B/alerta32.ino"
# Versao com MQTT ligado: pasta propria, com o segredos de teste mudado para USAR_MQTT 1
mkdir "$B/mqtt"
cp "$B/alerta_smarttag_rede.ino" "$B/mqtt/"
sed 's/#define USAR_MQTT 0/#define USAR_MQTT 1/' "$B/segredos.h" > "$B/mqtt/segredos.h"

FLAGS="-std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Wno-unused-parameter -Wno-format"
echo "Compilando..."
g++ $FLAGS -I stubs -DINO_FILE="\"$B/alerta_smarttag_rede.ino\"" harness.cpp -o "$B/h64" || exit 1
g++ $FLAGS -I stubs -DINO_FILE="\"$B/mqtt/alerta_smarttag_rede.ino\"" harness.cpp -o "$B/h64m" || exit 1
g++ $FLAGS -DMILLIS_T=uint32_t -I stubs -DINO_FILE="\"$B/alerta32.ino\"" harness.cpp -o "$B/h32" || exit 1

COMUM="dentro_estavel saida_gradual corte_perto corte_medio corte_longe troca_lacuna_2 troca_lacuna_4 troca_lacuna_6 troca_lacuna_8 troca_lacuna_10 troca_lacuna_14 volta_mac_novo_fraco so_tag_fraca vizinho_forte comandos telegram_403 wifi_cai perda_e_rajadas mqtt_fora rede_falhando_bloqueia envio_lento_realimenta saida_com_envio_lento oscila_no_limite oscila_e_sai"
MQTT="dentro_estavel saida_gradual corte_medio wifi_cai telegram_403 mqtt mqtt_fora"
VOLTA="rollover_troca rollover_silencio troca_lacuna_8 corte_medio saida_gradual"
if [ $# -gt 0 ]; then COMUM="$*"; MQTT=""; VOLTA=""; fi

falhas=0
roda() {  # binario cenarios...
  local bin=$1; shift
  for s in "$@"; do
    out=$(timeout 300 "$B/$bin" "$s" 2>&1); rc=$?
    echo "$out" | grep -E "^(OK|FALHA)|FALHOU|atraso|silencio ate|entregue|alertas|mensagens|estado de alerta|ERROR|runtime error"
    [ $rc -ne 0 ] && falhas=$((falhas+1))
  done
}
echo "##### 64 bits, MQTT desligado";  roda h64 $COMUM
[ -n "$MQTT" ]  && { echo "##### 64 bits, MQTT ligado"; roda h64m $MQTT; }
[ -n "$VOLTA" ] && { echo "##### 32 bits (contador de tempo dando a volta)"; roda h32 $VOLTA; }
echo "cenarios com falha: $falhas"
[ $falhas -eq 0 ]
