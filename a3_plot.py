# Assignment 3, Option 1: time-series and scatter plots (same as A2 Q2(b)) for
# all four algorithms on one representative destination, plus aggregate stats.
import argparse

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd

ALGOS = ["bdpcc", "cubic", "reno", "bbr"]
RUN_COLORS = ["tab:blue", "tab:orange", "tab:green", "tab:red", "tab:purple"]


def load(path):
    df = pd.read_csv(path)
    df["mbps"] = df["goodput_bps"] / 1e6
    df["rtt_ms"] = df["rtt"] / 1000           # tcpi_rtt is in microseconds
    df["min_rtt_ms"] = df["min_rtt"] / 1000
    # loss signal: retransmissions in each sampling interval
    df = df.sort_values(["run", "host", "algo", "t_s"])
    df["retrans"] = df.groupby(["run", "host", "algo"])["total_retrans"].diff().fillna(df["total_retrans"])
    df["timeout"] = (df["ca_state"] == 4).astype(int)
    df["tdack"] = (df["ca_state"] == 3).astype(int)
    return df


def pick_host(df):
    # among destinations with the most successful (algorithm, run) tests,
    # the one with the median base RTT (min RTT), i.e. a mid-length path
    tests = df.groupby("host")[["algo", "run"]].apply(lambda g: len(g.drop_duplicates()))
    complete = tests[tests == tests.max()].index
    base_rtt = df[df["host"].isin(complete)].groupby("host")["min_rtt_ms"].min().sort_values()
    return base_rtt.index[(len(base_rtt) - 1) // 2]


def matched(df):
    # only (run, host) pairs where all four algorithms succeeded
    n = df.groupby(["run", "host"])["algo"].nunique()
    full = n[n == len(ALGOS)].index
    return df.set_index(["run", "host"]).loc[full].reset_index()


def time_series(g, host):
    metrics = [("mbps", "goodput (Mbit/s)"), ("snd_cwnd", "snd_cwnd (packets)"),
               ("rtt_ms", "RTT (ms)"), ("retrans", "retransmissions / interval")]
    fig, axes = plt.subplots(len(metrics), len(ALGOS), figsize=(16, 11),
                             sharex=True, sharey="row")
    for col, algo in enumerate(ALGOS):
        a = g[g["algo"] == algo]
        axes[0, col].set_title(algo)
        for row, (m, label) in enumerate(metrics):
            ax = axes[row, col]
            for run, r in a.groupby("run"):
                ax.plot(r["t_s"], r[m], color=RUN_COLORS[(run - 1) % len(RUN_COLORS)],
                        label=f"run {run}")
            if col == 0:
                ax.set_ylabel(label)
            if row == len(metrics) - 1:
                ax.set_xlabel("time (s)")
    axes[0, 0].legend(fontsize=8)
    fig.suptitle(f"Time series per algorithm, destination {host}")
    fig.tight_layout()
    return fig


def scatter(g, host):
    metrics = [("snd_cwnd", "snd_cwnd (packets)"), ("rtt_ms", "RTT (ms)"),
               ("retrans", "retransmissions / interval")]
    fig, axes = plt.subplots(len(metrics), len(ALGOS), figsize=(16, 9),
                             sharex="row", sharey=True)
    for col, algo in enumerate(ALGOS):
        a = g[g["algo"] == algo]
        axes[0, col].set_title(algo)
        for row, (m, label) in enumerate(metrics):
            ax = axes[row, col]
            ax.scatter(a[m], a["mbps"], s=12, edgecolors="black", linewidths=0.3)
            ax.set_xlabel(label)
            if col == 0:
                ax.set_ylabel("goodput (Mbit/s)")
    fig.suptitle(f"Metric vs goodput per algorithm, destination {host}")
    fig.tight_layout()
    return fig


def p95(x):
    return x.quantile(0.95)   # pandas default: linear interpolation


def stats(df, by):
    s = df.groupby(by).agg(
        tput_min=("mbps", "min"), tput_median=("mbps", "median"),
        tput_mean=("mbps", "mean"), tput_p95=("mbps", p95),
        rtt_min=("rtt_ms", "min"), rtt_median=("rtt_ms", "median"),
        rtt_mean=("rtt_ms", "mean"), rtt_p95=("rtt_ms", p95),
        retrans=("retrans", "sum"), timeout_samples=("timeout", "sum"),
        tdack_samples=("tdack", "sum"), samples=("mbps", "size"),
    ).round(2)
    if by == "algo":
        s = s.reindex([a for a in ALGOS if a in s.index])
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-dir", default="results_a3")
    ap.add_argument("-host", default=None, help="representative destination")
    ap.add_argument("-prefix", default="", help="prefix for plot file names")
    args = ap.parse_args()

    df = load(f"{args.dir}/samples.csv")
    host = args.host or pick_host(df)
    g = df[df["host"] == host]

    for name, fig in [("timeseries", time_series(g, host)), ("scatter", scatter(g, host))]:
        fig.savefig(f"{args.dir}/{args.prefix}{name}.pdf")
        fig.savefig(f"{args.dir}/{args.prefix}{name}.png", dpi=120)

    by_algo = stats(df, "algo")
    by_algo.to_csv(f"{args.dir}/stats_by_algo.csv")
    by_algo_matched = stats(matched(df), "algo")
    by_algo_matched.to_csv(f"{args.dir}/stats_by_algo_matched.csv")
    stats(df, ["algo", "host"]).to_csv(f"{args.dir}/stats_by_algo_host.csv")
    print(f"representative destination: {host}")
    print(by_algo.to_string())
    print("\nmatched (run, host) pairs only:")
    print(by_algo_matched.to_string())


if __name__ == "__main__":
    main()
