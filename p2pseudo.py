Record into csv:
1. Timestamp
2. sndcwnd(from tcp_info option) - infostruct.tcpi_snd_cwnd
3. rtt - infostruct.tcpi_rtt
4. lost packets - infostruct.tcpi_lost
5. (optional) rttvar - tcpi_rttvar

import pandas
import matplotlib.pyplot as pyplot

stats = pandas.read_csv("p2stats.csv")

fig, axs = pyplot.subplots(1, 3, sharey = True)
datatypes = ["snd_cwnd", "rtt", "lost"]
colors = ["blue", "red", "yellow"]

for ax, datatype, color in zip(axs, datatypes, colors):
    ax.scatter(stats["goodput"], stats[datatype], edgecolors="black", color=color)
    ax.set_xlabel("Goodput (unit)")
    ax.set_title