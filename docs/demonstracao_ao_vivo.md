# Demonstração ao vivo: a mesma simulação na placa e no Git

Dois cenários, os mesmos nos dois lugares. No Git, o script roda o `.ino` de verdade no PC com leituras de sinal simuladas. Na placa, você refaz o mesmo roteiro com a SmartTag. As linhas que aparecem no terminal do PC têm o mesmo texto das linhas do Serial Monitor da placa, e as mensagens de Telegram são as mesmas.

## 1. Conferir que é o mesmo código

O firmware do repositório (`firmware/alerta_smarttag_rede/alerta_smarttag_rede.ino`, versão 6.1) é o arquivo que foi enviado para a placa. Para conferir, compare o hash do arquivo que você carregou na Arduino IDE:

```
SHA-256: ab6924db8edac5c8eb57b4decc6136b073099dea2399e6dd994b79e5339c0554
```

- Windows (PowerShell): `certutil -hashfile alerta_smarttag_rede.ino SHA256`
- Mac ou Linux: `shasum -a 256 alerta_smarttag_rede.ino`

Se você mudou o valor de `LIMIAR_FIXO_DBM` (a calibração do seu portão), o hash será outro. É esperado: o resto do arquivo é igual. O repositório fica com o valor padrão -68.

## 2. Preparar

- **Placa:** ligada, com o Serial Monitor em 115200, mostrando `TRAVADO na tag ...`, e o limiar calibrado (tag um passo antes do portão, comando `l`).
- **PC (Git):** `g++` instalado (no Windows, use o WSL). Na pasta do repositório: `./testes/demo.sh`. Leva poucos segundos, porque o tempo é simulado.
- **Lado a lado:** Serial Monitor da placa de um lado, o terminal com o `demo.sh` do outro, o Telegram no celular.

## 3. Cenário 1: tag parada dentro (3 min)

Esperado: nenhum alerta e duas mensagens no Telegram.

| | Placa | Git (`./testes/demo.sh`) |
|---|---|---|
| O que fazer | deixe a tag parada dentro de casa, num ponto onde `i` mostre folga positiva de uns 8 a 10 dB, por 3 min | nada, o cenário `demo_1_dentro` roda sozinho |
| Serial | `TRAVADO na tag ...`, depois linhas `Bruto: ... evidencia: 0/35` | as mesmas linhas, com o tempo simulado entre colchetes |
| Telegram | `ESP32 Online. (Reinício: ...)` e `Monitorando a SmartTag.` | as mesmas duas mensagens, mostradas como `[HTTP] POST -> 200` |
| Resultado | nenhuma linha `>>> ALERTA DISPARADO` | `OK demo_1_dentro` |

Se a placa alertar neste cenário, é um achado real: o limiar está perto demais do lugar onde a tag fica, ou a tag está num ponto de sinal ruim. Olhe a folga em `i`.

## 4. Cenário 2: sair pelo portão e voltar

Esperado: um alerta de saída pouco depois de cruzar o limiar e uma normalização na volta.

| Tempo | O que fazer na placa | O que o Git simula |
|---|---|---|
| 0 a 60 s | tag parada dentro | tag dentro, nível de cerca de -58 dBm |
| 60 a 80 s | sair andando pelo portão, em uns 20 s | o nível cai de -58 para -92 dBm |
| 80 a 140 s | ficar fora por 60 s | tag fora, nível de cerca de -92 dBm |
| 140 a 160 s | voltar andando, em uns 20 s | o nível sobe de -92 para -58 dBm |
| 160 a 200 s | ficar dentro | tag dentro de novo |

Sequência esperada nos dois lugares:

1. `>>> ALERTA DISPARADO: Idoso afastou-se do perímetro seguro (Portão). <<<`, cerca de 10 a 15 s depois de cruzar o limiar. No Git com a semente padrão: cruza em ~66 s e alerta em 78,1 s, atraso de 12,2 s.
2. Telegram: `ALERTA: Idoso afastou-se do perímetro seguro (Portão).`
3. Na volta: `<<< ESTADO NORMALIZADO: Tag retornou ao perímetro seguro. >>>`
4. Telegram: `Normalizado: Tag retornou ao perímetro seguro.`

Na placa, se o sinal sumir de uma vez ao passar pelo portão (porta de metal, parede), a mensagem de alerta pode vir como `Idoso afastou-se do perímetro seguro (Portão). O sinal da tag sumiu ao se afastar.` e a normalização como `Sinal da tag restaurado.` O começo é o mesmo: é a saída por silêncio, não por sinal fraco.

## 5. O que é igual e o que não é

- **Igual:** o código, o texto das linhas do Serial e das mensagens, a ordem dos eventos e a ordem de grandeza do atraso (10 a 15 s).
- **Diferente:** os níveis em dBm e os segundos exatos. O Git usa níveis fixos e ruído de 4,5 dB com semente fixa, então o resultado é sempre o mesmo. A placa usa o sinal real do seu portão e varia de uma tentativa para outra. Compare a sequência de eventos, não os números.
- **O que o Git não prova:** que o sinal real do seu portão se comporta como o simulado. Quem prova isso é o cenário 2 feito na placa, repetido várias vezes.
