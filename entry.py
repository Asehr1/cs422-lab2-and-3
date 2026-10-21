# Entrypoint script for Part 2
from pathlib import Path
import sys
import iperf_client
import random

cong_algs = ["cubic", "reno", "bbr"]

def main():
	seed = random.randint(0, sys.maxsize)
	for alg in cong_algs:
		print(f"\nRunning {alg}")
		sys.argv = ["iperf_client.py", "-congestion", f"{alg}", "-seed", f"{seed}"]
		iperf_client.main()
		with open("goodput_plot.py", "r") as file:
			exec(file.read())
		with open("plotp2.py", "r") as file:
			exec(file.read())
		# change file names to not be overwritten by subsequent cong algs
		old = Path("selected_destinations.csv")
		new = Path(f"selected_destinations_{alg}.csv")
		old.rename(new)
		old = Path("samples.csv")
		new = Path(f"samples_{alg}.csv")
		old.rename(new)
		old = Path("goodput_summary.csv")
		new = Path(f"goodput_summary_{alg}.csv")
		old.rename(new)
		old = Path("goodput.png")
		new = Path(f"goodput_{alg}.png")
		old.rename(new)
		old = Path("plotsp2.png")
		new = Path(f"plotsp2_{alg}.png")
		old.rename(new)
		old = Path("goodput_comps.png")
		new = Path(f"goodput_comps_{alg}.png")
		old.rename(new)
		print(f"Completed {alg}\n")

if __name__ == "__main__":
	main()
