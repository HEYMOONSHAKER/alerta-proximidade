#!/bin/bash
# Demonstracao: roda no PC os dois cenarios que tambem se refazem na placa (veja ../docs/demonstracao_ao_vivo.md).
#   Cenario 1: tag parada dentro, 3 min  -> nenhum alerta
#   Cenario 2: tag dentro, sai pelo portao, fica fora, volta -> alerta e normalizacao
# As linhas impressas tem o mesmo texto que a placa mostra no Serial Monitor (com o tempo simulado entre colchetes).
set -u
cd "$(dirname "$0")"
FW=../firmware/alerta_smarttag_rede
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT
cp "$FW/alerta_smarttag_rede.ino" "$B/"
sed 's#^\(\#define MQTT_PREFIXO\).*#\1  "teste/alerta"#' "$FW/segredos.exemplo.h" > "$B/segredos.h"
g++ -std=c++17 -O1 -Wno-format -I stubs -DINO_FILE="\"$B/alerta_smarttag_rede.ino\"" harness.cpp -o "$B/h" || exit 1

mostra() {  # cenario titulo
  echo
  echo "=================================================================="
  echo "$2"
  echo "=================================================================="
  "$B/h" -v "$1" 2>&1 | awk '
    /TRAVADO|>>> ALERTA|<<< ESTADO|\[HTTP\] POST|Sem leitura/ { print; next }
    /Bruto:/ { t = substr($0, 2, 9) + 0; if (t - ult >= 20) { print; ult = t } next }
    /^OK|^FALHA|FALHOU|cruzou o limiar/ { print }
  '
}
mostra demo_1_dentro "CENARIO 1: tag parada dentro por 3 min (esperado: nenhum alerta, 2 mensagens)"
mostra demo_2_saida  "CENARIO 2: dentro 60 s, sai em 20 s, fora 60 s, volta em 20 s, dentro 40 s (esperado: 1 alerta, 1 normalizacao)"
