# Testes do firmware no PC

Esta pasta roda o `alerta_smarttag_rede.ino` **de verdade**, sem placa: o Bluetooth, o Wi-Fi, o Telegram e o relógio são substituídos por versões simuladas (`stubs/`), e o `harness.cpp` injeta leituras de RSSI sintéticas em cenários (tag parada, saída gradual, corte brusco, troca de MAC, Telegram fora do ar, envio lento, sinal oscilando no limite etc.). O programa roda com o AddressSanitizer e o UBSanitizer ligados.

```bash
cd testes
./rodar_testes.sh                   # todos os cenários
./demo.sh                           # só os dois cenários da demonstração ao vivo, com a saída resumida
./rodar_testes.sh corte_medio       # só um
```

Precisa de `g++` com suporte a C++17 (Linux ou macOS; no Windows, use o WSL).

**O que isto mostra e o que não mostra.** Mostra que a lógica de decisão (evidência acumulada, silêncio, troca de MAC, fila de mensagens, anti-spam) se comporta como descrito nos cenários. Não mostra como o rádio real se comporta: o ruído e as falhas de recepção são modelos escolhidos por nós. O consumo de memória também só se confirma na placa (comando `i` no Serial Monitor).

Os dois cenários da demonstração (`demo_1_dentro` e `demo_2_saida`) também são refeitos na placa. O passo a passo está em `../docs/demonstracao_ao_vivo.md`.
