# Entrypoint script for Part 2
from pathlib import Path
import sys
import iperf_client

cong_algs = ["cubic", "reno", "bbr"]

def main():
	for alg in cong_algs:
		sys.argv = ["iperf_client.py", "-congestion", f"{alg}"]
		iperf_client.main()
		with open("goodput_plot.py", "r") as file:
			exec(file.read())
			# change file names to not be overwritten by subsequent cong algs
			old = Path("selected_destinations.csv")
			new = Path(f"selected_destinations_{alg}.csv")
			old.rename(new)
			old = Path("samples.csv")
			new = Path(f"samples_{alg}.csv")
			old.rename(new)
			old = Path("samplesp2.csv")
			new = Path(f"samplesp2_{alg}.csv")
			old.rename(new)
			old = Path("goodput_summary.csv")
			new = Path(f"goodput_summary_{alg}.csv")
			old.rename(new)
			old = Path("goodput.png")
			new = Path(f"goodput_{alg}.png")
			old.rename(new)

if __name__ == "__main__":
	main()
