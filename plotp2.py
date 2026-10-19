import math
import matplotlib.pyplot as plt
import pandas as pd
import statistics

df = pd.read_csv("samplesp2.csv")
df["mbps"] = df["goodput_bps"] / 1e6

rep_host = statistics.mode(df["host"])
g = df[df["host"] == rep_host]

#Time series
fig, axs = plt.subplots(1, 4, sharey = True)
datatypes = ["mbps", "snd_cwnd", "rtt", "lost"]
colors = ["blue", "red", "yellow", "green"]

for ax, datatype, color in zip(axs, datatypes, colors):
    ax.scatter(g[datatype], g["t_s"], edgecolors="black", color=color)
    match datatype:
        case "mbps":
            ax.set_xlabel("Goodput (mbps)")
            ax.set_title(f"{rep_host}: goodput over time")
        case "snd_cwnd":
            ax.set_xlabel("Congestion Window")
            ax.set_title(f"{rep_host}: snd_cwnd over time")
        case "rtt":
            ax.set_xlabel("Round Trip Time")
            ax.set_title(f"{rep_host}: rtt over time")
        case "lost":
            ax.set_xlabel("Lost Packets")
            ax.set_title(f"{rep_host}: lost packets over time")
axs[0].set_ylabel("Time (seconds)")
plt.tight_layout()
plt.savefig("plotsp2.png")

#Scatterplots
fig, axs = plt.subplots(1, 3, sharey = True)
datatypes = ["snd_cwnd", "rtt", "lost"]
colors = ["blue", "red", "yellow"]

for ax, datatype, color in zip(axs, datatypes, colors):
    ax.scatter(g[datatype], g["mbps"], edgecolors="black", color=color)
    match datatype:
        case "snd_cwnd":
            ax.set_xlabel("Congestion Window")
            ax.set_title("snd_cwnd vs. goodput")
        case "rtt":
            ax.set_xlabel("Round Trip Time")
            ax.set_title("rtt vs. goodput")
        case "lost":
            ax.set_xlabel("Lost Packets")
            ax.set_title("lost packets vs. goodput")
axs[0].set_ylabel("Goodput (mbps)")
plt.tight_layout()
plt.savefig("goodput_comps.png")