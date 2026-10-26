"""
Simulação do alerta de proximidade por RSSI (BLE) para idosos com demência.
Estudo dirigido, EPIIOTA - Internet das Coisas.

O que este programa faz
-----------------------
1) Gera leituras de RSSI sintéticas com o modelo de perda de percurso log-distance:
       RSSI(d) = RSSI0 - 10 * n * log10(d) + ruido
   onde o ruido segue uma distribuição normal (média 0, desvio sigma).
2) Suaviza as leituras com média móvel de W amostras e converte o RSSI filtrado em
   distância estimada:  d_est = 10 ** ((RSSI0 - RSSI) / (10 * n)).
3) Dispara o alerta quando a distância estimada passa do limiar.

Saídas (na pasta indicada em --saida, por padrão "saidas")
---------------------------------------------------------
- figura1_calibracao_modelo.png     : RSSI simulado contra o modelo teórico
- figura2_rssi_bruto_filtrado.png   : uma sessão de saída de casa
- figura3_falso_alarme_vs_janela.png: taxa de falso alarme por tamanho de janela
- tabela3_falso_alarme.csv          : os valores da Figura 3, em tabela

Como rodar
----------
    pip install -r requirements.txt
    python simulacao_rssi.py                       # parâmetros do relatório
    python simulacao_rssi.py --sigma 6 --seed 7    # outro ruído e outra semente
    python simulacao_rssi.py --help                # todas as opções

Os dados são sorteados. Com a mesma semente (--seed) o resultado é sempre o mesmo.
Trocando a semente os valores variam um pouco, mas a conclusão (falso alarme cai a perto
de zero com janela de 5 a 8 amostras) se mantém.
"""

import argparse
import csv
import os

import numpy as np
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


def ler_argumentos():
    ap = argparse.ArgumentParser(description="Simulação do alerta de proximidade por RSSI (BLE).")
    ap.add_argument("--rssi0", type=float, default=-50.0, help="RSSI de referência a 1 m, em dBm (padrão: -50)")
    ap.add_argument("--n", type=float, default=2.2, help="expoente de perda de percurso (padrão: 2,2)")
    ap.add_argument("--sigma", type=float, default=4.0, help="desvio padrão do ruído nos testes de falso alarme, em dB (padrão: 4)")
    ap.add_argument("--sigma-sessao", type=float, default=3.0, help="desvio padrão do ruído na sessão de exemplo, em dB (padrão: 3)")
    ap.add_argument("--limiar", type=float, default=4.0, help="limiar de distância, em metros (padrão: 4)")
    ap.add_argument("--janela-exemplo", type=int, default=5, help="janela da média móvel na sessão de exemplo (padrão: 5)")
    ap.add_argument("--janelas", type=int, nargs="+", default=[1, 2, 3, 5, 8, 12, 16, 20],
                    help="tamanhos de janela do teste de falso alarme (padrão: 1 2 3 5 8 12 16 20)")
    ap.add_argument("--repeticoes", type=int, default=300, help="sessões simuladas por janela (padrão: 300)")
    ap.add_argument("--leituras", type=int, default=140, help="leituras por sessão (padrão: 140)")
    ap.add_argument("--seed", type=int, default=42, help="semente do sorteio (padrão: 42)")
    ap.add_argument("--saida", default="saidas", help="pasta onde gravar figuras e tabela (padrão: saidas)")
    return ap.parse_args()


# Parâmetros fixos do cenário de falso alarme (Tabela 2 do relatório)
DIST_PARADO = 2.0   # distância real do idoso parado (m)
VARIACAO = 0.3      # variação natural de movimento (m, uniforme +- VARIACAO)


class Simulacao:
    def __init__(self, a):
        self.a = a
        self.rng = np.random.default_rng(a.seed)
        self.limiar_rssi = a.rssi0 - 10 * a.n * np.log10(a.limiar)   # cerca de -63,2 dBm com o padrão

    def rssi(self, d, sigma):
        """RSSI simulado para distância(s) d, em metros."""
        d = np.maximum(d, 0.1)
        return self.a.rssi0 - 10 * self.a.n * np.log10(d) + self.rng.normal(0, sigma, size=np.shape(d))

    def dist_estimada(self, r):
        """Inverte o modelo: distância (m) a partir do RSSI (dBm)."""
        return 10 ** ((self.a.rssi0 - r) / (10 * self.a.n))

    @staticmethod
    def media_movel(x, w):
        """Média das últimas w leituras (nas primeiras leituras usa as que já existem)."""
        out = np.empty(len(x))
        for i in range(len(x)):
            out[i] = np.mean(x[max(0, i - w + 1): i + 1])
        return out

    # ------------------------- sessão de exemplo: saída de casa -------------------------
    def sessao_exemplo(self, pasta):
        a = self.a
        t = np.arange(0, 90)
        d = np.empty(len(t))
        d[t < 30] = 1.8                                              # em casa
        m = (t >= 30) & (t < 55)
        d[m] = np.linspace(1.8, 6.0, m.sum())                        # caminha até a porta e sai
        m = t >= 55
        d[m] = np.linspace(6.0, 11.0, m.sum())                       # fora de casa
        bruto = self.rssi(d, a.sigma_sessao)
        filtrado = self.media_movel(bruto, a.janela_exemplo)
        acima = self.dist_estimada(filtrado) > a.limiar
        idx = int(np.argmax(acima)) if acima.any() else None

        fig, ax = plt.subplots(figsize=(10, 6))
        ax.axvspan(0, 30, color="#2f7d58", alpha=0.08)
        ax.axvspan(30, 55, color="#c9a227", alpha=0.10)
        ax.axvspan(55, 90, color="#c1432c", alpha=0.08)
        for x, txt in ((15, "Em casa"), (42.5, "Caminha até a porta"), (72.5, "Fora de casa")):
            ax.text(x, -41.5, txt, ha="center", va="top", fontsize=11)
        ax.plot(t, bruto, color="#a9bdbd", lw=1.5, label="RSSI bruto (com ruído)")
        ax.plot(t, filtrado, color="#3a7783", lw=3, label=f"RSSI filtrado (média móvel, W = {a.janela_exemplo})")
        ax.axhline(self.limiar_rssi, color="#c1432c", ls="--", lw=2, label="Limiar de alerta")
        if idx is not None:
            ax.plot(t[idx], filtrado[idx], "o", color="#c1432c", ms=10)
            ax.annotate("ALERTA ENVIADO", (t[idx], filtrado[idx]), (t[idx] + 8, filtrado[idx] - 9),
                        color="#c1432c", fontweight="bold", arrowprops=dict(arrowstyle="-", color="#c1432c"))
        ax.set_xlim(0, 90)
        ax.set_ylim(-85, -40)
        ax.set_xlabel("Tempo (s)")
        ax.set_ylabel("RSSI (dBm)")
        ax.set_title("Como o sistema detecta a saída de casa")
        ax.legend(loc="lower left")
        fig.tight_layout()
        fig.savefig(os.path.join(pasta, "figura2_rssi_bruto_filtrado.png"), dpi=150)
        plt.close(fig)
        return idx

    # ------------------------- falso alarme x janela -------------------------
    def falso_alarme(self, w):
        """Fração de sessões, com o idoso parado a ~2 m, em que o alerta dispara por engano."""
        a = self.a
        falsos = 0
        for _ in range(a.repeticoes):
            d = DIST_PARADO + self.rng.uniform(-VARIACAO, VARIACAO, a.leituras)
            r = self.rssi(d, a.sigma)
            # só considera a média quando já há w leituras (modo "valid")
            filt = np.convolve(r, np.ones(w) / w, mode="valid")
            if (self.dist_estimada(filt) > a.limiar).any():
                falsos += 1
        return falsos / a.repeticoes

    def teste_falso_alarme(self, pasta):
        a = self.a
        taxas = [self.falso_alarme(w) for w in a.janelas]
        with open(os.path.join(pasta, "tabela3_falso_alarme.csv"), "w", newline="", encoding="utf-8") as f:
            wr = csv.writer(f)
            wr.writerow(["janela_W", "taxa_falso_alarme_percent"])
            for w, p in zip(a.janelas, taxas):
                wr.writerow([w, round(100 * p)])
        fig, ax = plt.subplots(figsize=(9, 6))
        ax.plot(a.janelas, taxas, "o-", color="#f28c28", lw=2, ms=8)
        ax.set_xlabel("Tamanho da janela de filtragem (nº de amostras)")
        ax.set_ylabel("Taxa de falso alarme")
        ax.set_title("Falso alarme vs. filtragem do sinal (média móvel)")
        ax.set_ylim(-0.05, 1.05)
        ax.grid(alpha=0.3)
        fig.tight_layout()
        fig.savefig(os.path.join(pasta, "figura3_falso_alarme_vs_janela.png"), dpi=150)
        plt.close(fig)
        return taxas

    # ------------------------- calibração do modelo -------------------------
    def calibracao(self, pasta):
        """Compara leituras simuladas com a curva teórica do modelo, de 0,5 a 8 m."""
        a = self.a
        dist = np.arange(0.5, 8.01, 0.5)
        teorico = a.rssi0 - 10 * a.n * np.log10(dist)
        # rng próprio, para não alterar os sorteios das outras etapas
        rng = np.random.default_rng(a.seed + 1)
        pontos = a.rssi0 - 10 * a.n * np.log10(dist)[:, None] + rng.normal(0, a.sigma, (len(dist), 30))
        fig, ax = plt.subplots(figsize=(9, 6))
        for k in range(pontos.shape[1]):
            ax.plot(dist, pontos[:, k], ".", color="#a9bdbd", ms=4, label="leituras simuladas" if k == 0 else None)
        ax.plot(dist, pontos.mean(axis=1), "o", color="#3a7783", ms=6, label="média das leituras")
        ax.plot(dist, teorico, "-", color="#c1432c", lw=2, label="modelo teórico")
        ax.set_xlabel("Distância (m)")
        ax.set_ylabel("RSSI (dBm)")
        ax.set_title("Calibração: RSSI simulado contra o modelo")
        ax.legend()
        ax.grid(alpha=0.3)
        fig.tight_layout()
        fig.savefig(os.path.join(pasta, "figura1_calibracao_modelo.png"), dpi=150)
        plt.close(fig)


if __name__ == "__main__":
    args = ler_argumentos()
    os.makedirs(args.saida, exist_ok=True)
    sim = Simulacao(args)
    print(f"Limiar de RSSI: {sim.limiar_rssi:.1f} dBm (equivale a {args.limiar} m), semente {args.seed}")
    idx = sim.sessao_exemplo(args.saida)
    print("Sessão de exemplo: alerta no instante", idx, "s" if idx is not None else "(não disparou)")
    taxas = sim.teste_falso_alarme(args.saida)
    print("Janela W  ->  taxa de falso alarme")
    for w, p in zip(args.janelas, taxas):
        print(f"  {w:>2}      ->  {100 * p:5.1f}%")
    sim.calibracao(args.saida)
    print(f"Arquivos gravados em: {os.path.abspath(args.saida)}")
