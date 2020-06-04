#include <stdlib.h>
#include "ctcp_bbr.h"
#include "ctcp.h"
#include "ctcp_utils.h"
#include "ctcp_linked_list.h"

/* BBR has the following modes for deciding how fast to send: */
enum bbr_mode
{
    BBR_STARTUP,   /* ramp up sending rate rapidly to fill pipe */
    BBR_DRAIN,     /* drain any queue created during startup */
    BBR_PROBE_BW,  /* discover, share bw: pace around estimated bw */
    BBR_PROBE_RTT, /* cut cwnd to min to probe min_rtt */
};

/* static uint32_t bbr_min_rtt_win_sec = 10;     min RTT filter window (in sec) */
static uint32_t bbr_probe_rtt_mode_ms = 200; /* min ms at cwnd=4 in BBR_PROBE_RTT */

/* We use a high_gain value chosen to allow a smoothly increasing pacing rate
 * that will double each RTT and send the same number of packets per RTT that
 * an un-paced, slow-starting Reno or CUBIC flow would.
 */
static float bbr_high_gain = 2885 / 1000; /* 2/ln(2) */
static float bbr_drain_gain = 1000 / 2885;    /* 1/high_gain */
static float bbr_cwnd_gain = 2;               /* gain for steady-state cwnd */
/* The pacing_gain values for the PROBE_BW gain cycle: */
static float bbr_pacing_gain[] = {5 / 4, 3 / 4, 1, 1, 1, 1, 1, 1};
static uint32_t bbr_cycle_rand = 7; /* randomize gain cycling phase over N phases */

/* Try to keep at least this many packets in flight, if things go smoothly. For
 * smooth functioning, a sliding window protocol ACKing every other packet
 * needs at least 4 packets in flight.
 */
static uint32_t bbr_cwnd_min_target = 4;

/* To estimate if BBR_STARTUP mode (i.e. high_gain) has filled pipe. */
static float bbr_full_bw_thresh = 5 / 4; /* bw up 1.25x per round? */
static uint32_t bbr_full_bw_cnt = 3;                   /* N rounds w/o bw growth -> pipe full */

static uint32_t tcp_min_rtt = 40;

void bbr_init(bbr_state_t *bbr, uint32_t snd_cwnd)
{
    bbr->pacing_gain = bbr_high_gain;
    bbr->cwnd_gain = 0;
    bbr->mode = BBR_STARTUP;
    bbr->max_btlbw = snd_cwnd;
    bbr->max_btlbw_stamp = current_time();
    bbr->min_rtt_us = tcp_min_rtt;
    bbr->min_rtt_stamp = current_time();
    bbr_set_pacing_rate(bbr, bbr->max_btlbw, bbr_high_gain);
    bbr_set_cwnd(bbr, bbr->max_btlbw, bbr->min_rtt_us, bbr->cwnd_gain);
    int ind_bw;
    for(ind_bw = 0; ind_bw < BBR_BW_RTTS; ind_bw++)
    {
        bbr->BtlBwFilter[ind_bw] = 0;
    }
    int ind_rtt;
    for(ind_rtt = 0; ind_rtt < BBR_RTT_RTTS; ind_rtt++)
    {
        bbr->RTpropFilter[ind_rtt] = 0x7FFFFFFF;
    }
    bbr->cycle_idx = 0;
    bbr->full_bw = 0;
    bbr->full_bw_cnt = 0;
    bbr->inflight = 0;
    bbr->probe_rtt_done_stamp = 0;
    bbr->restore_cwnd = false;
    bbr->prior_cwnd = 0;
    bbr->delivered_time = current_time();
    bbr->delivered = 0;
    bbr->next_send_time = current_time();
    bbr_reset_startup_mode(bbr);
    fprintf(stderr, "Initial complete.\n");
}

void bbr_main(bbr_state_t *bbr, uint32_t bw_sample, uint32_t rtt_sample)
{
    uint32_t bw;
    uint32_t rtt;

    bbr_update_model(bbr, bw_sample, rtt_sample);

    bw = bbr_bw(bbr);
    rtt = bbr_rtt(bbr);
    bbr_set_pacing_rate(bbr, bw, bbr->pacing_gain);
    bbr_set_cwnd(bbr, bw, rtt, bbr->cwnd_gain);
}

/* Return the estimated bandwidth of the path, in pBytes/uS << BW_SCALE. */
uint32_t bbr_bw(bbr_state_t *bbr)
{
    return bbr->max_btlbw;
}

/* Return the estimated rtt of the path, in uS. */
uint32_t bbr_rtt(bbr_state_t *bbr)
{
    return bbr->min_rtt_us;
}

/* Pace using current bw estimate and a gain factor. */
void bbr_set_pacing_rate(bbr_state_t *bbr, uint32_t bw, float pacing_gain)
{
    uint32_t rate = bw;
    rate *= pacing_gain;
    if (bbr->mode != BBR_STARTUP || rate > bbr->pacing_rate)
		bbr->pacing_rate = rate;
        fprintf(stderr, "Pacing rate is %d\n", rate);
}

/* Find target cwnd. Right-size the cwnd based on min RTT and the
 * estimated bottleneck bandwidth:
 *
 * cwnd = bw * min_rtt * gain = BDP * gain
 *
 * The key factor, gain, controls the amount of queue. While a small gain
 * builds a smaller queue, it becomes more vulnerable to noise in RTT
 * measurements (e.g., delayed ACKs or other ACK compression effects). This
 * noise may cause BBR to under-estimate the rate.
 */
void bbr_set_cwnd(bbr_state_t *bbr, uint32_t bw, uint32_t rtt, float cwnd_gain)
{
    uint32_t w = bw * rtt;
    bbr->cwnd = w * cwnd_gain;
    bbr->cwnd = bbr->cwnd > bbr_cwnd_min_target ? bbr->cwnd : bbr_cwnd_min_target;
    fprintf(stderr, "cwnd is %d\n", bbr->cwnd);
}

void bbr_update_model(bbr_state_t *bbr, uint32_t bw_sample, uint32_t rtt_sample)
{
    bbr_update_bw(bbr, bw_sample);
    bbr_update_cycle_phase(bbr, bw_sample, rtt_sample);
    bbr_check_full_bw_reached(bbr, bw_sample, rtt_sample);
    bbr_check_drain(bbr, bw_sample, rtt_sample);
    bbr_update_min_rtt(bbr, rtt_sample);
}

void bbr_update_bw(bbr_state_t *bbr, uint32_t bw_sample)
{
    int ind_bw;
    bbr->max_btlbw = bw_sample;
    for (ind_bw = 0; ind_bw < BBR_BW_RTTS - 1; ind_bw++)
    {
        bbr->BtlBwFilter[ind_bw] = bbr->BtlBwFilter[ind_bw + 1];
        if (bbr->max_btlbw < bbr->BtlBwFilter[ind_bw])
            bbr->max_btlbw = bbr->BtlBwFilter[ind_bw];
    }
    bbr->BtlBwFilter[ind_bw] = bw_sample;
    bbr->max_btlbw_stamp = current_time();
}

/* Gain cycling: cycle pacing gain to converge to fair share of available bw. */
void bbr_update_cycle_phase(bbr_state_t *bbr, uint32_t bw_sample, uint32_t rtt_sample)
{
    if ((bbr->mode == BBR_PROBE_BW))
    {
        bbr_advance_cycle_phase(bbr);
    }
}

void bbr_advance_cycle_phase(bbr_state_t *bbr)
{
    bbr->cycle_idx = (bbr->cycle_idx + 1) % (CYCLE_LEN - 1);
    bbr->pacing_gain = bbr_pacing_gain[bbr->cycle_idx];
}

/* Do we estimate that STARTUP filled the pipe? */
bool bbr_full_bw_reached(bbr_state_t *bbr)
{
    return bbr->full_bw_cnt >= bbr_full_bw_cnt;
}

/* Estimate when the pipe is full, using the change in delivery rate: BBR
 * estimates that STARTUP filled the pipe if the estimated bw hasn't changed by
 * at least bbr_full_bw_thresh (25%) after bbr_full_bw_cnt (3) non-app-limited
 * rounds. Why 3 rounds: 1: rwin autotuning grows the rwin, 2: we fill the
 * higher rwin, 3: we get higher delivery rate samples. Or transient
 * cross-traffic or radio noise can go away. CUBIC Hystart shares a similar
 * design goal, but uses delay and inter-ACK spacing instead of bandwidth.
 */
void bbr_check_full_bw_reached(bbr_state_t *bbr, uint32_t bw_sample, uint32_t rtt_sample)
{
    uint32_t bw_thresh;

    if (bbr_full_bw_reached(bbr))
        return;

    bw_thresh = bbr->full_bw * bbr_full_bw_thresh;
    if (bbr_bw(bbr) >= bw_thresh)
    {
        bbr->full_bw = bbr_bw(bbr);
        bbr->full_bw_cnt = 0;
        return;
    }
    ++bbr->full_bw_cnt;
}

/* If pipe is probably full, drain the queue and then enter steady-state. */
void bbr_check_drain(bbr_state_t *bbr, uint32_t bw_sample, uint32_t rtt_sample)
{
    if (bbr->mode == BBR_STARTUP && bbr_full_bw_reached(bbr))
    {
        bbr->mode = BBR_DRAIN;             /* drain queue we created */
        bbr->pacing_gain = bbr_drain_gain; /* pace slow to drain */
        bbr->cwnd_gain = bbr_high_gain;    /* maintain cwnd */
    }                                      /* fall through to check if in-flight is already small: */
    if (bbr->mode == BBR_DRAIN && bbr->inflight <= bbr->cwnd)
    {
        bbr_reset_probe_bw_mode(bbr); /* we estimate queue is drained */
    }
}

/* The goal of PROBE_RTT mode is to have BBR flows cooperatively and
 * periodically drain the bottleneck queue, to converge to measure the true
 * min_rtt (unloaded propagation delay). This allows the flows to keep queues
 * small (reducing queuing delay and packet loss) and achieve fairness among
 * BBR flows.
 *
 * The min_rtt filter window is 10 seconds. When the min_rtt estimate expires,
 * we enter PROBE_RTT mode and cap the cwnd at bbr_cwnd_min_target=4 packets.
 * After at least bbr_probe_rtt_mode_ms=200ms and at least one packet-timed
 * round trip elapsed with that flight size <= 4, we leave PROBE_RTT mode and
 * re-enter the previous mode. BBR uses 200ms to approximately bound the
 * performance penalty of PROBE_RTT's cwnd capping to roughly 2% (200ms/10s).
 *
 * Note that flows need only pay 2% if they are busy sending over the last 10
 * seconds. Interactive applications (e.g., Web, RPCs, video chunks) often have
 * natural silences or low-rate periods within 10 seconds where the rate is low
 * enough for long enough to drain its queue in the bottleneck. We pick up
 * these min RTT measurements opportunistically with our min_rtt filter. :-)
 */
void bbr_update_min_rtt(bbr_state_t *bbr, uint32_t rtt_sample)
{
    uint32_t last_min_rtt_us = bbr->min_rtt_us;

    int i;
    bbr->min_rtt_us = rtt_sample;
    for (i = 0; i < BBR_BW_RTTS - 1; i++)
    {
        bbr->RTpropFilter[i] = bbr->RTpropFilter[i + 1];
        if (bbr->min_rtt_us > bbr->RTpropFilter[i])
            bbr->min_rtt_us = bbr->RTpropFilter[i];
    }
    bbr->RTpropFilter[i] = rtt_sample;
    bbr->min_rtt_stamp = current_time();

    
    
    bool filter_expired;
    filter_expired = bbr->min_rtt_us > last_min_rtt_us;

    if (bbr_probe_rtt_mode_ms > 0 && filter_expired && bbr->mode != BBR_PROBE_RTT)
    {
        bbr->mode = BBR_PROBE_RTT; /* dip, drain queue */
        bbr->pacing_gain = BBR_UNIT;
        bbr->cwnd_gain = BBR_UNIT;
        bbr_save_cwnd(bbr); /* note cwnd so we can restore it */
        bbr->probe_rtt_done_stamp = bbr_probe_rtt_mode_ms + current_time();
    }

    if (bbr->mode == BBR_PROBE_RTT && bbr->probe_rtt_done_stamp >= current_time())
    {
        bbr_reset_mode(bbr);
        if (bbr->restore_cwnd)
        {
            bbr->cwnd = bbr->cwnd > bbr->prior_cwnd ? bbr->cwnd : bbr->prior_cwnd;
            bbr->restore_cwnd = false;
        }
    }
}

void bbr_reset_startup_mode(bbr_state_t *bbr)
{
    bbr->mode = BBR_STARTUP;
    bbr->pacing_gain = bbr_high_gain;
    bbr->cwnd_gain = bbr_high_gain;
}

void bbr_reset_drain_mode(bbr_state_t *bbr)
{
    bbr->mode = BBR_STARTUP;
    bbr->pacing_gain = bbr_drain_gain;
    bbr->cwnd_gain = bbr_high_gain;
}

void bbr_reset_probe_bw_mode(bbr_state_t *bbr)
{
    bbr->mode = BBR_PROBE_BW;
    bbr->pacing_gain = BBR_UNIT;
    bbr->cwnd_gain = bbr_cwnd_gain;
    bbr->cycle_idx = CYCLE_LEN - 1 - rand() % bbr_cycle_rand;
}

void bbr_reset_mode(bbr_state_t *bbr)
{
    if (!bbr_full_bw_reached(bbr))
    {
        bbr_reset_startup_mode(bbr);
    }
    else
    {
        bbr_reset_probe_bw_mode(bbr);
    }
}

void bbr_save_cwnd(bbr_state_t *bbr)
{
    bbr->prior_cwnd = bbr->cwnd;
    bbr->restore_cwnd = true;
}
