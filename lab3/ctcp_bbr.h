#ifndef CTCP_BBR_H
#define CTCP_BBR_H

#include "ctcp_sys.h"
#include "ctcp_linked_list.h"

/* Scale factor for rate in pkt/uSec unit to avoid truncation in bandwidth
 * estimation. The rate unit ~= (1500 bytes / 1 usec / 2^24) ~= 715 bps.
 * This handles bandwidths from 0.06pps (715bps) to 256Mpps (3Tbps) in a u32.
 * Since the minimum window is >=4 packets, the lower bound isn't
 * an issue. The upper bound isn't an issue with existing technologies.
 */
#define BW_SCALE 24
#define BW_UNIT (1 << BW_SCALE) /* 1 << 24 = 16777216 */

#define BBR_SCALE 8	/* scaling factor for fractions in BBR (e.g. gains) */
#define BBR_UNIT (1 << BBR_SCALE) /* 1 << 8 = 256  */

#define CYCLE_LEN 8	/* number of phases in a pacing gain cycle */

#define BBR_BW_RTTS CYCLE_LEN + 2 /* win len of bw filter (in rounds) */
#define BBR_RTT_RTTS 10 /* win len of rtt filter (in rounds) */

/* BBR congestion control block */
struct bbr_state {
    int pacing_gain;   /* current gain for setting pacing rate */
    int cwnd_gain; /* current gain for setting cwnd */
    int mode;   /* current bbr_mode in state machine */
    uint32_t max_btlbw;   /* min RTT in min_rtt_win_sec window */
    long max_btlbw_stamp;
    uint32_t min_rtt_us;
    long min_rtt_stamp;
    uint32_t pacing_rate;
    uint32_t cwnd;
    uint32_t BtlBwFilter[BBR_BW_RTTS];
    uint32_t RTpropFilter[BBR_RTT_RTTS];
    int cycle_idx;
    uint32_t full_bw;
    int full_bw_cnt;
    uint32_t inflight;
    long probe_rtt_done_stamp;
    bool restore_cwnd;
    uint32_t prior_cwnd;
    long delivered_time;
    uint64_t delivered;
    long next_send_time;
};
typedef struct bbr_state bbr_state_t;

void bbr_init(bbr_state_t *bbr, uint32_t snd_cwnd);
void bbr_main(bbr_state_t *bbr, uint32_t bw_sample, uint32_t rtt_sample);

uint32_t bbr_bw(bbr_state_t *bbr);
uint32_t bbr_rtt(bbr_state_t *bbr);
void bbr_set_pacing_rate(bbr_state_t *bbr, uint32_t bw, float pacing_gain);
void bbr_set_cwnd(bbr_state_t *bbr, uint32_t bw, uint32_t rtt, float cwnd_gain);

void bbr_update_model(bbr_state_t *bbr, uint32_t bw_sample, uint32_t rtt_sample);
void bbr_update_bw(bbr_state_t *bbr, uint32_t bw_sample);
void bbr_update_cycle_phase(bbr_state_t *bbr, uint32_t bw_sample, uint32_t rtt_sample);
void bbr_check_full_bw_reached(bbr_state_t *bbr, uint32_t bw_sample, uint32_t rtt_sample);
void bbr_check_drain(bbr_state_t *bbr, uint32_t bw_sample, uint32_t rtt_sample);
void bbr_update_min_rtt(bbr_state_t *bbr, uint32_t rtt_sample);

void bbr_advance_cycle_phase(bbr_state_t *bbr);
bool bbr_full_bw_reached(bbr_state_t *bbr);

void bbr_reset_startup_mode(bbr_state_t *bbr);
void bbr_reset_drain_mode(bbr_state_t *bbr);
void bbr_reset_probe_bw_mode(bbr_state_t *bbr);
void bbr_reset_mode(bbr_state_t *bbr);
void bbr_save_cwnd(bbr_state_t *bbr);

#endif /* CTCP_H */