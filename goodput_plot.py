import math
import matplotlib.pyplot as plt
import pandas as pd

df = pd.read_csv("samples.csv")
df["mbps"] = df["goodput_bps"] / 1e6

#Summary table: min, median, mean, 95th percentile
summary = df.groupby("host")["mbps"].agg(
    min="min",
    median="median",
    mean="mean",
    p95=lambda x: x.quantile(0.95),
).round(2)
print(f"Goodput (Mbit/s)")
print(summary)
summary.to_csv("goodput_summary.csv")


hosts = df["host"].unique()
cols = 3
rows = math.ceil(len(hosts) / cols)
fig, axes = plt.subplots(rows, cols, figsize=(5 * cols, 3 * rows), squeeze=False)
axes = axes.flatten()

for ax, host in zip(axes, hosts):
    g = df[df["host"] == host]
    ax.plot(g["t_s"], g["mbps"])
    ax.set_title(host, fontsize=9)
    ax.set_xlabel("time (s)")
    ax.set_ylabel("goodput (Mbit/s)")

for ax in axes[len(hosts):]:
    ax.axis("off")

fig.suptitle(f"Goodput over time")
fig.tight_layout()
fig.savefig("goodput.png")