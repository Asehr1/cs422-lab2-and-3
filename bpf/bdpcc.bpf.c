// SPDX-License-Identifier: GPL-2.0
/*
 * bdpcc: BDP-floor congestion control (Assignment 2 Q3(b), revised).
 *
 * Window dynamics, in course terms:
 *   - Slowstart (cwnd < ssthresh): double cwnd per RTT, until a loss is
 *     detected or RTT shows queueing (q_ratio >= Q_THRESHOLD).
 *   - Congestion avoidance (probing): +1 MSS per RTT.
 *   - Drain: RTT high but no loss -> remove half the window above the BDP.
 *   - Loss (ssthresh callback): mild cut (0.9) if RTT is low, i.e. loss is
 *     probably not congestion; strong cut (0.7) if RTT is high (full buffer).
 *     The cwnd never drops below the BDP estimate (max_bw * min_rtt).
 *
 * No floating point in eBPF, so all ratios are integer/fixed-point.
 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

char _license[] SEC("license") = "GPL";

#define MIN_CWND	10	/* Linux initial window, IW10 (RFC 6928) */
#define Q_NUM		6	/* Q_THRESHOLD = 1.2 = 6/5 */
#define Q_DEN		5
#define BW_SHIFT	16	/* max_bw is packets/us scaled by 2^16 */

/* Kernel functions exported to BPF congestion control (kfuncs). */
extern __u32 tcp_slow_start(struct tcp_sock *tp, __u32 acked) __ksym;
extern void tcp_cong_avoid_ai(struct tcp_sock *tp, __u32 w, __u32 acked) __ksym;
extern __u32 tcp_reno_undo_cwnd(struct sock *sk) __ksym;

/* Per-connection state, stored in inet_csk_ca(sk) (max 104 bytes). */
struct bdpcc {
	__u32 min_rtt_us;	/* min smoothed RTT seen so far */
	__u32 max_bw;		/* max delivery rate seen so far (pkts/us << 16) */
	__u64 round_start_us;	/* start time of current round */
	__u32 round_delivered;	/* tp->delivered at round start */
};

static inline struct tcp_sock *tcp_sk(const struct sock *sk)
{
	return (struct tcp_sock *)sk;
}

static inline struct bdpcc *inet_csk_ca(const struct sock *sk)
{
	return (struct bdpcc *)((struct inet_connection_sock *)sk)->icsk_ca_priv;
}

/* Smoothed RTT (RTT_est, alpha = 1/8): the kernel stores srtt << 3. */
static inline __u32 curr_rtt_us(const struct tcp_sock *tp)
{
	return tp->srtt_us >> 3;
}

/* q_ratio = curr_rtt / min_rtt >= 1.2, without division. */
static inline bool rtt_is_high(const struct bdpcc *ca, __u32 rtt)
{
	if (ca->min_rtt_us == ~0U)
		return false;
	return (__u64)rtt * Q_DEN >= (__u64)ca->min_rtt_us * Q_NUM;
}

/* bdp = max_bw * min_rtt, in packets (MSS units), at least MIN_CWND. */
static inline __u32 bdp_pkts(const struct bdpcc *ca)
{
	__u64 bdp;

	if (ca->min_rtt_us == ~0U)
		return MIN_CWND;
	bdp = ((__u64)ca->max_bw * ca->min_rtt_us) >> BW_SHIFT;
	return bdp < MIN_CWND ? MIN_CWND : (__u32)bdp;
}

/* Same check as tcp_is_cwnd_limited(): don't grow cwnd if we aren't using it. */
static inline bool is_cwnd_limited(const struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);

	if (tp->snd_cwnd < tp->snd_ssthresh)
		return tp->snd_cwnd < 2 * tp->max_packets_out;
	return !!BPF_CORE_READ_BITFIELD(tp, is_cwnd_limited);
}

static inline void set_cwnd(struct tcp_sock *tp, __u32 cwnd)
{
	if (cwnd > tp->snd_cwnd_clamp)
		cwnd = tp->snd_cwnd_clamp;
	tp->snd_cwnd = cwnd;
}

SEC("struct_ops/bdpcc_init")
void BPF_PROG(bdpcc_init, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bdpcc *ca = inet_csk_ca(sk);

	ca->min_rtt_us = ~0U;	/* set from the first round's RTT */
	ca->max_bw = 0;
	ca->round_start_us = tp->tcp_mstamp;
	ca->round_delivered = tp->delivered;
}

/*
 * End of a round (~1 RTT): update_cwnd_size() from the pseudocode, minus the
 * loss branch (that is bdpcc_ssthresh). Runs even when the flow is not
 * cwnd-limited, so min_rtt, max_bw and the drain check keep updating.
 */
static inline void end_round(struct tcp_sock *tp, struct bdpcc *ca,
			     __u32 rtt, __u64 elapsed)
{
	__u32 cwnd = tp->snd_cwnd;
	__u32 bdp, bw;
	bool rtt_high;

	/* curr_bandwidth = delivered / round_length */
	bw = (__u32)(((__u64)(tp->delivered - ca->round_delivered) << BW_SHIFT) / elapsed);
	ca->round_start_us = tp->tcp_mstamp;
	ca->round_delivered = tp->delivered;

	rtt_high = rtt_is_high(ca, rtt);	/* q_ratio uses the old min_rtt */
	if (rtt < ca->min_rtt_us)
		ca->min_rtt_us = rtt;
	if (bw > ca->max_bw)
		ca->max_bw = bw;
	bdp = bdp_pkts(ca);

	if (rtt_high) {
		/* RTT is high, but no loss yet: drain the queue above the BDP. */
		if (cwnd > bdp)
			cwnd -= (cwnd - bdp) >> 1;	/* DRAIN_FACTOR = 2 */
		else
			cwnd = bdp;
		set_cwnd(tp, cwnd);
		tp->snd_ssthresh = tp->snd_cwnd;	/* leave startup */
	}
}

/*
 * Called on every ACK outside of loss recovery. Once per round runs
 * end_round(); on every ACK grows cwnd by slowstart (x2 per RTT) or
 * probing (+1 MSS per RTT), the same way tcp_reno_cong_avoid() does.
 */
SEC("struct_ops/bdpcc_cong_avoid")
void BPF_PROG(bdpcc_cong_avoid, struct sock *sk, __u32 ack, __u32 acked)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bdpcc *ca = inet_csk_ca(sk);
	__u32 rtt = curr_rtt_us(tp);
	__u64 elapsed = tp->tcp_mstamp - ca->round_start_us;

	if (rtt && elapsed >= rtt)
		end_round(tp, ca, rtt, elapsed);

	if (!is_cwnd_limited(sk))
		return;

	if (tp->snd_cwnd < tp->snd_ssthresh) {
		acked = tcp_slow_start(tp, acked);	/* returns leftover ACKs */
		if (!acked)
			return;
	}
	tcp_cong_avoid_ai(tp, tp->snd_cwnd, acked);
}

/*
 * Called when a loss is detected: triple duplicate ACK (fast recovery) or
 * timeout. The kernel brings cwnd down to the returned value.
 */
SEC("struct_ops/bdpcc_ssthresh")
__u32 BPF_PROG(bdpcc_ssthresh, struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bdpcc *ca = inet_csk_ca(sk);
	__u32 cwnd = tp->snd_cwnd;
	__u32 bdp = bdp_pkts(ca);
	__u32 target;

	if (rtt_is_high(ca, curr_rtt_us(tp)))
		target = cwnd * 7 / 10;	/* loss due to full buffer */
	else
		target = cwnd * 9 / 10;	/* non-full buffer loss: mild cut */

	/* BDP floor, but a loss never raises cwnd above its current value */
	if (bdp > cwnd)
		bdp = cwnd;
	if (target < bdp)
		target = bdp;
	return target < 2 ? 2 : target;	/* same minimum as Reno */
}

SEC("struct_ops/bdpcc_undo_cwnd")
__u32 BPF_PROG(bdpcc_undo_cwnd, struct sock *sk)
{
	return tcp_reno_undo_cwnd(sk);
}

SEC(".struct_ops")
struct tcp_congestion_ops bdpcc = {
	.init		= (void *)bdpcc_init,
	.cong_avoid	= (void *)bdpcc_cong_avoid,
	.ssthresh	= (void *)bdpcc_ssthresh,
	.undo_cwnd	= (void *)bdpcc_undo_cwnd,
	.name		= "bdpcc",
};
