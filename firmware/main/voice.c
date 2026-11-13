/*
 * Local voice paths -- implementation.
 */
#include "driver/gpio.h"
#include "voice.h"

#include <math.h>
#include <string.h>

#include "esp_adc/adc_continuous.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_codec.h"
#include "audio_i2s.h"
#include "adc_audio_clock.h"
#include "bridge.h"
#include "rx_guard.h"
#include "eq6.h"
#include "rx_band.h"
#include "netcom.h"
#include "sa818.h"
#include "sa818_cfg.h"
#include "board_hw.h"
#include "nvs.h"
#include "ptt.h"
#include "sdr.h"
#include "voice_tail.h"
#include "speaker_diag.h"

static volatile bool s_preview_amp;   /* amplifier forced on during tail preview */
/* Set while a network talk also keys the radio: the talk task feeds the DAC and
 * the engine's playback loop must keep its hands off it (see voice.h). */
static volatile bool s_dac_talk;
/* This talk's local-RF decision, latched at start so a config change mid-talk
 * cannot move the DAC owner around. */
static bool s_rf_also;
static volatile bool s_bridge_inj_amp; /* amplifier forced on while bridge audio plays */

static const char *TAG = "voice";

#define BLOCK_FRAMES   (AUDIO_SAMPLE_RATE_HZ * 16 / 1000)   /* 16 ms of output (192 at 12 kHz) */
/*
 * The ADC runs at OVERSAMPLE times the audio rate and is then filtered and
 * decimated.
 *
 * This is not gold-plating.  Sampling the SA818's AF_OUT directly at 8 kHz
 * aliases everything above 4 kHz straight back into the voice band, and an
 * unsquelched FM discriminator is loud exactly there -- the result is a wall of
 * hiss that buries speech, and no amount of post-filtering can undo it because
 * the folding already happened.  Capturing at 32 kHz and low-passing before
 * decimation is what actually removes it.
 */
/*
 * How many ADC samples per output sample.  Not a free choice: the ESP32-S3's
 * continuous-ADC mode stops at SOC_ADC_SAMPLE_FREQ_THRES_HIGH = 83,333 Hz, and
 * asking for more makes adc_continuous_config() reject the ENTIRE configuration.
 * The monitor then never starts, voice_start() still returns ESP_OK, and the
 * panel simply shows "off" with no error anywhere -- which is exactly what a
 * 24 kHz stream did with a fixed 4x (96 kHz).  Take the largest ratio that fits,
 * because more oversampling is better anti-aliasing above the voice band.
 */
/*
 * The current stream is 16 kHz and the ADC clock is exactly 32 kHz (2x).
 * Both fixed receive filters run at that ADC rate before the 2:1 decimation.
 */
#define ADC_RATE_CEILING  33000
#define OVERSAMPLE_CHOOSE(r) ((r) * 4 <= ADC_RATE_CEILING ? 4 : \
                              (r) * 3 <= ADC_RATE_CEILING ? 3 : \
                              (r) * 2 <= ADC_RATE_CEILING ? 2 : 1)
#define OVERSAMPLE     OVERSAMPLE_CHOOSE(AUDIO_SAMPLE_RATE_HZ)
#define ADC_RATE_HZ    (AUDIO_SAMPLE_RATE_HZ * OVERSAMPLE)
_Static_assert(ADC_RATE_HZ == ADC_AUDIO_RATE_HZ,
               "Update the fixed ADC clock when changing the audio rate");
_Static_assert(AUDIO_SAMPLE_RATE_HZ * OVERSAMPLE <= 83333,
               "ADC rate above the S3 continuous-mode ceiling");
#define ADC_FRAME      512                              /* samples per frame */

/*
 * BYTES PER CONVERSION -- the bug that made this path unintelligible.
 *
 * On the ESP32-S3 each ADC DMA conversion is FOUR bytes, not two:
 *
 *     SOC_ADC_DIGI_RESULT_BYTES        = 4
 *     SOC_ADC_DIGI_DATA_BYTES_PER_CONV = 4
 *
 * because adc_digi_output_data_t is a union whose members are uint32_t
 * (data:12, reserved12:1, channel:4, unit:1).
 *
 * The code here was written for a 2-byte sample, so it consumed the low
 * half-word as a sample and then the CHANNEL/RESERVED half-word as the next
 * one.  Half of every block was therefore not audio at all.
 *
 * That is why the fault hid for so long: every test that sounded fine --
 * single tones, the 7-segment pattern, the pulsed self-test -- went through
 * direct I2S playback (ESP32 -> codec -> amplifier) and never touched the ADC.
 * Only the monitor path uses the ADC, and only the monitor path sounded wrong.
 */
#define ADC_ATOM_BYTES 4                                /* SOC_ADC_DIGI_RESULT_BYTES */
#define ADC_FRAME_BYTES (ADC_FRAME * ADC_ATOM_BYTES)    /* 2048 bytes = 16 ms */

/*
 * At the fixed 32 kHz clock a 512-conversion frame lasts 16 ms. The driver's
 * byte ring holds eight frames (16 KiB, 128 ms); one read is at most one frame.
 * adc_continuous_read() returns the available contiguous bytes without waiting
 * to fill the request. The monitor processes that block and writes it to I2S.
 *
 * flush_pool stays disabled. If a new DMA frame cannot enter the pool it is
 * dropped and on_pool_ovf records it. Pool capacity is unchanged; the exact
 * ADC/I2S clock ratio prevents the steady drift that used to fill it.
 */
#define ADC_POOL_FRAMES  8
#define ADC_POOL_BYTES   (ADC_FRAME_BYTES * ADC_POOL_FRAMES)   /* 16384 */
#define ADC_READ_FRAMES  1
#define ADC_READ_BYTES   (ADC_FRAME_BYTES * ADC_READ_FRAMES)   /* 2048 */
/*
 * +2 of slack: the decimation phase carries across calls, so a read can yield
 * one output sample more or less than the exact ratio.  Clipping at the exact
 * ratio would silently discard an output sample now and then, which is a rate
 * error the I2S side then fills with zeros.
 */
#define OUT_MAX          ((ADC_READ_BYTES / ADC_ATOM_BYTES / OVERSAMPLE) + 2)

/* ------------------------------------------------------------------ filters */

typedef struct {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
} biquad_t;

static inline float biquad_run(biquad_t *f, float x)
{
    const float y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2
                    - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1;  f->x1 = x;
    f->y2 = f->y1;  f->y1 = y;
    return y;
}


/*
 * Receive trial: 270 Hz low cut and 3.7 kHz high cut. The complete cascade runs
 * before the 32 kHz -> 16 kHz decimation, so it also rejects frequencies above
 * the output's 8 kHz Nyquist limit before they can fold into the voice band.
 * It cannot repair interference already aliased by the analogue ADC input.
 */
/* Operator-requested 270 Hz receive low cut replaces the 30 Hz DC blocker.
 * SA818 pin 3 -> ESP32-S3-WROOM pad 39 (GPIO1/ADC1_CH0). Apply only here,
 * at the raw 32 kHz rate, before decimation and the first gain/int16 boundary.
 * Microphone, browser TX, network injection and generated tails bypass it. */
_Static_assert(ADC_RATE_HZ == RX_LOWCUT_RATE_HZ, "Retune RX low cut for new ADC rate");
_Static_assert(ADC_RATE_HZ == RX_HIGHCUT_RATE_HZ, "Retune RX high cut for new ADC rate");
static rx_band_t s_rx_band;

/*
 * Both receive filters precede decimation and gain. Generated or injected
 * transmit audio joins later in monitor_task and bypasses both filters.
 */

/*
 * The software squelch was retired from the audio path: the SA818's OWN RF
 * squelch is the audio gate (multimeter-confirmed: with the module squelch
 * shut, AF_OUT sits at 0 V and nothing reaches the speaker), so re-gating
 * the amplifier in firmware only added delay after speech and tripped on
 * the module's post-TX settle transient.  What remains of it is the status
 * LED, which follows the input level so "receiving" is visible without a
 * console; the threshold below only decides when the LED lights.
 *
 * Measured levels after the band-pass (pre-gain, raw counts):
 *   - module squelch closed, no signal: 0-4 counts
 *   - a real carrier:                   30-300+ counts
 *
 * The threshold stays adjustable at runtime (`sqgate`) for weak or strong
 * signals.  If an empty channel makes the LED flicker, raise it.
 */
#define SQ_OPEN_COUNTS_DEF   10
#define SQ_CLOSE_COUNTS      10
/*
 * How long the LED stays lit after the last block above the close threshold:
 * anti-flicker for weak signals.  31 blocks of 16 ms = 0.5 s.
 */
#define LED_HOLD_BLOCKS      31

/*
 * Freeze on the input-level LED right after a TALK -> MONITOR handoff.
 *
 * When the SA818 is released from TX, its AF_OUT does not go straight back
 * to the idle noise floor: the module's TX -> RX bias step and its AGC
 * re-settle push a transient onto the input for several hundred ms.  Those
 * peaks sit above the LED threshold, so without this guard every
 * transmission would end with the LED lit for the hold time.  800 ms
 * covers the measured settle (transients die ~500 ms after the monitor
 * task starts; the module's own RSSI readout is back after ~1 s) with
 * margin.  The AUDIO side of the same transient is covered by the RX fade
 * below.
 */
#define GATE_GUARD_MS        800

/*
 * The amplifier is on for the whole monitoring session, so the post-TX
 * settle transient on AF_OUT would otherwise play through the speaker as a
 * half-second noise burst after every transmission.  Fade the monitor
 * output in over 500 ms after every monitor start instead: the burst dies
 * while the audio is still ramping, and a real carrier arriving early is
 * only attenuated, never hard-muted.
 */
#define RX_FADE_BLOCKS       3             /* 3 x 16 ms = 48 ms */
static int s_rx_fade_left;

/*
 * Set by voice_monitor_instant() before voice_start(): skip the post-TX
 * settle fade so a tail preview tone is not swallowed by the ramp.
 */
static bool s_instant_start;

static int s_sq_open_counts = SQ_OPEN_COUNTS_DEF;
/*
 * LED frozen off until this time (esp_timer ms).  Written by voice_start()
 * and read by the monitor task: a volatile uint32_t, so the store is a
 * single atomic word and the unsigned comparison below is wrap-safe.  Zero
 * means no guard.
 */
static volatile uint32_t s_gate_guard_ms;

/*
 * The amplifier follows the SA818's OWN RF squelch state, observed at its
 * source: the DC bias on AF_OUT.  With the module squelch shut, AF_OUT sits
 * at 0 V (raw mean ~0, occasional ADC glitches to ~40); when it opens for
 * a carrier, the output stage's bias appears on the line.
 *
 * MEASURED on the v0.3 board with a strong 12.5 kHz voice carrier (the
 * "0.3-0.8 V" figure in the bias note below proved to be several times too
 * high): raw per-chunk mean ~180-250 counts, max ~290-300, min 10-140
 * (the demodulated audio rides across the bias, so the MAX is not a stable
 * discriminator -- the MEAN is, because the bias is DC).
 *
 * Consequences: on an empty channel the amplifier is OFF (no output-stage
 * hiss at the speaker), and a carrier reaches the speaker only after the
 * module's own squelch release time -- the RF squelch owns the speaker, end
 * to end.  The thresholds sit wide inside the measured ranges: idle mean
 * ~0-5, biased mean ~180+, so 60/25 give a 3x+ margin on both sides.
 */
/*
 * The module's squelch is inferred from the AF_OUT DC bias (the board has no
 * DC-blocking capacitor, so the bias sits on the ADC and steps up when the
 * module's own squelch -- CTCSS included -- opens).
 *
 * Absolute thresholds do not survive reality: the bias drifts with temperature,
 * differs between boards, and changed again when board A was re-soldered.  With
 * a fixed "open above 60 counts" the operator saw the squelch open with no
 * signal at all, at maximum squelch level, with CTCSS enabled.  The threshold is
 * therefore relative to a slow baseline learned while closed.
 */
#define MOD_SQ_MARGIN       70.0f   /* counts above the recent noise PEAK   */
#define MOD_SQ_PEAK_DECAY   0.0015f /* peak-hold decay, ~10 s at 16 ms      */

/*
 * What this detector may and may not use.
 *
 * RSSI CANNOT be part of the decision.  The module reports a carrier for any
 * signal near the tuned frequency, regardless of CTCSS, and a handheld a few
 * metres away on another channel raises it just as well -- which made the
 * squelch open with no tone, and even with the handheld on a different
 * frequency.  That is exactly the bug the operator reported.
 *
 * The only signal that already encodes "carrier AND matching CTCSS" is the
 * AUDIO: the module gates AF_OUT through its own squelch and tone decoder, so
 * when it is muted the samples sit at 0-2 counts, and when it is open you get
 * the carrier's hiss (hundreds to thousands of counts) whether or not anyone is
 * talking.  Speaker hiss therefore keeps the squelch open while a carrier is
 * present, which is what a real radio does.
 *
 * The AF_OUT bias transient is kept as a second, faster trigger: it fires within
 * one block when the module opens, which catches the first quiet syllable.
 */
/*
 * Thresholds are in the same counts as blk_peak, the pre-gain DC-blocked block
 * peak.  That level is NOT comparable between boards: measured at idle,
 * 192.0.2.13 sits at 0-2 while 192.0.2.178 reaches 82 (its module is not fully
 * muted at squelch level 6).  A constant therefore either never opens or opens
 * on noise, which is exactly what two rounds of field testing showed.
 *
 * So the threshold follows the panel's own squelch control (s_sq_open_counts,
 * the "静噪阈值" that has been tuned in the field and drives the LED):
 * open at three times it, close at it.  That keeps one knob for the operator and
 * makes this detector behave like the LED, which does work.
 */
#define SQ_AUDIO_OPEN    ((float)s_sq_open_counts * 3.0f)
#define SQ_AUDIO_CLOSE   ((float)s_sq_open_counts)
#define SQ_HOLD_MS       2500     /* bridges sentence gaps, not just word gaps */
/* AF_OUT audio/bias or hardware COS alone determines module squelch. */
#define SQ_BLOCK_MS      ((int)(BLOCK_FRAMES / (AUDIO_SAMPLE_RATE_HZ / 1000)))
#define MOD_SQ_ON_BLOCKS    4     /* 64 ms of bias before amp on        */
#define MOD_SQ_OFF_BLOCKS   8     /* 128 ms of ~0V before amp off       */

static bool s_mod_sq_open;        /* module squelch: audio + bias, see below */
static int16_t s_rf_prefix[OUT_MAX];
static size_t s_rf_prefix_n;
static uint8_t s_rf_prefix_blocked;
static uint8_t s_rf_prefix_mode;
static int64_t s_rf_prefix_us;

/* RF -> network level (1/16x).  The stream used to be tapped before the speaker
 * gain, so it inherited whatever the ADC produced and had no control of its own. */
static int s_net_rx_gain = 16;   /* 0 dB (RF -> network default; was +6 dB/32) */
static int gain_mul(int16_t *buf, size_t n, int gain);
static int  s_sq_quiet_ms;        /* ms of "no audio" while open */
/*
 * Hardware COS.  SA818/S A818S pin 1 is "Audio ON": the module pulls it LOW when
 * its own squelch opens -- carrier on frequency AND, when a CTCSS tone is
 * programmed, the matching tone.  That is the signal every AllStar hotspot uses
 * as COS, and it is the only one that is exactly right by construction.
 *
 * As shipped pin 1 is tied to +3V3 (docs/PINMAP.md, U5 pin 1).  The field
 * boards have that link cut and pin 1 run through 1 kΩ to IO40
 * (docs/HANDOVER-SUMMARY.md), so IO40 is the compiled-in default: a reflash or
 * an NVS wipe must not silently fall back to inferring squelch from audio.
 * /api/set?k=sqgpio&v=<gpio> still overrides it and persists in NVS; 0 disables
 * it again.  On a board without the mod the pulled-up pin reads high and the
 * squelch never opens -- set 0 there.
 */
#define SQ_GPIO_DEFAULT  40
static int  s_sq_gpio = SQ_GPIO_DEFAULT;
static int  s_sq_gpio_run;        /* +N = high confidence open, -N = closed */
static volatile int s_bpk_hold;   /* peak-hold of blk_peak, for diagnosis */
static int64_t s_last_stop_us;     /* when the last session released the path */
static float s_bias_base;         /* learned closed-squelch bias          */
static bool  s_bias_have;
static volatile int s_bias_raw;   /* last block mean, for diagnostics     */
static int  s_mod_sq_on_run;
static int  s_mod_sq_off_run;
/*
 * True when the block currently being built was replaced by audio coming FROM
 * the room (bridge / browser / SDR transmit).  The monitor task sets it in the
 * injection branch and consumes it at the EQ step, which is what decides whether
 * a block gets the SPK bank (audio being received) or the MIC bank (audio being
 * put on the air).  A file-scope flag because the injection branch and the EQ
 * call sit in different scopes of a deeply nested block.
 */
static volatile bool s_blk_injected;
/* True when the current injected block has already gone through the common
 * microphone TX EQ and fixed TX gain (the browser SDR path).  Network bridge
 * audio remains raw here and is processed by the monitor's MIC bank below. */
static volatile bool s_blk_tx_processed;
static volatile uint32_t s_bridge_starve;
static volatile uint32_t s_bridge_blocks;  /* monitor blocks played */
static bool s_inj_primed;   /* injection ring holds a cushion */
static volatile uint32_t s_bridge_trim;    /* rate trim actions       */  /* keyed blocks with no injection audio */

/* Silent injection (SDR): keep the LM4871 shut while browser microphone audio
 * is being injected towards the transmitter.  Defined with the injection ring
 * further down; declared here because the monitor task reads it. */
static volatile bool s_inject_silent;
/* The injection ring carries either network PCM (which still needs the MIC
 * EQ in the monitor) or browser PCM that was processed before fan-out. */
static volatile bool s_inject_tx_processed;
static uint32_t s_inject_token;
static ptt_owner_t s_inject_owner;
static bool s_net_feeding, s_net_rf_required, s_net_rf_keyed, s_net_write_pending;
static int64_t s_net_ready_us, s_net_close_us;
#define NET_TX_LEAD_US 300000LL
#define NET_DRAIN_LIMIT_US 2000000LL
/* Set by voice_bridge_play() for the block it just returned. */
static volatile bool s_last_bridge_tx_processed;
/* SDR release tail: the monitor consumes the selected tone on its sample
 * clock after any queued browser PCM, then reports the final written block to
 * sdr.c before the owner is released. */
static bool s_sdr_tail_active;
static bool s_sdr_tail_block_pending;
static int s_sdr_tail_id;
static size_t s_sdr_tail_pos;
static uint32_t s_sdr_tail_token;
static uint32_t s_sdr_tail_done_token;
static int s_sdr_tail_config;

/* Courtesy tone for a network over that keys the radio (bridge / P2P / relay). */
static bool s_net_tail_active;
static int s_net_tail_id;
static size_t s_net_tail_pos;
/* Latched when the browser transmission starts.  The tail is fetched by the
 * monitor task, so this flag tells that task whether its samples also belong
 * to the network TX stream. */
static bool s_sdr_net_tx;

/* 6-band EQ chains, used by the audio tasks; defined with the NVS layer at
 * the bottom of this file. */
static eq6_t s_eq_spk;
static eq6_t s_eq_mic;

/* --------------------------------------------------- input-level indicator */

/*
 * History: the software squelch used to shut the amplifier down with no
 * signal, killing the output stage's own hiss (the ES8311 DAC, the LM4871
 * and the USB supply amplifying their own noise).  The operator found the
 * resulting close-delay and the post-TX transient behaviour more annoying
 * than the hiss, and the SA818's RF squelch already gates the radio's own
 * noise at the source (AF_OUT = 0 V when it is shut).  The amplifier now
 * stays on for the whole monitoring session; what remains here is the LED
 * hold counter.  The cost is the output stage's own quiet hiss on an empty
 * channel, which the gain slider trims.
 */
static int  s_sq_hold;                 /* LED: blocks left before turning off */

/* Recent-audio ring: 3 s of filtered, pre-gain samples.  Lives in PSRAM:
 * 48 KB of internal .bss that is only touched while the capture feature is on
 * left the board with ~13 KB of internal RAM, which is what makes lwIP drop
 * packets and task creation fail. */
#define CAP_SAMPLES  (AUDIO_SAMPLE_RATE_HZ * 3)
static int16_t *s_cap_ring;

static bool cap_ensure(void)
{
    if (s_cap_ring) {
        return true;
    }
    s_cap_ring = heap_caps_malloc(CAP_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_cap_ring) {
        ESP_LOGE("voice", "capture ring: no PSRAM for %u bytes",
                 (unsigned)(CAP_SAMPLES * sizeof(int16_t)));
        return false;
    }
    return true;
}
static volatile size_t s_cap_w;
static volatile bool   s_cap_on;
static volatile bool s_quiet;

/*
 * Level logging is handed to another task instead of being printed here.
 *
 * ESP_LOGI writes to the console UART, and that write BLOCKS when nobody is
 * draining the port (the line is ~150 chars, 13 ms at 115200).  This task is the
 * one that empties the ADC pool, so a blocking print here is a stall of exactly
 * the kind that makes the pool overflow -- measured at ~0.13 overflows/s with
 * 20 ms frames, one 10.7 ms hole every eight seconds, which is audible as an
 * occasional tick.  The audio task now only formats the line; ptt_task prints it.
 */
static char          s_level_line[192];
static volatile bool s_level_pending;
static volatile bool s_amp_noise_test;
static volatile bool s_bypass_squelch;
/*
 * True while an audio task exists.  The task itself clears this immediately
 * before vTaskDelete(NULL), which is the only reliable way for voice_stop() to
 * know it is safe to touch the ADC and I2S drivers again.
 */
static volatile bool s_task_running;
static volatile bool s_adc_error;

static inline void cap_push(int16_t v)
{
    if (!s_cap_on || !s_cap_ring) {
        return;
    }
    s_cap_ring[s_cap_w] = v;
    s_cap_w = (s_cap_w + 1) % CAP_SAMPLES;
}

static voice_mode_t s_mode = VOICE_OFF;
static TaskHandle_t s_task;

/*
 * TALK stack lives in .bss instead of the heap.
 *
 * With the panel served over HTTPS the browser keeps 3-5 TLS connections
 * open, which leaves the internal heap with an 8 KiB largest block -- exactly
 * the size xTaskCreate() asked for the talk task, so PTT failed with
 * ESP_ERR_NO_MEM whenever the page was open.  6 KiB is enough for the talk
 * loop and its completion path (the same size the monitor task uses) and is
 * reserved once at link time, so fragmentation can never refuse a press.
 * The high-water mark is logged on every release; if it drops below ~1 KiB
 * of margin the size must go back up. */
#define TALK_STACK_BYTES 6144
static StackType_t s_talk_stack[TALK_STACK_BYTES / sizeof(StackType_t)];
static StaticTask_t s_talk_tcb;
static volatile bool s_stop;
/* Serialize public lifecycle calls.  Audio tasks never take this lock before
 * acknowledging exit, so an external stop can safely wait for them. */
static StaticSemaphore_t s_control_storage;
static SemaphoreHandle_t s_control;
static portMUX_TYPE s_control_init = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_control_generation;
static void control_lock(void)
{
    taskENTER_CRITICAL(&s_control_init);
    if (!s_control) { s_control = xSemaphoreCreateRecursiveMutexStatic(&s_control_storage); }
    taskEXIT_CRITICAL(&s_control_init);
    xSemaphoreTakeRecursive(s_control, portMAX_DELAY);
}
static void control_unlock(void) { xSemaphoreGiveRecursive(s_control); }
static esp_err_t voice_start_locked(voice_mode_t mode);
static esp_err_t voice_stop_locked(void);
/*
 * Talk release requested by the caller (panel PTT release, web talk timer)
 * without immediately releasing the PTT.
 *
 * The old flow called ptt_request_rx() at once, which unkeyed the radio 50 ms
 * later -- BEFORE the end-of-transmission tail tone played.  The tail then
 * went out the DAC with the transmitter already in receive: audible path and
 * ptt_extend() both silently did nothing, and the tone never reached the air.
 * Now the caller sets this flag; the talk task breaks its main loop on it
 * while the PTT is still keyed, plays the tail under a live ptt_extend(), and
 * releases the PTT itself once the tone is done.
 */
static volatile bool s_release_requested;
static int s_ab_gate_ms;      /* audible on/off self-test gate, 0 = off */
/*
 * Default make-up gain for MONITOR.
 *
 * Sized from measurement.  Two input levels matter:
 *   - squelch closed: ~150 raw counts residual
 *   - squelch OPEN (which is what happens the moment a carrier arrives):
 *     ~1055 counts of noise floor, and a voice on top of that
 * So the gain has to keep 1055 * gain under the int16 rail, otherwise every
 * key-up turns into a wall of clipped noise that buries the speech inside it.
 * 24 keeps the worst case at ~25000, and the soft limiter below covers the rest.
 */
/*
 * Default make-up gain for MONITOR.
 *
 * Sized from the band-passed signal, which is what the DAC actually receives.
 * On-device analysis of real speech (console `V`) measured:
 *
 *     per-16 ms-block RMS:  mean 5-14,  min 0-2,  max 43-45 counts
 *     crest 3.1-5.0,  dip 0.07-0.18
 *
 * Two things follow.  First, the gaps between syllables fall to ~1 count while
 * speech peaks near 45, so the module's demodulated audio is clean -- better
 * than 30 dB of in-envelope SNR, with no noise filling the gaps.
 *
 * Second, the signal is TINY in absolute terms: 45 counts out of 2048 full
 * scale.  An earlier gain of 64 was scaled for the UNFILTERED signal
 * (300-880 counts), most of which turned out to be out-of-band noise that the
 * anti-alias and band-pass filters correctly removed.  After fixing the
 * filtering the gain had to be re-derived, and 64 left speech at about
 * -21 dBFS -- quiet enough that the DAC's and amplifier's own noise sat at the
 * same level and the operator heard hiss rather than speech.
 *
 * 160 puts a 45-count speech RMS at about 7200 (-13 dBFS), leaving the residual
 * ~1 count RMS far below audibility.
 *
 * LOWERED TO 96 after the amplifier was repaired and speech became audible:
 * a 45-count block RMS implies instantaneous peaks of roughly 4x that, i.e.
 * ~180 counts, and 180 x 160 = 28800 crosses the soft-limiter knee at 24000 on
 * every loud syllable.  That is audible as grit, and it was self-inflicted by
 * sizing the gain from RMS alone.  96 keeps typical peaks near 17000, clear of
 * the knee, and can be raised live with the console `gain` command.
 */
/*
 * Fixed gain, and the measurements behind it.
 *
 * The `G` sweep stepped the gain and counted limiter hits against the measured
 * input peak, with speech held constant throughout (in-peak stayed 208-237
 * counts, so no step was corrupted by silence):
 *
 *     gain 120 ->   0 hits      gain 200 ->  19 hits
 *     gain 160 ->   5 hits      gain 240 ->  52 hits
 *                               gain 255 ->  67 hits
 *
 * Those are counts out of 40000 samples per step, so even at the maximum the
 * limiter touches 0.17% of samples -- only the very tips of the waveform.  The
 * digital chain therefore has plenty of headroom, and loudness cannot come from
 * pushing it further: it has to come from the analogue side, which is the R18
 * change from 20k to 47k (LM4871 gain 4 -> 9.4, +7.4 dB, the highest the
 * datasheet allows without adding a compensation capacitor).
 *
 * 240 is chosen as the default: it uses the digital range fully while leaving a
 * little margin below the limiter's onset for a louder talker.
 *
 * RE-SCALED to 80 after the analogue gain went up.
 *
 * R18 was changed from 20k to 100k (with the 30 pF compensation capacitor the
 * LM4871 datasheet requires above 10x), so the analogue gain rose from 4x to
 * 20x -- exactly 5x, or +14 dB.  Loudness is the product of digital gain, codec
 * volume and analogue gain, so the digital gain has to come down by the same
 * factor: 240 / 5 = 48 keeps the previous loudness, and 80 sits a little above
 * that because more output was wanted.
 *
 * There is a hard limit on the analogue side now: the LM4871 runs from 5 V in
 * BTL, so it can swing about +/-4.7 V, and at 20x that means its input must stay
 * under roughly 235 mV peak.  Pushing the digital gain up past that clips the
 * amplifier, which is audible as distortion and is NOT visible in the `lim`
 * counter -- that counter only watches the digital limiter.
 */
static int s_gain = 96;    /* +15.6 dB: receive -> speaker (default, 2026-11-05) */

/*
 * No soft limiter.
 *
 * There used to be a 4:1 knee at 29000 here, as a "peak safety net".  It is
 * dynamic processing on the voice path -- the same class as the noise gate that
 * was removed for good -- and it squashed transients that the operator can hear.
 * Gain is set low enough now (bgtx 2.2x instead of 3x) that peaks land around
 * 40% of full scale, so what is left is a plain saturation: it stops an
 * int16 wrap-around, which would be a roar, and does nothing else.
 */
static inline int clamp16(int v)
{
    if (v > 32767)  { return 32767; }
    if (v < -32768) { return -32768; }
    return v;
}

/*
 * The SA818's AF_OUT lands on GPIO1 with NO coupling capacitor (the netlist has
 * exactly two pins on SA818_AUDIO_IN: U5.3 and U2.39), so the module's DC bias
 * arrives at the ADC along with the audio.  Measured on the v0.3 board with a
 * strong carrier it sits at roughly 180-250 raw counts (~0.06 V) -- the
 * earlier "0.3-0.8 V" estimate was several times too high.  The bias is the
 * module-squelch state signal: 0 V when the squelch is shut (see MOD_SQ_*).
 *
 * The low cut's first section removes that bias, then the 3.7 kHz high cut and
 * remaining 270 Hz low-cut sections limit bandwidth, all before decimation.
 */
static adc_continuous_handle_t s_adc_dma;
static voice_stats_t s_stats;

/* ------------------------------------------------------------------- ADC DMA */

/*
 * Dropped-sample detection.
 *
 * DMA-completed samples include frames that could not enter the driver pool;
 * successful reads alone cannot reveal a fast producer. Keep both rates and
 * preserve the overflow total across monitor sessions. The fixed hardware
 * clock gives the 2:1 decimator 32 kHz for the 16 kHz I2S sink.
 */
static portMUX_TYPE s_adc_diag_mux = portMUX_INITIALIZER_UNLOCKED;
static voice_adc_diag_t s_adc_diag;
static int64_t s_start_us;
static int64_t s_stopped_us;

static bool IRAM_ATTR adc_conv_done_cb(adc_continuous_handle_t h,
                                      const adc_continuous_evt_data_t *edata,
                                      void *user)
{
    (void)h;
    (void)user;
    portENTER_CRITICAL_ISR(&s_adc_diag_mux);
    s_adc_diag.produced_samples += edata->size / ADC_ATOM_BYTES;
    portEXIT_CRITICAL_ISR(&s_adc_diag_mux);
    return false;
}

static bool IRAM_ATTR adc_pool_ovf_cb(adc_continuous_handle_t h,
                                      const adc_continuous_evt_data_t *edata,
                                      void *user)
{
    (void)h;
    (void)edata;
    (void)user;
    portENTER_CRITICAL_ISR(&s_adc_diag_mux);
    ++s_adc_diag.ovf_total;
    ++s_adc_diag.ovf_session;
    portEXIT_CRITICAL_ISR(&s_adc_diag_mux);
    return false;
}

static void adc_diag_stopped(void)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_adc_diag_mux);
    s_stopped_us = now;
    s_adc_diag.running = false;
    portEXIT_CRITICAL(&s_adc_diag_mux);
}

void voice_adc_diag_get(voice_adc_diag_t *out)
{
    int64_t start, stopped;
    portENTER_CRITICAL(&s_adc_diag_mux);
    *out = s_adc_diag;
    start = s_start_us;
    stopped = s_stopped_us;
    portEXIT_CRITICAL(&s_adc_diag_mux);
    const int64_t elapsed = (out->running ? esp_timer_get_time() : stopped) - start;
    if (elapsed > 0) {
        out->elapsed_ms = (uint32_t)(elapsed / 1000);
        out->produced_hz = (uint32_t)(out->produced_samples * 1000000ULL / elapsed);
        out->read_hz = (uint32_t)(out->read_samples * 1000000ULL / elapsed);
    }
}

/*
 * The handle (driver ring buffer, DMA descriptors, GDMA channel) is created
 * exactly once and kept alive for the whole boot; only the conversion starts
 * and stops per monitor session.
 *
 * Why: every session used to new_handle/deinit the driver, and the driver's
 * ring-buffer storage is an 8 KB INTERNAL-DRAM allocation.  After a talk cycle
 * had chipped that region into smaller blocks, the next monitor start failed
 * with adc_continuous_new_handle: ESP_ERR_NO_MEM (gdma channel-struct allocs
 * included) and the board could not re-enter the monitor at all until a
 * reboot.  A persistent handle allocates once, while internal DRAM is still
 * clean at first use, and the talk path never touches it.
 *
 * The newlib-lock rule still holds: the driver's start takes and the stop
 * releases an owner-sensitive lock, so both ends run in the same monitor task
 * (see below).  Creating the handle itself takes no such lock.
 */
static esp_err_t adc_dma_start(void)
{
    if (!s_adc_dma) {
        /* Take ADC1 from the one-shot driver for the rest of the boot. */
        board_adc_dma_begin();

        adc_continuous_handle_cfg_t hdl = {
            .max_store_buf_size = ADC_POOL_BYTES,
            .conv_frame_size    = ADC_FRAME_BYTES,
        };
        esp_err_t err = adc_continuous_new_handle(&hdl, &s_adc_dma);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "adc_continuous_new_handle: %s", esp_err_to_name(err));
            return err;
        }

        adc_digi_pattern_config_t pattern = {
            .atten     = ADC_ATTEN_DB_12,
            .channel   = BOARD_ADC_CH_RADIO_AF,
            .unit      = ADC_UNIT_1,
            /*
             * Must be a concrete width.  ADC_BITWIDTH_DEFAULT is 0, and the
             * continuous driver validates bit_width >= SOC_ADC_DIGI_MIN_BITWIDTH,
             * so the "default" value is rejected with ESP_ERR_INVALID_ARG.
             */
            .bit_width = ADC_BITWIDTH_12,
        };
        adc_continuous_config_t cfg = {
            .pattern_num    = 1,
            .adc_pattern    = &pattern,
            .sample_freq_hz = ADC_RATE_HZ,
            .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
            .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
        };
        err = adc_continuous_config(s_adc_dma, &cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "adc_continuous_config: %s", esp_err_to_name(err));
            adc_continuous_deinit(s_adc_dma);
            s_adc_dma = NULL;
            return err;
        }
        {
            adc_continuous_evt_cbs_t cbs = {
                .on_conv_done = adc_conv_done_cb,
                .on_pool_ovf = adc_pool_ovf_cb,
            };
            err = adc_continuous_register_event_callbacks(s_adc_dma, &cbs, NULL);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "ADC callbacks not registered: %s",
                         esp_err_to_name(err));
                (void)adc_continuous_deinit(s_adc_dma);
                s_adc_dma = NULL;
                return err;
            }
        }
    }

    /* The persistent handle retains old pool data after stop. Only discard it
     * between sessions, while the driver is stopped and no reader exists. */
    esp_err_t err = adc_continuous_flush_pool(s_adc_dma);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC stopped-pool clear: %s", esp_err_to_name(err));
        return err;
    }
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_adc_diag_mux);
    ++s_adc_diag.pool_clears;
    ++s_adc_diag.session;
    s_adc_diag.ovf_session = 0;
    s_adc_diag.produced_samples = 0;
    s_adc_diag.read_samples = 0;
    s_adc_diag.i2s_write_max_us = 0;
    s_adc_diag.clock_exact = false;
    s_adc_diag.requested_hz = ADC_RATE_HZ;
    s_adc_diag.running = true;
    s_start_us = now;
    s_stopped_us = 0;
    portEXIT_CRITICAL(&s_adc_diag_mux);
    err = adc_continuous_start(s_adc_dma);
    if (err != ESP_OK) {
        adc_diag_stopped();
        ESP_LOGE(TAG, "adc_continuous_start: %s", esp_err_to_name(err));
        /*
         * A failed start may leave the driver half-configured; the persistent
         * handle is not reusable, so drop it and let the next session create a
         * fresh one.  stop first so the deinit sees the init state.
         */
        (void)adc_continuous_stop(s_adc_dma);
        (void)adc_continuous_deinit(s_adc_dma);
        s_adc_dma = NULL;
        return err;
    }
    adc_audio_clock_t clock;
    adc_audio_clock_read(&clock);
    portENTER_CRITICAL(&s_adc_diag_mux);
    s_adc_diag.clock_exact = clock.exact;
    s_adc_diag.configured_hz = clock.configured_hz;
    s_adc_diag.source_hz = clock.source_hz;
    s_adc_diag.div_num = clock.div_num;
    s_adc_diag.div_a = clock.div_a;
    s_adc_diag.div_b = clock.div_b;
    s_adc_diag.interval = clock.interval;
    portEXIT_CRITICAL(&s_adc_diag_mux);
    if (!clock.exact) {
        (void)adc_continuous_stop(s_adc_dma);
        adc_diag_stopped();
        ESP_LOGE(TAG, "ADC exact clock readback failed; capture stopped");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "ADC1 DMA GPIO%d: %u Hz, divider=%u interval=%u -> %d Hz",
             PIN_SA818_AUDIO_IN, (unsigned)clock.configured_hz,
             (unsigned)(clock.div_num + 1U), (unsigned)clock.interval,
             AUDIO_SAMPLE_RATE_HZ);
    return ESP_OK;
}

static void adc_dma_stop(void)
{
    if (s_adc_dma) {
        /*
         * Stop the conversion only.  The handle persists across sessions
         * (see adc_dma_start); deiniting it here is what leaked the re-allocation
         * of 8 KB of internal DRAM back into the fragmented pool.
         */
        (void)adc_continuous_stop(s_adc_dma);
        adc_diag_stopped();
    }
}

/* --------------------------------------------------------------------- task */

/*
 * MONITOR: ADC at 32 kHz -> fixed 270 Hz / 3.7 kHz RX band -> decimate 2:1 ->
 *          fixed gain / EQ -> codec DAC at 16 kHz.
 *
 * The ADC continuous driver is started AND stopped inside this task, never from
 * the caller (the persistent handle itself is created on first use, also from
 * here -- see adc_dma_start).
 *
 * That is not tidiness.  The driver takes a newlib recursive lock on start and
 * releases it on stop, and newlib locks are owner-sensitive: releasing one from
 * a task that does not own it trips
 * "assert failed: xTaskPriorityDisinherit" in FreeRTOS and reboots the board.
 * Monitor mode is auto-started from app_main, so with start in app_main and
 * stop in the console task the two never matched and pressing the amp-alone key
 * crashed the board every time.  Keeping both ends in one task makes ownership
 * consistent by construction.
 *
 * S3 TYPE2 uses four bytes per conversion; the low 12 bits hold the value.
 */
/*
 * The working buffers are static, not stack locals.
 *
 * ADC_READ_BYTES + OUT_MAX + OUT_MAX is about 6 KB, and the audio task's stack
 * is 4 KB: keeping them on the stack overflowed it and put the board into a
 * reset loop (it dropped off USB entirely).  Only one audio task ever runs at a
 * time -- voice_stop() waits for it to exit before another is created -- so
 * static storage is safe here and removes the pressure completely.
 */
/*
 * ...and they live in PSRAM.  Even with the capture ring moved out, these three
 * (about 16 KB of internal .bss) left the WiFi driver reporting "mem fail" and
 * lwIP dropping packets under load.  The audio path touches ~8 KB/s, which is
 * nothing for PSRAM.
 */
static uint8_t *s_raw;
static int16_t *s_out;
static int16_t *s_mid;

static bool voice_bufs_alloc(void)
{
    if (!s_raw) {
        s_raw = heap_caps_malloc(ADC_READ_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (!s_out) {
        s_out = heap_caps_malloc(sizeof(int16_t) * OUT_MAX, MALLOC_CAP_SPIRAM);
    }
    if (!s_mid) {
        s_mid = heap_caps_malloc(sizeof(int16_t) * OUT_MAX, MALLOC_CAP_SPIRAM);
    }
    if (!s_raw || !s_out || !s_mid) {
        ESP_LOGE(TAG, "audio buffers: no PSRAM (%u/%u/%u bytes)",
                 (unsigned)ADC_READ_BYTES, (unsigned)(sizeof(int16_t) * OUT_MAX),
                 (unsigned)(sizeof(int16_t) * OUT_MAX));
        return false;
    }
    return true;
}

/*
 * Raw ADC statistics, taken before any filtering.
 *
 * ESP32's SAR ADC input is not high impedance: every conversion charges an
 * internal sample-and-hold capacitor from the source.  ESP-IDF's ADC guidance
 * therefore expects a low source impedance and a bypass capacitor at the pin --
 * and this board has neither, because SA818_AUDIO_IN is a direct two-pin
 * connection between the module's AF_OUT and GPIO1.
 *
 * Incomplete settling shows up as gain error and amplitude-dependent
 * distortion, so the raw numbers are the place to look: if the signal is
 * running into 0 or 4095 it is clipping, and if the DC operating point moves
 * with the signal the sampling network is being dragged around by it.
 */
static uint16_t s_raw_min = 0x0FFF;
static uint16_t s_raw_max;
static uint32_t s_raw_sum;
static uint32_t s_raw_cnt;
/*
 * Channel/unit as reported by the DMA itself.
 *
 * In TYPE2 the sample is only valid when channel < ADC_CHANNEL_MAX, and the
 * frame carries the channel and unit that produced it.  Logging them settles
 * whether the continuous driver is really converting ADC1 channel 0 (GPIO1) or
 * something else -- the one-shot driver reads 513 mV on that pin while the
 * continuous one reports almost nothing, so "which channel is this actually?"
 * is the question that has to be answered.
 */
static uint8_t  s_raw_chan = 0xFF;
static uint8_t  s_raw_unit = 0xFF;

/*
 * Amplifier transition muting.
 *
 * The LM4871 emits a pop whenever its shutdown pin changes state -- the
 * internal bias has to ramp, and the resulting step appears across the speaker.
 * TI's own support forum has a thread about exactly this ("LM4871 has a pop
 * sound when enable shutdown").
 *
 * Reducing the BYPASS capacitor (C18, 2.2 uF -> 1 uF) made the pop quieter but
 * did not remove it, which confirms the pop is the switching itself rather than
 * a slow ramp.
 *
 * The standard cure is to mute the audio across the transition: hold the output
 * at digital silence while the amplifier switches and until it has settled.
 * The pop still happens electrically, it just has nothing to ride on.
 */
#define AMP_SETTLE_SAMPLES  (AUDIO_SAMPLE_RATE_HZ * 25 / 1000)   /* 25 ms */
static int s_amp_settle;

/* Samples that reached the int16 hard boundary, per reporting window.  This is
 * a diagnostic counter only; the audio path has no dynamic level processing. */
static uint32_t s_clip_count;
static volatile uint32_t s_clip_total;

uint32_t voice_hard_clip_total(void) { return s_clip_total; }

uint32_t voice_pool_ovf_total(void)
{
    portENTER_CRITICAL(&s_adc_diag_mux);
    const uint32_t total = s_adc_diag.ovf_total;
    portEXIT_CRITICAL(&s_adc_diag_mux);
    return total;
}

/*
 * Peak of the band-passed input since the last call, and a reset.
 *
 * The gain sweep needs to know whether the operator was actually talking
 * during a step: a step with no speech produces zero limiter hits for reasons
 * that have nothing to do with the gain, which silently corrupts the sweep.
 */
static volatile int s_peak_win;

int voice_take_peak(void)
{
    const int p = s_peak_win;
    s_peak_win = 0;
    return p;
}

/*
 * ---------------------------------------------------------------------------
 * There is deliberately NO noise gate / downward expander / noise suppressor
 * on this path.  Do not add one back.
 *
 * History: a -24 dBFS downward expander (-16 dB floor) was added to clean up
 * the SA818's AF_OUT hiss (HANDOVER 13.20).  It also attenuated every quiet
 * syllable, and on a narrowband FM link the Chinese digit "1" read as "yi" is
 * exactly that -- short, high, low energy.  The user heard "123" lose its
 * first digit unless it was read "yao", which is the classic symptom of a gate
 * sitting on speech.  Removed 2026-10-21 at the user's instruction, who also
 * asked that nothing similar be added again.
 *
 * The radio's own squelch is the only gate.  Hiss is solved in hardware (the
 * SA818's module volume / supply), not by processing the speech signal.
 * ---------------------------------------------------------------------------
 */

/*
 * Forward (net -> RF) gain.
 *
 * Measured on the air: speech captured from the SA818's AF_OUT peaks at
 * 3,000-6,000 counts (9-18 % of full scale) after the receive chain, so a
 * re-transmission driven straight from those samples deviates 1/4 to 1/10 of
 * what the original signal does -- the operator hears "quieter than the direct
 * signal".  This stage is applied only to audio that arrives from the network
 * and is about to be re-transmitted, so the local speaker and the SDR stream
 * keep their own levels.
 */
#define BGTX_DEFAULT 16      /* 2.19x = +6.8 dB.  Was 3.0x, which needed
                              * the soft limiter to stay out of clipping. */
static volatile int s_bgtx = BGTX_DEFAULT;

void voice_set_bgtx(int v)
{
    if (v < 0) { v = 0; }
    if (v > 64) { v = 64; }        /* 4.0x, beyond that the limiter just squashes */
    s_bgtx = v;
    nvs_handle_t h;
    if (nvs_open("orl", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_i32(h, "bgtx", v) == ESP_OK) {
            (void)nvs_commit(h);
        }
        nvs_close(h);
    }
}

int voice_get_bgtx(void)
{
    return s_bgtx;
}

/* Peak meters for the level question: what the receive chain produces, and what
 * leaves for the SA818.  Reading both is the only way to tell whether a quiet
 * re-transmission is a capture-gain problem or a missing TX stage. */
static volatile int s_meter_in;
static volatile int s_meter_out;

int voice_take_meter_in(void)
{
    const int v = s_meter_in;
    s_meter_in = 0;
    return v;
}

int voice_take_meter_out(void)
{
    const int v = s_meter_out;
    s_meter_out = 0;
    return v;
}

/* The speaker pop mute must never erase audio sent to the network/phone.
 * One hardware-COS confirmation block may be replayed only after valid COS,
 * with both capture-time and current receive-guard masks applied. */
static void rf_forward_one(const int16_t *pcm, size_t n, bool open, uint8_t blocked)
{
    if (blocked & RX_GUARD_OUT_NET) { bridge_rx_inhibit(); }
    else { bridge_rx_tap(pcm, n, open, open); }
    int16_t phone[OUT_MAX];
    memcpy(phone, pcm, n * sizeof(*pcm));
    (void)gain_mul(phone, n, s_net_rx_gain);
    sdr_rx_push(phone, n, open && !(blocked & RX_GUARD_OUT_SDR));
}

static void rf_forward_block(const int16_t *pcm, size_t n, bool rf_audio,
                             uint8_t blocked, int64_t block_start)
{
    const bool open = rf_audio && s_mod_sq_open;
    const uint8_t mode = netcom_mode();
    if (s_rf_prefix_n && open && block_start >= s_rf_prefix_us &&
        block_start - s_rf_prefix_us <= 64000 && mode == s_rf_prefix_mode) {
        rf_forward_one(s_rf_prefix, s_rf_prefix_n, true, blocked | s_rf_prefix_blocked);
    }
    s_rf_prefix_n = 0;
    rf_forward_one(pcm, n, open, blocked);
    if (rf_audio && !s_mod_sq_open && s_sq_gpio >= 0 && s_sq_gpio_run == 1 && n <= OUT_MAX) {
        memcpy(s_rf_prefix, pcm, n * sizeof(*pcm));
        s_rf_prefix_n = n; s_rf_prefix_blocked = blocked; s_rf_prefix_us = block_start;
        s_rf_prefix_mode = mode;
    }
}

static void monitor_task(void *arg)
{
    (void)arg;
    uint8_t *raw = s_raw;
    int16_t *out = s_out;
    int win_peak = 0;
    unsigned win_blocks = 0;
    int decim = 0;
    bool diagnostic_previous = false;

    if (adc_dma_start() != ESP_OK) {
        s_adc_error = true;
        s_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    speaker_diag_monitor_begin();
    audio_i2s_diag_receive(false);
    while (!s_stop) {
        speaker_diag_tick();
        if (speaker_diag_output_active()) {
            if (!diagnostic_previous) {
                bridge_rx_end();
                sdr_rx_push(NULL, 0, false);
            }
            diagnostic_previous = true;
            audio_i2s_diag_receive(false);
            /* Drain capture without pacing replay from its DMA completion.
             * The blocking I2S write below supplies the test's sample clock. */
            for (int drain = 0; drain < ADC_POOL_FRAMES; ++drain) {
                uint32_t discarded = 0;
                if (adc_continuous_read(s_adc_dma, raw, ADC_READ_BYTES, &discarded, 0) != ESP_OK) { break; }
                portENTER_CRITICAL(&s_adc_diag_mux);
                s_adc_diag.read_samples += discarded / ADC_ATOM_BYTES;
                portEXIT_CRITICAL(&s_adc_diag_mux);
            }
            const bool was_drain = speaker_diag_render(out, BLOCK_FRAMES);
            voice_speaker_apply(out, BLOCK_FRAMES);
            if (speaker_diag_output_amp_on() && !board_amp_is_enabled()) { board_amp_enable(true); }
            const esp_err_t e = audio_i2s_write(out, BLOCK_FRAMES);
            if (e == ESP_OK) {
                speaker_diag_render_written(out, BLOCK_FRAMES, was_drain);
            } else {
                speaker_diag_render_failed(was_drain);
            }
            continue;
        }
        if (diagnostic_previous) {
            diagnostic_previous = false;
            rx_band_reset(&s_rx_band);
            eq6_reset_state(&s_eq_spk);
            board_amp_enable(s_mod_sq_open);
        }
        uint32_t got = 0;
        /*
         * Read at most one 16 ms frame per iteration. The driver's byte buffer
         * can return a shorter contiguous span without waiting to fill it.
         * ADC_READ_BYTES explicitly, not sizeof(raw): raw is a pointer here.
         */
        esp_err_t err = adc_continuous_read(s_adc_dma, raw, ADC_READ_BYTES, &got, 20);
        if (err != ESP_OK) {
            ++s_stats.underruns;
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        const int64_t block_start = esp_timer_get_time();
        /* Each conversion is ADC_ATOM_BYTES wide (4 on the S3), not 2. */
        const int n = (int)(got / ADC_ATOM_BYTES);
        portENTER_CRITICAL(&s_adc_diag_mux);
        s_adc_diag.read_samples += (uint32_t)n;
        portEXIT_CRITICAL(&s_adc_diag_mux);
        int m = 0;                      /* output samples produced */
        int blk_peak = 0;
        uint32_t blk_raw_sum = 0;       /* unfiltered: the AF_OUT bias is DC, so the mean is the stable discriminator */
        int16_t *mid = s_mid;           /* band-passed, before gain */

        for (int i = 0; i < n; ++i) {
            const uint8_t *p = &raw[i * ADC_ATOM_BYTES];
            const uint32_t v32 = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            const uint16_t rawv = (uint16_t)(v32 & 0x0FFF);   /* type2: data:12 */
            if (!s_raw_cnt) {
                s_raw_chan = (uint8_t)((v32 >> 13) & 0x0F);
                s_raw_unit = (uint8_t)((v32 >> 17) & 0x01);
            }
            if (rawv < s_raw_min) { s_raw_min = rawv; }
            if (rawv > s_raw_max) { s_raw_max = rawv; }
            blk_raw_sum += rawv;
            s_raw_sum += rawv;
            ++s_raw_cnt;
            const float x = (float)rawv;

            /* Fixed RX band also removes AF_OUT's DC bias. */
            float v = rx_band_run(&s_rx_band, x);

            if (++decim < OVERSAMPLE) {
                continue;
            }
            decim = 0;

            /*
             * 16 kHz from here on. The division by OVERSAMPLE is only a scale
             * factor (the biquads are unity gain); it is folded into the gain
             * staging, so `gain` is calibrated with it in place.
             *
             * Both fixed receive filters have run before this decimation.
             */
            v = v / OVERSAMPLE;

            const int ac = (int)v;
            const int a = ac < 0 ? -ac : ac;
            if (a > win_peak)     { win_peak = a; }
            if (a > blk_peak)     { blk_peak = a; }
            if (a > s_peak_win)   { s_peak_win = a; }
            if (a > s_stats.peak_max) { s_stats.peak_max = a; }

            /* Recent-audio ring, pre-gain: exactly what the filters produced. */
            cap_push((int16_t)ac);

            if (m < OUT_MAX) {
                mid[m++] = (int16_t)ac;
            }
        }

        /*
         * Module RF squelch state -> amplifier (see MOD_SQ_* above).  The
         * per-chunk raw MEAN tracks the AF_OUT bias, which is DC: stable
         * while the module squelch is open regardless of the audio riding on
         * it.  Skipped during the output-chain noise test, where the
         * amplifier is pinned on by the test itself.
         */
        if (!s_amp_noise_test) {
            const uint32_t blk_raw_mean = (n > 0) ? (blk_raw_sum / (uint32_t)n) : 0;
            s_bias_raw = (int)blk_raw_mean;
            if (!s_bias_have) {
                s_bias_base = (float)blk_raw_mean;
                s_bias_have = true;
            }
            /* Evaluate a bias step BEFORE learning the idle baseline.  A
             * candidate carrier must not be absorbed into that baseline. */
            const bool bias_on = (float)blk_raw_mean > s_bias_base + MOD_SQ_MARGIN;
            if (!s_mod_sq_open && !bias_on && (float)blk_peak < SQ_AUDIO_OPEN) {
                if ((float)blk_raw_mean > s_bias_base) {
                    s_bias_base = (float)blk_raw_mean;
                } else {
                    s_bias_base -= (s_bias_base - (float)blk_raw_mean) * MOD_SQ_PEAK_DECAY;
                }
            }
            if (blk_peak > s_bpk_hold) { s_bpk_hold = blk_peak; }

            if (s_sq_gpio >= 0) {
                /* Active low: low means the module's squelch is open. */
                const bool hot = (gpio_get_level((gpio_num_t)s_sq_gpio) == 0);
                if (hot) {
                    if (s_sq_gpio_run < 0) { s_sq_gpio_run = 0; }
                    if (s_sq_gpio_run < 2) { ++s_sq_gpio_run; }
                } else {
                    if (s_sq_gpio_run > 0) { s_sq_gpio_run = 0; }
                    if (s_sq_gpio_run > -4) { --s_sq_gpio_run; }
                }
                bool want = s_mod_sq_open;
                if (s_sq_gpio_run >= 2) {
                    want = true;
                } else if (s_sq_gpio_run <= -4) {
                    want = false;
                }
                if (want != s_mod_sq_open) {
                    s_mod_sq_open = want;
                    ptt_trace_note(want ? PTT_TRACE_COS_ON : PTT_TRACE_COS_OFF,
                                   (unsigned)netcom_mode(), (uint32_t)s_sq_gpio, 0);
                    if (!want) { sa818_cfg_arm_refresh(); }   /* an over just ended */
                    board_amp_enable(want);
                    s_amp_settle = AMP_SETTLE_SAMPLES;
                    ESP_LOGI(TAG, "module squelch %s (hardware COS gpio %d)",
                             want ? "OPEN" : "closed", s_sq_gpio);
                }
            } else {

            const bool audio_on = (float)blk_peak >= SQ_AUDIO_OPEN;

            if (!s_mod_sq_open) {
                if (bias_on || audio_on) {
                    s_mod_sq_on_run++;
                    s_sq_quiet_ms = 0;
                } else {
                    s_mod_sq_on_run = 0;
                }
            } else if (bias_on || (float)blk_peak >= SQ_AUDIO_CLOSE) {
                s_mod_sq_off_run = 0;
                s_sq_quiet_ms = 0;              /* still something there */
            } else {
                s_sq_quiet_ms += SQ_BLOCK_MS;
                if (s_sq_quiet_ms >= SQ_HOLD_MS) {
                    s_mod_sq_off_run++;
                    s_mod_sq_on_run = 0;
                }
            }
            if (!s_mod_sq_open && s_mod_sq_on_run >= MOD_SQ_ON_BLOCKS) {
                s_mod_sq_open = true;
                /*
                 * Reset BOTH counters.  s_mod_sq_off_run used to stay at its
                 * "ready to close" value across a close, so once the squelch
                 * had closed once it closed again on the first quiet block after
                 * every reopen -- the state machine then toggled every 200-300 ms
                 * (measured on 192.0.2.178: OPEN/CLOSED/OPEN/CLOSED at a steady
                 * RSSI of 102).
                 */
                s_mod_sq_off_run = 0;
                s_sq_quiet_ms = 0;
                board_amp_enable(true);
                s_amp_settle = AMP_SETTLE_SAMPLES;   /* hide the enable pop */
                ESP_LOGI(TAG, "module squelch OPEN -- amplifier on");
            } else if (s_mod_sq_open && s_mod_sq_off_run >= MOD_SQ_OFF_BLOCKS) {
                s_mod_sq_open = false;
                s_mod_sq_on_run = 0;
                s_sq_quiet_ms = 0;
                board_amp_enable(false);
                s_amp_settle = AMP_SETTLE_SAMPLES;   /* hide the bias step down */
                ESP_LOGI(TAG, "module squelch closed -- amplifier off");
            }
            }   /* end software-inference path */
        }

        /* Fixed panel gain.  Saturation is only the int16 representation
         * boundary; no level detector or dynamic gain acts on these samples. */
        for (int i = 0; i < m; ++i) {
            const int raw = (int)mid[i] * s_gain;
            if (raw > 32767 || raw < -32768) {
                ++s_clip_count;
                ++s_clip_total;
            }
            out[i] = (int16_t)clamp16(raw);
        }

        /*
         * Input-level indicator (the status LED), with hysteresis.
         *
         * The amplifier is NOT switched here anymore: the SA818's own RF
         * squelch gates the audio (AF_OUT sits at 0 V while it is shut), so
         * the speaker follows the module with no software delay.  Only the
         * LED follows the level, with the post-TX release guard
         * (GATE_GUARD_MS) freezing it off while the module's bias step /
         * AGC settle transient is on the input.
         */
        if (s_gate_guard_ms &&
            (uint32_t)(esp_timer_get_time() / 1000) >= s_gate_guard_ms) {
            s_gate_guard_ms = 0;
        }
        if (!s_gate_guard_ms) {
            const int open_at = (s_sq_hold > 0) ? SQ_CLOSE_COUNTS : s_sq_open_counts;
            if (blk_peak >= open_at) {
                s_sq_hold = LED_HOLD_BLOCKS;
            } else if (s_sq_hold > 0) {
                --s_sq_hold;
            }
        } else if (s_sq_hold > 0) {
            s_sq_hold = 0;                 /* frozen: cannot be lit in the guard */
        }

        if (m > 0) {
            /*
             * Tail preview: an armed preview tone (voice_tail) replaces the
             * radio audio for these blocks.  The amplifier is forced on while
             * previewing -- the module-squelch gate would otherwise hold it
             * closed (nothing is being received), which would swallow the
             * tone.  This task is the DAC owner, so the preview never races
             * another writer.
             */
            const bool previewing = (tail_preview_fetch(out, (size_t)m) > 0);
            /* The SDR release state owns the DAC once queued browser PCM is
             * empty; capture it before choosing this block's source. */
            const bool sdr_tailing = voice_sdr_tail_active();
            s_blk_injected = false;
            s_blk_tx_processed = false;
            if (previewing) {
                if (!s_preview_amp) {
                    s_preview_amp = true;
                    board_amp_enable(true);
                }
            } else if (voice_net_tail_active()) {
                /* The over has ended: this task owns the DAC, so play the
                 * courtesy tone here and release the carrier when it is out. */
                voice_net_tail_run((size_t)m);
                memset(out, 0, (size_t)m * sizeof(int16_t));
                s_blk_injected = false;
                s_blk_tx_processed = false;
            } else if (sdr_tailing) {
                /* Drain a possible interpolation residue, then append the
                 * browser release tail on the monitor sample clock.  This
                 * transmit-bound block must not feed bridge or browser RX. */
                size_t injected = voice_bridge_take(out, (size_t)m);
                if (injected < (size_t)m) {
                    injected += voice_sdr_tail_fetch(out + injected,
                                                     (size_t)m - injected);
                }
                if (injected < (size_t)m) {
                    memset(out + injected, 0,
                           ((size_t)m - injected) * sizeof(int16_t));
                }
                s_blk_injected = injected != 0;
                /* Both the queued browser PCM and the generated browser tail
                 * already use the common TX processing path. */
                s_blk_tx_processed = injected != 0;
                if (!s_bridge_inj_amp) {
                    s_bridge_inj_amp = true;
                    board_amp_enable(false);
                }
            } else {
                if (s_preview_amp) {
                    s_preview_amp = false;
                    /*
                     * Give the amplifier back to the module-squelch gate
                     * instead of forcing it off.  Forcing it off while the
                     * squelch was open killed the receive audio (the gate's
                     * state was already "open", so its on-transition never
                     * fired again) -- the classic "after previewing a tail
                     * tone the monitor went silent".  s_mod_sq_open tracks
                     * the radio bias, not the previewed tone, so it is the
                     * correct resting state here.
                     */
                    board_amp_enable(s_mod_sq_open);
                }
                /*
                 * Analog bridge (net -> RF): while this task owns the DAC the
                 * network playback task pushes room audio in; play it through
                 * the same chain so it reaches the speaker AND the SA818's
                 * MIC_IN on the keyed transmitter.  Force the amplifier on:
                 * during our own TX the module squelch is normally closed,
                 * and the gate would swallow the repeated audio.
                 */
                ++s_bridge_blocks;
                /*
                 * Elastic playout for the network -> RF path.  The stream arrives
                 * in bursts (a browser microphone chunk every 64-256 ms) while
                 * this task drains at the I2S clock, and the two clocks never
                 * match exactly: without a level controller the ring drifts down
                 * to empty and every 16 ms block in between is padded with
                 * silence -- heard as chopped, "Morse code" audio.
                 *
                 * Hold a target level by taking one sample more (ring too full)
                 * or one less (ring too empty) per block: +-0.78 % of pitch,
                 * inaudible, and it self-corrects at 62 samples/s.
                 */
                /*
                 * Take exactly one block.  The earlier design trimmed by one
                 * sample per block to chase a buffer target, but the target was
                 * unreachable (a client supplies exactly real time), so the
                 * trim fired on EVERY block: one dropped or duplicated sample
                 * every 16 ms is a phase discontinuity that turns a continuous
                 * tone into broken, buzzy audio.  The transport now delivers
                 * small, frequent chunks and the prefill keeps a reserve, so no
                 * sample surgery is needed.
                 */
                /*
                 * Prime before draining, re-prime after an underrun: the rule
                 * every working PCM player follows (cushion first, silence on
                 * underrun, never a mid-block catch-up).  Without a cushion a
                 * real-time source supplies exactly what playback consumes, so
                 * the ring sits at zero and dips empty once per block -- a 16 ms
                 * silent hole every ~100 ms, heard as broken "Morse code" audio
                 * even with a locally generated tone.
                 */
                const size_t inj = voice_bridge_play(out, (size_t)m);
                if (inj > 0) {
                    s_blk_injected = true;
                    s_blk_tx_processed = voice_bridge_last_injection_preprocessed();
                    if (inj < (size_t)m) {
                        memset(out + inj, 0, ((size_t)m - inj) * sizeof(int16_t));
                    }
                    s_bridge_inj_amp = true;
                    board_amp_enable(!s_inject_silent);
                } else if (ptt_bridge_keyed()) {
                    /* Keyed by bridge, waiting for next packet: feed clean
                     * silence to SA818 MIC_IN rather than RX noise. */
                    memset(out, 0, (size_t)m * sizeof(int16_t));
                } else if (s_bridge_inj_amp) {
                    s_bridge_inj_amp = false;
                    board_amp_enable(s_mod_sq_open);
                    sa818_cfg_arm_refresh();   /* the network over just ended */
                }
            }

            /*
             * Which EQ bank this block gets.
             *
             * The operator's rule: the SPK bank is for audio the board is
             * receiving -- the local speaker, what the bridge forwards to the
             * room, what the browser listens to -- and the MIC bank is for audio
             * the board is putting on the air.  Injected blocks (room audio on
             * its way to the SA818, or the browser's own microphone) are the
             * transmit case, so they get the MIC bank; blocks captured from the
             * radio get the SPK bank.  Browser blocks already processed before
             * fan-out skip this second MIC pass.
             *
             * Before this, everything that reached the DAC went through the SPK
             * bank, so the MIC bank was only ever heard on the PTT-talk path and
             * looked completely dead from the panel.
             */
            const bool injected = s_blk_injected;
            const bool tx_processed = s_blk_tx_processed;
            s_blk_injected = false;
            s_blk_tx_processed = false;
            if (!tx_processed) {
                eq6_run(injected ? &s_eq_mic : &s_eq_spk, out, (size_t)m);
            }

            /*
             * Post-monitor-start fade (RX_FADE_BLOCKS): ramp the output from
             * silence over the first blocks -- that soaks the SA818's
             * TX -> RX settle transient instead of popping it through the
             * speaker the moment the module squelch (and therefore the
             * amplifier) reopens.
             */
            if (s_rx_fade_left > 0) {
                const float f = (float)(RX_FADE_BLOCKS - s_rx_fade_left)
                                / (float)RX_FADE_BLOCKS;
                for (int i = 0; i < m; ++i) {
                    out[i] = (int16_t)((int)out[i] * f);
                }
                --s_rx_fade_left;
            }

            const bool speaker_settling = s_amp_settle > 0;
            if (speaker_settling) {
                s_amp_settle -= m;
                if (s_amp_settle < 0) { s_amp_settle = 0; }
            } else if (s_amp_noise_test) {
                /*
                 * Output-chain noise-floor test: amplifier ON, DAC fed pure
                 * digital silence.  Any sound now is the output stage's own.
                 */
                memset(out, 0, sizeof(int16_t) * (size_t)m);
            }
            /*
             * s_bypass_squelch is kept for API compatibility: with the
             * amplifier no longer gated, "bypass" is simply the normal path.
             */
            (void)s_bypass_squelch;
            /*
             * Optional on/off gate for the audible self-test.  Applied to the
             * output only, so the input level readings stay meaningful.
             */
            if (s_ab_gate_ms > 0) {
                const unsigned per_phase = (unsigned)(s_ab_gate_ms / 16);
                if (per_phase > 0 && ((s_stats.blocks / per_phase) & 1u)) {
                    memset(out, 0, sizeof(int16_t) * (size_t)m);
                }
            }
            /*
             * Analog bridge (RF -> net): forward what the speaker is playing
             * -- the squelch-gated radio audio, after gain/limiter/EQ/fade --
             * to the active room, with the module squelch state as the ptt
             * flag.  Skipped while a preview tone consumes the blocks (a
             * local test, not the channel) and while bridge audio injected
             * from the room is still draining (half-duplex: the room does not
             * need to hear itself coming back).
             */
            /* Fixed-time RX inhibition affects only RF-origin blocks. Network
             * playout, local previews and the transmitter's own tail continue.
             * Keep raw COS/meters intact for diagnosis. Do not buffer mute data. */
            const uint8_t rx_blocked = rx_guard_blocked_at(block_start);
            const bool rf_audio = !previewing && !sdr_tailing && !injected &&
                                  !s_bridge_inj_amp && !ptt_bridge_keyed();
            rf_forward_block(out, (size_t)m, rf_audio, rx_blocked, block_start);

            /*
             * SDR mode: follow the same module squelch state as RF -> network.
             * `out` still contains the ADC floor while the module is muted;
             * the speaker amplifier hides it locally, but sending it here would
             * make the browser play a continuous hiss.  This is an explicit
             * module-state gate, never an amplitude/RSSI gate.
             */

            /*
             * Speaker volume, applied to the copy that goes to the DAC.
             *
             * Order matters: the RF -> network tap above already has this
             * audio, and while the bridge is keying the radio these very
             * samples are also our RF re-transmission -- so the trim is
             * skipped then.  Everything else here (radio receive audio, network
             * audio being listened to, a previewed tail tone) is bound for the
             * loudspeaker and may be attenuated.
             */
            int16_t diag_common[OUT_MAX];
            memcpy(diag_common, out, (size_t)m * sizeof(int16_t));
            if (!ptt_bridge_keyed()) {
                voice_speaker_apply(out, (size_t)m);
            }
            if (rf_audio && (speaker_settling || (rx_blocked & RX_GUARD_OUT_SPK))) {
                memset(out, 0, (size_t)m * sizeof(int16_t));
            }
            const bool diag_receive = s_mod_sq_open && !previewing &&
                !injected && !sdr_tailing && ptt_owner() == PTT_OWNER_NONE;
            audio_i2s_diag_receive(diag_receive);
            speaker_diag_observe(diag_common, out, (size_t)m, diag_receive,
                (uint32_t)(esp_timer_get_time() - block_start));

            const int64_t write_start = esp_timer_get_time();
            const esp_err_t write_err = audio_i2s_write(out, (size_t)m);
            voice_bridge_network_written();
            if (write_err == ESP_OK) {
                ++s_stats.blocks;
                voice_sdr_tail_block_commit();
            }
            const uint32_t write_us = (uint32_t)(esp_timer_get_time() - write_start);
            portENTER_CRITICAL(&s_adc_diag_mux);
            if (write_us > s_adc_diag.i2s_write_max_us) {
                s_adc_diag.i2s_write_max_us = write_us;
            }
            portEXIT_CRITICAL(&s_adc_diag_mux);
            {
                int pk = 0;
                for (size_t i = 0; i < (size_t)m; ++i) {
                    const int a = out[i] < 0 ? -out[i] : out[i];
                    if (a > pk) { pk = a; }
                }
                if (pk > s_meter_out) { s_meter_out = pk; }
                if (pk > s_meter_in)  { s_meter_in = pk; }
            }
        }

        /*
         * ~0.32 s per line (20 blocks of 16 ms): fine enough to see the shape of
         * a transmission -- squelch-open transient on key-up, the speech
         * envelope, then the drop back when the carrier goes away.  A 1 s window
         * was too coarse to tell "voice is arriving" from "the squelch just
         * opened", which cost a lot of time earlier.
         */
        if (++win_blocks >= 20) {
            s_stats.peak_in = win_peak;
            /*
             * RSSI alongside the audio level is what separates "the operator
             * was not transmitting" from "a carrier is present but no audio is
             * coming out of the module": if RSSI is high while the audio level
             * sits at the noise floor, the module is receiving and not
             * demodulating, which is a completely different fault.
             */
            if (!s_quiet) {
                voice_adc_diag_t adc;
                voice_adc_diag_get(&adc);
                const uint32_t raw_mean = s_raw_cnt ? (s_raw_sum / s_raw_cnt) : 0;
                if (!s_level_pending) {
                    snprintf(s_level_line, sizeof(s_level_line),
                             "level: peak %4d counts  rssi=%3d  gain=%d  "
                             "blocks=%u clip=%u ovf=%u adc=%uHz read=%uHz",
                             win_peak, sa818_state()->rssi,
                             s_gain,
                             s_stats.blocks, (unsigned)s_clip_count,
                             (unsigned)adc.ovf_total, (unsigned)adc.produced_hz,
                             (unsigned)adc.read_hz);
                    s_level_pending = true;
                }
                s_clip_count = 0;
                /*
                 * Raw statistics are diagnostic only, so they go out on every
                 * tenth window instead of every one -- the level line is the
                 * one worth watching during normal use.
                 */
                if ((s_stats.blocks / 20) % 10 == 0) {
                    ESP_LOGI(TAG, "       raw ADC: min %4u  max %4u  mean %4u  "
                                  "chan=%u unit=%u",
                             (unsigned)s_raw_min, (unsigned)s_raw_max,
                             (unsigned)raw_mean,
                             (unsigned)s_raw_chan, (unsigned)s_raw_unit);
                }
                s_raw_min = 0x0FFF;
                s_raw_max = 0;
                s_raw_sum = 0;
                s_raw_cnt = 0;
                s_raw_chan = 0xFF;
            }
            win_peak = 0;
            win_blocks = 0;
        }
    }
    speaker_diag_monitor_end();
    audio_i2s_diag_receive(false);
    /* Capture can stop without a final closed-squelch block (e.g. TALK takes
     * the ADC). Finish the RF -> network stream before another producer starts. */
    bridge_rx_end();
    /* Stop the ADC here, in the same task that started it (see above). */
    adc_dma_stop();
    board_amp_enable(false);    /* the speaker belongs to this task's lifetime */
    s_task_running = false;
    vTaskDelete(NULL);
}

/*
 * Transmit-path gain, fixed-point at 1/16.
 *
 * Declared here because the TALK task below uses it, but defined (and
 * persisted) with the other settings at the bottom of this file.  At file
 * scope this is a tentative definition, which the later initialised
 * definition completes -- it is not a duplicate.
 */
static int s_tx_gain;
/* Network/app PCM has its own multiplier: the phone stream and the handset
 * capsule need different amounts of gain, and one shared knob forced a
 * compromise (a level that kept the phone audible clipped the local mic). */
static int s_net_tx_gain;

/* Apply one fixed transmit multiplier.  This is deliberately a plain fixed gain
 * followed by integer saturation: no level detector, compressor, soft limiter
 * or other dynamic processing. */
static int gain_mul(int16_t *buf, size_t n, int gain)
{
    if (!buf || n == 0) {
        return 0;
    }
    int peak = 0;
    for (size_t i = 0; i < n; ++i) {
        const int v = clamp16((int)((float)buf[i] * (float)gain / 16.0f));
        buf[i] = (int16_t)v;
        const int a = v < 0 ? -v : v;
        if (a > peak) {
            peak = a;
        }
    }
    return peak;
}

/* A courtesy tone is played by whichever task owns the DAC.  Both the local
 * TALK path and the network bridge path use this one loop, so a tail behaves
 * identically no matter where the over came from. */
typedef void (*tail_sink_fn)(int16_t *buf, size_t n, void *ctx);

static int tx_gain_apply(int16_t *buf, size_t n);

struct talk_tail_ctx { bool network; bool rf_also; };

static void talk_tail_sink(int16_t *b, size_t n, void *ctx)
{
    struct talk_tail_ctx *c = (struct talk_tail_ctx *)ctx;
    (void)tx_gain_apply(b, n);
    if (!c->network) {
        (void)audio_i2s_write(b, n);          /* blocking = sample-locked */
        return;
    }
    netcom_tx_sample(b, n, true);
    if (c->rf_also) {
        (void)audio_i2s_write(b, n);
    } else {
        /* No sample clock here: pace against the wall clock. */
        vTaskDelay(pdMS_TO_TICKS(1000 * (int)n / AUDIO_SAMPLE_RATE_HZ));
    }
}

static bool tail_play(int id, int16_t *buf, size_t frames,
                      tail_sink_fn sink, void *ctx)
{
    const size_t total = tail_samples(id);
    if (id <= TAIL_OFF || id >= TAIL_COUNT || total == 0) { return false; }
    size_t pos = 0;
    while (pos < total) {
        ptt_extend(1000);
        const size_t n = tail_fetch(id, pos, buf, frames);
        if (!n) { return false; }
        pos += n;
        sink(buf, n, ctx);
    }
    return true;
}

/* TALK (handset microphone) and its courtesy tone. */
static int tx_gain_apply(int16_t *buf, size_t n) { return gain_mul(buf, n, s_tx_gain); }

/*
 * Transmit pre-emphasis, +6 dB/octave above 1.2 kHz (about +9 dB at 3 kHz).
 *
 * Commercial receivers de-emphasise by the same slope, so a transmitter that
 * skips pre-emphasis sounds dull however loud it is.  The SA818 has its own
 * pre/de-emphasis field, but enabling it also changes our receive path and was
 * measured to inject a buzz there, so the slope is applied here instead -- only
 * to the audio on its way to the radio.  RBJ high shelf, fs = 8 kHz.
 */
static bool s_tx_preemph = true;
static float s_lo_z1, s_lo_z2, s_hi_z1, s_hi_z2;

static void tx_preemph(int16_t *buf, size_t n)
{
    if (!buf || n == 0 || !s_tx_preemph) {
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        float x = (float)buf[i];
        /* Low shelf 500 Hz, -11 dB: takes the mud out of the transmit voice. */
        float y = 0.839355f * x + s_lo_z1;
        s_lo_z1 = -1.340531f * x - (-1.267241f) * y + s_lo_z2;
        s_lo_z2 = 0.558700f * x - 0.471344f * y;
        /* High shelf 1.8 kHz, +9 dB: the slope a de-emphasising receiver expects. */
        x = y;
        y = 1.760160f * x + s_hi_z1;
        s_hi_z1 = -0.841128f * x - 0.118540f * y + s_hi_z2;
        s_hi_z2 = 0.373566f * x - 0.174058f * y;
        buf[i] = (int16_t)clamp16((int)(y >= 0 ? y + 0.5f : y - 0.5f));
    }
}

/*
 * Peak limiter, -1 dBFS ceiling.
 *
 * Raising the network gain to +24 dB is what makes the handset audible on the
 * other side, so the level cannot simply be turned down; without this the only
 * outcome is integer clipping, which the far end hears as breakup.  Attack is
 * instantaneous (the envelope jumps to the sample), release is ~50 ms.
 */
#define TX_LIMIT_CEIL 29204.0f          /* 32767 * 10^(-1/20) */
static float s_lim_env;

static void tx_limit(int16_t *buf, size_t n)
{
    if (!buf || n == 0) {
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        const float a = buf[i] < 0 ? -(float)buf[i] : (float)buf[i];
        if (a > s_lim_env) {
            s_lim_env = a;                       /* instant attack */
        } else {
            s_lim_env = s_lim_env * 0.9993f + a * 0.0007f;  /* ~50 ms release */
        }
        float g = 1.0f;
        if (s_lim_env > TX_LIMIT_CEIL) {
            g = TX_LIMIT_CEIL / s_lim_env;
        }
        const float y = (float)buf[i] * g;
        buf[i] = (int16_t)clamp16((int)(y >= 0 ? y + 0.5f : y - 0.5f));
    }
}

/* Browser PCM uses the same microphone EQ and fixed TX gain as ordinary TALK.
 * The caller may fan this one processed buffer out to network and local RF. */
int voice_apply_tx_processing(int16_t *buf, size_t n)
{
    if (!buf || n == 0) {
        return 0;
    }
    eq6_run(&s_eq_mic, buf, n);
    tx_preemph(buf, n);
    const int peak = tx_gain_apply(buf, n);
    tx_limit(buf, n);
    return peak;
}

/* Phone/browser PCM: same microphone EQ, but the network TX multiplier.
 * Kept separate so a level that suits the handset capsule cannot force the
 * phone stream to clip, and vice versa. */
int voice_apply_net_tx_processing(int16_t *buf, size_t n)
{
    if (!buf || n == 0) {
        return 0;
    }
    eq6_run(&s_eq_mic, buf, n);
    tx_preemph(buf, n);
    const int peak = gain_mul(buf, n, s_net_tx_gain);
    tx_limit(buf, n);
    return peak;
}

/*
 * Transmit level reporting, about once a second.
 *
 * The compressor that used to live here is gone -- see the note in talk_task
 * for the measurements that condemned it.  Only the logging counter remains.
 */
static int s_tx_log_blocks;

/*
 * TALK: microphone (codec ADC via I2S RX) -> codec DAC -> SA818 MIC_IN.
 *
 * The PTT deadline is re-extended every block.  If this task ever stops
 * running, the previously granted window expires and the transmitter is
 * released -- the safety property is preserved by construction.
 */
static void talk_task(void *arg)
{
    const uint32_t generation = (uint32_t)(uintptr_t)arg;
    int16_t buf[BLOCK_FRAMES * 2];
    bool ptt_released = false;

    while (!s_stop) {
        size_t got = 0;
        if (audio_i2s_read(buf, BLOCK_FRAMES, &got) != ESP_OK || got == 0) {
            ++s_stats.underruns;
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        s_stats.samples += (unsigned)got;
        /* Match the browser TX source to this established TALK path: the
         * operator's microphone EQ, then one fixed TX gain. */
        const int out_peak = voice_apply_tx_processing(buf, got);
        if (out_peak > s_stats.peak_in) { s_stats.peak_in = out_peak; }
        if (++s_tx_log_blocks >= 62) {
            s_tx_log_blocks = 0;
            ESP_LOGI(TAG, "TX: gain %.2fx  out peak %5d  (full scale 32767)",
                     (double)s_tx_gain / 16.0, out_peak);
        }
        if (netcom_mode() != NETCOM_MODE_LOCAL) {
            /*
             * Network modes (P2P/relay): the shaped mic audio goes to the
             * room over the network.  The engine's playback task owns the DAC
             * (remote audio or silence), so this task must not write to it --
             * unless the operator asked for a simultaneous local transmission,
             * in which case we take the DAC over (voice_dac_held_by_talk()).
             */
            netcom_tx_sample(buf, (size_t)got, ptt_get_state() == PTT_TX);
            if (s_rf_also) {
                (void)audio_i2s_write(buf, got);   /* same audio on air */
            }
        } else {
            audio_i2s_write(buf, got);
        }
        ++s_stats.blocks;

        /*
         * The operator's release request: stop taking new audio, but keep the
         * transmitter keyed while the tail tone below plays (see
         * voice_request_talk_end).  The PTT state is still TX here.
         */
        if (s_release_requested) {
            ptt_released = true;
            break;
        }

        /* Keep the transmitter keyed, one safety window at a time. */
        if (ptt_get_state() == PTT_TX) {
            ptt_extend(1000);
        } else if (ptt_get_state() == PTT_IDLE || ptt_get_state() == PTT_FAULT) {
            ESP_LOGW(TAG, "PTT left TX (state=%s) -- stopping talk mode",
                     ptt_state_name());
            ptt_released = true;
            break;
        }
    }
    /*
     * End-of-transmission tail tone: while it sounds, the transmitter stays
     * keyed, so the tone is actually transmitted (local: out the SA818 to
     * the air; network: published as ordinary audio frames).  The classic
     * amateur courtesy signal -- K-CHOO means "over to you".  See voice_tail.
     *
     * Runs before s_task_running is cleared: voice_stop() only releases the
     * PTT once this task has exited, and in local mode the DAC is still ours.
     */
    {
        const int tid = tail_get();
        const size_t total = tail_samples(tid);
        if (tid == TAIL_OFF || total == 0) {
            if (tid != TAIL_OFF) {
                ESP_LOGW(TAG, "tail '%s' selected but empty -- nothing sent",
                         tail_name(tid));
            }
        } else if (ptt_get_state() != PTT_TX) {
            /*
             * The radio is already unkeyed (deadline, fault, an external stop):
             * writing the tone now would produce audio nobody transmits and
             * would look like "PTT was held for the tone" in the logs.  Say so
             * instead of pretending.
             */
            ESP_LOGW(TAG, "tail '%s' skipped: PTT is already %s",
                     tail_name(tid), ptt_state_name());
        } else {
            const bool network = netcom_mode() != NETCOM_MODE_LOCAL;
            /* Microphone capture is finished; reuse its buffer for the tail
             * instead of retaining another PCM block on the TALK stack. */
            int16_t *tbuf = buf;
            size_t pos = 0;
            ESP_LOGI(TAG, "tail: %s (%u samples, %ss)", tail_name(tid),
                     (unsigned)total, network ? " network" : " to air");
            /*
             * Cover the WHOLE tone up front.
             *
             * The voice loop only ever granted ~1 s, refreshed per audio block.
             * Fetching the first tail block can block for longer than that
             * (flash access in an older build, a busy I2S, another task holding
             * the CPU): the window then expired mid-fetch, the radio unkeyed,
             * and a short transmission lost its tail completely -- or a long one
             * lost just the opening.  Granting the tone's own length (+1.5 s of
             * margin) makes the release independent of how long the first
             * fetch takes.  ptt_extend() still clamps to the absolute ceiling.
             */
            ptt_extend((int)(total * 1000 / AUDIO_SAMPLE_RATE_HZ) + 1500);
            /*
             * Independent of s_stop: an explicit voice_stop() (panel PTT
             * release) IS a normal end of transmission, and it releases the
             * PTT only after this task exits -- so the key holds either way.
             */
            struct talk_tail_ctx tctx = { .network = network, .rf_also = s_rf_also };
            (void)tail_play(tid, tbuf, BLOCK_FRAMES, talk_tail_sink, &tctx);
            /*
             * The tone is NOT played back on the local speaker (user
             * requirement): in local mode the amplifier stays off as during
             * talk, so the DAC output reaches only the SA818 MIC_IN and the
             * tone goes to the air (the far end hears it); in network modes
             * the frames go to the room, whose own playback decides the
             * sound, and the local board filters its own frames.
             */
        }
    }
    /* Every network talk needs an end marker, including TAIL_OFF and a
     * skipped/empty tail. Otherwise peers retain PTT until its timeout. */
    if (netcom_mode() != NETCOM_MODE_LOCAL) {
        netcom_tx_end();
    }
    /*
     * Release the PTT now that the tone is fully out.  For an operator
     * release this is the first unkey (the request only set a flag); for the
     * timer/deadline/FAULT paths the state machine is already TAIL or IDLE
     * and the call is idempotent.  A follow-up voice_stop() from outside
     * calls it again -- also idempotent.
     */
    /* Give the DAC back first: the engine's playback loop resumes on its next
     * packet, which must not race the unkey. */
    s_dac_talk = false;
    s_rf_also = false;
    ptt_release(PTT_OWNER_TALK, generation);
    s_stop = true;
    /* RF keying is restored only by the matching TX owner. */     /* always restore physical RF keying on exit */
    s_task_running = false;      /* all hardware access has finished */
    control_lock();
    if (ptt_released && generation == s_control_generation) {
        /*
         * The transmission ended on its own (web timer expired, or the
         * absolute PTT cap was hit): the device's resting state is receive,
         * so come straight back to MONITOR instead of idling in TALK mode
         * with no audio task running at all.  An explicit voice_stop() sets
         * s_stop from outside first, in which case the operator meant "stop"
         * and we stay stopped.
         *
         * Safe from this task: voice_stop() below only waits on
         * s_task_running, which we have just cleared, and s_task is never
         * read by anyone.
         */
        if (netcom_mode() != NETCOM_MODE_LOCAL) {
            /*
             * In network modes the resting state is "listening to the room
             * over the network": unless the analog bridge is still borrowing
             * the monitor task for its RF -> net tap, the engine's playback
             * task owns the DAC.
             */
            s_mode = VOICE_OFF;   /* was VOICE_TALK: nothing is talking now */
            if (bridge_active() || sdr_enabled()) {
                (void)voice_monitor_instant();
                ESP_LOGW(TAG, "transmission ended -- bridge/SDR monitor resumed");
            } else {
                ESP_LOGW(TAG, "transmission ended -- network listening");
            }
        } else if (voice_start(VOICE_MONITOR) == ESP_OK) {
            ESP_LOGW(TAG, "transmission ended -- back to MONITOR");
        }
    }
    control_unlock();
    /* High-water mark is reported in stack words; publish bytes plus the internal
     * heap so the next build can decide whether 6 KiB still has margin. */
    ESP_LOGI(TAG, "TALK stack high-water %u B free of %u B, internal heap %u B",
             (unsigned)uxTaskGetStackHighWaterMark(NULL) * (unsigned)sizeof(StackType_t),
             (unsigned)TALK_STACK_BYTES, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------- start */

static esp_err_t voice_stop_locked(void)
{
    s_last_stop_us = esp_timer_get_time();
    sa818_cfg_arm_refresh();                    /* the over is over */
    /* The monitor drains pending test PCM without holding the PTT lock. */
    if (speaker_diag_output_active()) {
        (void)speaker_diag_command("stop", 1000);
        for (int i = 0; i < 30 && speaker_diag_output_active(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (speaker_diag_output_active()) { return ESP_ERR_TIMEOUT; }
    }
    if (s_mode == VOICE_OFF && !s_task_running) {
        return ESP_OK;
    }
    s_stop = true;

    /*
     * Wait for the audio task to exit before doing anything else.
     *
     * The task stops the ADC driver itself -- see the note in monitor_task --
     * so this function must not touch it at all.
     */
    /*
     * Up to 6 s: the end-of-transmission tail tone (voice_tail) can hold the
     * task for up to its own length (5 s max) before it signals exit.
     */
    for (int i = 0; i < 600 && s_task_running; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_task_running) {
        ESP_LOGE(TAG, "audio task did not stop within 6 s");
        return ESP_ERR_TIMEOUT;
    }

    /* TALK releases its own token before acknowledging task exit.
     * Stopping MONITOR must never release another source or a reservation. */
    board_amp_enable(false);
    s_task = NULL;

    s_mode = VOICE_OFF;
    s_stop = false;
    s_release_requested = false;
    ESP_LOGI(TAG, "voice stopped");
    return ESP_OK;
}

/*
 * End a TALK session the same way an operator pressing the PTT release does:
 * the talk task notices the flag, finishes the transmission (including the
 * end-of-transmission tail tone, with the transmitter still keyed), and
 * releases the PTT itself.  Unlike ptt_request_rx() this does not unkey the
 * radio before the tail has been sent.  Safe to call when no talk is running
 * (the flag is consumed, or cleared, on the next voice_start()).
 */
void voice_request_talk_end(void)
{
    s_release_requested = true;
}

uint32_t voice_talk_token(void)
{
    ptt_control_lock();
    const uint32_t token = ptt_owned(PTT_OWNER_TALK, s_control_generation) ? s_control_generation : 0;
    ptt_control_unlock();
    return token;
}

bool voice_request_talk_end_for(uint32_t token)
{
    ptt_control_lock();
    const bool ok = token && s_mode == VOICE_TALK && ptt_owned(PTT_OWNER_TALK, token);
    if (ok) { s_release_requested = true; }
    ptt_control_unlock();
    return ok;
}

static void sqgpio_load(void)
{
    nvs_handle_t h;
    if (nvs_open("orl", NVS_READONLY, &h) == ESP_OK) {
        int32_t v = -1;
        /* 0 or a GPIO is an explicit operator choice and is applied.  -1 (what
         * older firmware wrote for "disabled") or a missing key means the board
         * was never configured, so SQ_GPIO_DEFAULT stands. */
        if (nvs_get_i32(h, "sqgp", &v) == ESP_OK && v >= 0 && v <= 48) {
            voice_set_sq_gpio((int)v);
        }
        nvs_close(h);
    }
}

static void bgtx_load(void)
{
    nvs_handle_t h;
    if (nvs_open("orl", NVS_READONLY, &h) == ESP_OK) {
        int32_t v = 0;
        if (nvs_get_i32(h, "bgtx", &v) == ESP_OK && v >= 0 && v <= 64) {
            s_bgtx = (int)v;
        }
        nvs_close(h);
    }
}

static esp_err_t voice_start_locked(voice_mode_t mode)
{
    bgtx_load();
    sqgpio_load();
    if (mode == VOICE_OFF) {
        return voice_stop_locked();
    }
    const esp_err_t stopped = voice_stop_locked();
    if (stopped != ESP_OK) { return stopped; }

    if (!voice_bufs_alloc()) {
        return ESP_ERR_NO_MEM;
    }
    if (audio_codec_init() != ESP_OK) {
        ESP_LOGE(TAG, "codec not available");
        return ESP_FAIL;
    }
    if (audio_i2s_start_clock_only() != ESP_OK) {
        return ESP_FAIL;
    }

    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.sample_rate = AUDIO_SAMPLE_RATE_HZ;
    s_stats.gain = s_gain;
    s_stop = false;
    s_release_requested = false;
    /* Reset the filter chain: stale state would ring on the first block. */
    rx_band_reset(&s_rx_band);
    s_amp_settle = 0;
    s_sq_hold = 0;
    s_rx_fade_left = 0;
    s_mod_sq_open = false;
    s_rf_prefix_n = 0;
    s_mod_sq_on_run = 0;
    s_mod_sq_off_run = 0;
    s_sq_quiet_ms = 0;
    s_sq_gpio_run = 0;
    /*
     * A restart a moment after the last session (PTT release, mode toggle) does
     * not need the cold-start ceremony: the module is still tuned and its bias
     * is still valid.  Relearning and fading here is what ate the first
     * syllable of every forwarded over.
     */
    const bool warm = s_last_stop_us != 0 &&
        (esp_timer_get_time() - s_last_stop_us) <= 2000000LL &&
        sa818_cfg_tx_ready();
    if (!warm) {
        s_bias_have = false;
    }
    /* Stale EQ filter state would ring on the first block of a new session. */
    eq6_reset_state(&s_eq_spk);
    eq6_reset_state(&s_eq_mic);
    if (mode == VOICE_MONITOR) {
        s_gate_guard_ms = (uint32_t)(esp_timer_get_time() / 1000) + GATE_GUARD_MS;
        if (s_instant_start) {
            /*
             * The monitor was started only to play a tail preview: the
             * 500 ms input ramp would swallow a short built-in tone
             * entirely, so skip the settle fades in this case.
             */
            s_instant_start = false;
            s_rx_fade_left = 0;
        } else {
            /* A warm restart skips the ramp entirely: it is 48 ms long now, and
             * any of it still trims the front of the next over. */
            s_rx_fade_left = warm ? 0 : RX_FADE_BLOCKS;
            ESP_LOGI(TAG, "monitor: post-TX settle -- LED frozen %d ms, input "
                     "fading in over %d blocks%s",
                     GATE_GUARD_MS, s_rx_fade_left, warm ? " (warm start)" : "");
        }
    } else {
        s_gate_guard_ms = 0;
    }

    /*
     * The transmit path now uses a fixed gain, so there is no compressor state
     * to clear here; only the level-report counter is reset.
     */
    s_tx_log_blocks = 0;

    if (mode == VOICE_MONITOR) {
        /*
         * The amplifier starts OFF and is switched by the module-squelch
         * state machine in the monitor task (see MOD_SQ_*): the SA818's
         * own RF squelch owns the speaker, end to end.  Empty channel ->
         * AF_OUT 0 V -> amplifier off -> no hiss.  The input fade
         * (s_rx_fade_left) still soaks the post-TX settle transient for
         * the first blocks after every monitor start.
         */
        board_amp_enable(false);
        s_mode = VOICE_MONITOR;
        s_cap_on = cap_ensure();
        s_adc_error = false;
        s_task_running = true;
        /*
         * Priority 12, not 8: the NET-P2P engine and playout tasks also run at 8
         * on core 1, so at equal priority they interleave with this one -- and
         * anything they do that blocks (a console write, an NVS commit) is
         * time the ADC pool spends filling.  The audio path is the hard real-time
         * consumer here; everything else can wait a tick.
         */
        if (xTaskCreatePinnedToCore(monitor_task, "mon", 6144, NULL, 12, &s_task, 1)
            != pdPASS) {
            s_task_running = false;
            voice_stop_locked();
            return ESP_ERR_NO_MEM;
        }
        /* The task starts the ADC itself; give it a moment to report failure. */
        for (int i = 0; i < 30 && s_task_running && !s_adc_error; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_adc_error) {
            ESP_LOGE(TAG, "ADC continuous start failed");
            voice_stop_locked();
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "MONITOR: radio AF_OUT -> speaker (gain %d)", s_gain);
        return ESP_OK;
    }

    /* VOICE_TALK: the caller reserved TX before stopping MONITOR. */
    for (int w = 0; w < 20 && ptt_get_state() == PTT_TAIL; ++w) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    const bool net_talk = (netcom_mode() != NETCOM_MODE_LOCAL);
    /*
     * A network talk with no working network is worse than a refusal: the audio
     * goes into the engine and is silently dropped (broker not connected, no
     * peer linked), so the operator talks to nobody while the panel claims the
     * talk is running.  Refuse loudly instead.
     */
    if (net_talk && !netcom_tx_ready()) {
        ESP_LOGE(TAG, "network talk refused: %s", netcom_tx_ready_hint());
        ptt_set_rf_keying(true);
        return ESP_ERR_INVALID_STATE;
    }
    board_amp_enable(false);             /* avoid acoustic feedback */
    /*
     * Network modes keep the radio in RX unless the operator asked for both
     * ("网络 + 本机射频"): then the same microphone audio also goes to the SA818
     * MIC_IN and the local radio transmits as well.
     */
    s_rf_also = net_talk && netcom_rf_tx();
    ptt_set_rf_keying(!net_talk || s_rf_also);
    if (ptt_request_tx(2000) != ESP_OK) {
        if (net_talk) { ptt_set_rf_keying(true); }
        ESP_LOGE(TAG, "talk refused: PTT state is %s", ptt_state_name());
        return ESP_ERR_INVALID_STATE;
    }
    /* Wait for the (logical or real) key before feeding audio. */
    for (int i = 0; i < 100 && ptt_get_state() == PTT_PREPARE; ++i) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (ptt_get_state() != PTT_TX) {
        if (net_talk) { ptt_set_rf_keying(true); }
        ESP_LOGE(TAG, "talk: never reached TX (state=%s)", ptt_state_name());
        ptt_request_rx();
        return ESP_FAIL;
    }
    s_mode = VOICE_TALK;
    s_task_running = true;
    /* Hand the DAC over before the first block: the engine's playback loop polls
     * this flag once per packet, so it must be set before we start writing. */
    s_dac_talk = s_rf_also;
    /* TALK also restarts MONITOR on release, including NVS/driver calls.
     * Its former 4 KiB stack did not cover that nested completion path. */
    s_task = xTaskCreateStaticPinnedToCore(talk_task, "talk", TALK_STACK_BYTES / sizeof(StackType_t),
        (void *)(uintptr_t)s_control_generation, 12, s_talk_stack, &s_talk_tcb, 1);
    if (!s_task) {
        s_dac_talk = false;
        s_task_running = false;
        voice_stop_locked();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "%s", net_talk
             ? "TALK: microphone -> network (radio stays in RX)"
             : "TALK: microphone -> SA818 MIC_IN, transmitter keyed");
    return ESP_OK;
}

/*
 * Start monitor mode with the post-TX settle fades skipped.  Used by the web
 * panel's tail preview in local mode when no voice path is running: the
 * monitor task is the DAC owner that will play the armed preview, and the
 * 500 ms input ramp would swallow a short built-in tone before it is
 * audible.
 */
esp_err_t voice_monitor_instant(void)
{
    s_instant_start = true;
    return voice_start(VOICE_MONITOR);
}

/*
 * Monitor-task borrowing.
 *
 * Things need the monitor task as a DAC owner without the operator asking for
 * it: the tail preview (local mode, nothing else running), the analog bridge
 * (its RF -> net tap lives in the monitor task's output chain) and the SDR
 * web-audio mode (the RF -> browser tap plus the browser-mic injection).  The
 * borrow starts the monitor only when nothing is running, and stops it again
 * when the last borrower is done -- unless the operator took over in the
 * meantime, which is authoritative.
 */
static bool s_borrow_preview;
static bool s_borrow_bridge;
static bool s_borrow_sdr;
static bool s_borrowed;

static void monitor_borrow_locked(const char *who)
{
    if (!who) {
        return;
    }
    if (!strcmp(who, "preview")) {
        s_borrow_preview = true;
    } else if (!strcmp(who, "bridge")) {
        s_borrow_bridge = true;
    } else if (!strcmp(who, "sdr")) {
        s_borrow_sdr = true;
    } else {
        return;
    }
    if (s_mode == VOICE_OFF && voice_monitor_instant() == ESP_OK) {
        s_borrowed = true;
    }
}

static void monitor_release_locked(const char *who)
{
    if (!who) {
        return;
    }
    if (!strcmp(who, "preview")) {
        s_borrow_preview = false;
    } else if (!strcmp(who, "bridge")) {
        s_borrow_bridge = false;
    } else if (!strcmp(who, "sdr")) {
        s_borrow_sdr = false;
    } else {
        return;
    }
    if (s_borrowed && !s_borrow_preview && !s_borrow_bridge && !s_borrow_sdr
        && s_mode == VOICE_MONITOR && ptt_owner() == PTT_OWNER_NONE) {
        (void)voice_stop();
        s_borrowed = false;
    }
}

void voice_monitor_borrow(const char *who)
{
    control_lock(); monitor_borrow_locked(who); control_unlock();
}
void voice_monitor_release(const char *who)
{
    control_lock(); monitor_release_locked(who); control_unlock();
}

/* Reserve the browser TX and its DAC path together, before any other caller
 * can stop MONITOR. The SDR key lock may call this; audio tasks never do. */
bool voice_sdr_begin(uint32_t token)
{
    control_lock();
    const bool ok = s_mode == VOICE_MONITOR && ptt_claim(PTT_OWNER_SDR, token);
    if (ok) {
        voice_inject_reset();
        s_inject_owner = PTT_OWNER_SDR; s_inject_token = token;
        voice_bridge_set_silent(true);
        s_sdr_tail_active = false;
        s_sdr_tail_block_pending = false;
        s_sdr_tail_id = TAIL_OFF;
        s_sdr_tail_pos = 0;
        s_sdr_tail_token = token;
        s_sdr_tail_done_token = 0;
        s_sdr_net_tx = netcom_mode() != NETCOM_MODE_LOCAL;
        s_sdr_tail_config = tail_should_play(netcom_mode()) ? tail_get() : TAIL_OFF;
    }
    control_unlock();
    return ok;
}

void voice_monitor_operator_takeover(void)
{
    control_lock();
    if (ptt_owner() != PTT_OWNER_NONE) { control_unlock(); return; }
    s_borrow_preview = false;
    s_borrow_bridge = false;
    s_borrow_sdr = false;
    s_borrowed = false;
    control_unlock();
}

/*
 * Analog-bridge injection ring (net -> RF direction).
 *
 * While the monitor task owns the DAC, the network playback task pushes the
 * room audio here; the monitor task plays it through the same output chain,
 * so it reaches both the speaker and the SA818's MIC_IN on the keyed
 * transmitter.  256 ms of 8 kHz audio: the producer paces itself at the
 * sample clock (one 29 ms block at a time) and the consumer drains at the
 * same rate, so the ring only has to absorb brief I2S/ADC stalls.  It is a
 * static block in internal DRAM, which is scarce on this board (about 36 KB
 * free at runtime -- see voice_tail.c), so 2048 samples (4 KB) instead of a
 * full second (16 KB) which pushed lwip over the edge at boot.
 */
/* 512 ms at 8 kHz.  This ring is what absorbs a phone's WiFi power-save
 * stalls (100-300 ms) on the transmit path; 256 ms was not enough and the
 * monitor padded silent blocks (chopped audio). */
#define BRIDGE_INJ_SAMPLES 8192
static int16_t        s_bridge_inj[BRIDGE_INJ_SAMPLES];
static volatile size_t s_bridge_inj_head;   /* write index  */
static volatile size_t s_bridge_inj_tail;   /* read index   */
static portMUX_TYPE   s_bridge_inj_mux = portMUX_INITIALIZER_UNLOCKED;

static void inject_locked(const int16_t *pcm, size_t n, bool apply_bgtx)
{
    if (!pcm || n == 0) {
        return;
    }    if (n > BRIDGE_INJ_SAMPLES) {
        n = BRIDGE_INJ_SAMPLES;
    }
    const int g = s_bgtx;
    portENTER_CRITICAL(&s_bridge_inj_mux);
    const size_t free_n = BRIDGE_INJ_SAMPLES
                          - (s_bridge_inj_head - s_bridge_inj_tail);
    const size_t take = (n < free_n) ? n : free_n;
    for (size_t i = 0; i < take; ++i) {
        int v = pcm[i];
        if (apply_bgtx && g != 16) {
            v = (int)((float)v * (float)g / 16.0f);
            v = clamp16(v);
        }
        s_bridge_inj[s_bridge_inj_head % BRIDGE_INJ_SAMPLES] = (int16_t)v;
        s_bridge_inj_head++;
    }
    portEXIT_CRITICAL(&s_bridge_inj_mux);
}

static float s_inj_frac;

void voice_inject_reset(void)
{
    ptt_control_lock();
    portENTER_CRITICAL(&s_bridge_inj_mux);
    s_bridge_inj_tail = s_bridge_inj_head;
    s_inj_frac = 0; s_inj_primed = false;
    s_inject_owner = PTT_OWNER_NONE; s_inject_token = 0;
    s_inject_tx_processed = false;
    s_last_bridge_tx_processed = false;
    s_net_feeding = s_net_rf_required = s_net_rf_keyed = s_net_write_pending = false;
    s_net_ready_us = s_net_close_us = 0;
    portEXIT_CRITICAL(&s_bridge_inj_mux);
    ptt_control_unlock();
}

void voice_bridge_network_state(bool feeding, bool rf_required)
{
    ptt_control_lock();
    s_net_rf_required = rf_required;
    if (feeding) { s_net_close_us = 0; }
    else if (!s_net_close_us && s_inject_owner == PTT_OWNER_NETWORK) {
        s_net_close_us = esp_timer_get_time();
    }
    s_net_feeding = feeding;
    ptt_control_unlock();
}

void voice_bridge_network_keyed(bool keyed)
{
    ptt_control_lock();
    if (keyed && !s_net_rf_keyed) { s_net_ready_us = esp_timer_get_time() + NET_TX_LEAD_US; }
    if (!keyed && s_net_rf_keyed && s_inject_owner == PTT_OWNER_NETWORK &&
        !voice_bridge_level() && !s_net_write_pending) { voice_inject_reset(); }
    s_net_rf_keyed = keyed;
    ptt_control_unlock();
}

bool voice_bridge_network_pending(void)
{
    ptt_control_lock();
    bool pending = s_inject_owner == PTT_OWNER_NETWORK &&
                   (voice_bridge_level() != 0 || s_net_write_pending);
    /* A failed/stopped DAC cannot keep a completed network over keyed forever. */
    if (pending && !s_net_feeding && s_net_close_us &&
        esp_timer_get_time() - s_net_close_us > NET_DRAIN_LIMIT_US) {
        ESP_LOGW(TAG, "network PCM drain timed out; discarding stalled audio");
        voice_inject_reset(); pending = false;
    }
    ptt_control_unlock();
    return pending;
}

bool voice_bridge_network_ready(void)
{
    ptt_control_lock();
    const bool ready = !s_net_rf_required || (s_net_rf_keyed &&
        ptt_owned(PTT_OWNER_NETWORK, 1) && ptt_bridge_keyed() &&
        esp_timer_get_time() >= s_net_ready_us);
    ptt_control_unlock();
    return ready;
}

void voice_bridge_network_written(void)
{
    ptt_control_lock();
    s_net_write_pending = false;
    ptt_control_unlock();
}

void voice_bridge_network_cancel(void)
{
    ptt_control_lock();
    if (s_inject_owner == PTT_OWNER_NETWORK) { voice_inject_reset(); }
    ptt_control_unlock();
}

size_t voice_bridge_room(void)
{
    portENTER_CRITICAL(&s_bridge_inj_mux);
    const size_t room = BRIDGE_INJ_SAMPLES - (s_bridge_inj_head - s_bridge_inj_tail);
    portEXIT_CRITICAL(&s_bridge_inj_mux);
    return room;
}

void voice_bridge_inject(const int16_t *pcm, size_t n)
{
    ptt_control_lock();
    if (ptt_local_audio_active()) { ptt_control_unlock(); return; }
    const ptt_owner_t owner = ptt_owner();
    if (owner == PTT_OWNER_NONE || owner == PTT_OWNER_NETWORK) {
        if (s_inject_owner != PTT_OWNER_NETWORK) {
            const bool feeding = s_net_feeding, rf = s_net_rf_required, keyed = s_net_rf_keyed;
            const int64_t ready = s_net_ready_us, closing = s_net_close_us;
            voice_inject_reset();
            s_net_feeding = feeding; s_net_rf_required = rf; s_net_rf_keyed = keyed;
            s_net_ready_us = ready; s_net_close_us = closing;
        }
        s_inject_owner = PTT_OWNER_NETWORK; s_inject_token = 1;
        s_inject_tx_processed = false;
        s_inject_silent = false;
        inject_locked(pcm, n, true);
    }
    ptt_control_unlock();
}

void voice_sdr_inject(uint32_t token, const int16_t *pcm, size_t n)
{
    ptt_control_lock();
    if (ptt_owned(PTT_OWNER_SDR, token)) {
        if (s_inject_owner != PTT_OWNER_SDR || s_inject_token != token) { voice_inject_reset(); }
        s_inject_owner = PTT_OWNER_SDR; s_inject_token = token;
        s_inject_tx_processed = false;
        inject_locked(pcm, n, true);
    }
    ptt_control_unlock();
}

/* Browser PCM has already gone through the common MIC EQ and fixed TX gain.
 * Keep the legacy voice_sdr_inject() above raw for its existing callers and
 * tests; this entry point only skips the network->RF bgtx stage and marks the
 * ring data so the monitor does not run the MIC EQ a second time. */
void voice_sdr_inject_processed(uint32_t token, const int16_t *pcm, size_t n)
{
    ptt_control_lock();
    if (ptt_owned(PTT_OWNER_SDR, token)) {
        if (s_inject_owner != PTT_OWNER_SDR || s_inject_token != token) {
            voice_inject_reset();
        }
        s_inject_owner = PTT_OWNER_SDR; s_inject_token = token;
        s_inject_tx_processed = true;
        inject_locked(pcm, n, false);
    }
    ptt_control_unlock();
}

bool voice_sdr_tail_start(uint32_t token)
{
    ptt_control_lock();
    const bool ok = ptt_owned(PTT_OWNER_SDR, token) &&
                    s_inject_owner == PTT_OWNER_SDR &&
                    s_inject_token == token &&
                    s_sdr_tail_config > TAIL_OFF &&
                    s_sdr_tail_config < TAIL_COUNT &&
                    tail_samples(s_sdr_tail_config) > 0 &&
                    !s_sdr_tail_active;
    if (ok) {
        s_sdr_tail_id = s_sdr_tail_config;
        s_sdr_tail_pos = 0;
        s_sdr_tail_token = token;
        s_sdr_tail_block_pending = false;
        s_sdr_tail_done_token = 0;
        /* The monitor normally waits for a 600 ms cushion before draining the
         * browser ring.  Once release is requested, the remaining samples
         * must be consumed before the courtesy tone, even if the ring never
         * reached that normal prefill. */
        s_inj_primed = true;
        s_sdr_tail_active = true;
    }
    ptt_control_unlock();
    return ok;
}

bool voice_sdr_tail_configured(void)
{
    ptt_control_lock();
    const bool configured = s_sdr_tail_config > TAIL_OFF &&
                            s_sdr_tail_config < TAIL_COUNT &&
                            tail_samples(s_sdr_tail_config) > 0;
    ptt_control_unlock();
    return configured;
}

uint32_t voice_sdr_tail_ms(void)
{
    ptt_control_lock();
    uint32_t ms = 0;
    if (s_sdr_tail_config > TAIL_OFF && s_sdr_tail_config < TAIL_COUNT) {
        const size_t n = tail_samples(s_sdr_tail_config);
        ms = (uint32_t)(n * 1000u / AUDIO_SAMPLE_RATE_HZ);
    }
    ptt_control_unlock();
    return ms;
}

size_t voice_sdr_tail_fetch(int16_t *out, size_t n)
{
    if (!out || n == 0) {
        return 0;
    }
    ptt_control_lock();
    size_t got = 0;
    if (s_sdr_tail_active && !s_sdr_tail_block_pending &&
        s_sdr_tail_id > TAIL_OFF && s_sdr_tail_id < TAIL_COUNT) {
        const size_t total = tail_samples(s_sdr_tail_id);
        got = tail_fetch(s_sdr_tail_id, s_sdr_tail_pos, out, n);
        s_sdr_tail_pos += got;
        /* Browser tails are outgoing TX audio, so they use the same fixed
         * network TX gain as browser speech.  The receiver's bridge path
         * applies bgtx later only when it turns network audio back into RF. */
        (void)gain_mul(out, got, s_net_tx_gain);
        if (s_sdr_net_tx && got) {
            /* The monitor owns the tail clock.  Send exactly the samples it
             * just fetched; sdr.c sends browser PCM, so this cannot duplicate
             * audio.  Keep the call under the PTT lock so a timeout cannot
             * place the end marker ahead of this final tail block. */
            netcom_tx_sample(out, got, true);
        }
        if (!got || s_sdr_tail_pos >= total) {
            s_sdr_tail_block_pending = got != 0;
            if (!got) {
                s_sdr_tail_active = false;
                s_sdr_tail_done_token = s_sdr_tail_token;
            }
        }
    }
    ptt_control_unlock();
    return got;
}

bool voice_sdr_tail_active(void)
{
    ptt_control_lock();
    const bool active = s_sdr_tail_active || s_sdr_tail_block_pending;
    ptt_control_unlock();
    return active;
}

void voice_sdr_tail_stop(uint32_t token)
{
    ptt_control_lock();
    if (s_sdr_tail_token == token) {
        s_sdr_tail_active = false;
        s_sdr_tail_block_pending = false;
        s_sdr_tail_id = TAIL_OFF;
        s_sdr_tail_pos = 0;
        s_sdr_tail_token = 0;
        s_sdr_tail_done_token = 0;
        s_sdr_net_tx = false;
    }
    ptt_control_unlock();
}

void voice_sdr_tail_block_commit(void)
{
    ptt_control_lock();
    if (s_sdr_tail_block_pending) {
        s_sdr_tail_done_token = s_sdr_tail_token;
        s_sdr_tail_block_pending = false;
        s_sdr_tail_active = false;
    }
    ptt_control_unlock();
}

uint32_t voice_sdr_tail_done(void)
{
    ptt_control_lock();
    const uint32_t token = s_sdr_tail_done_token;
    s_sdr_tail_done_token = 0;
    ptt_control_unlock();
    return token;
}

void voice_sdr_end(uint32_t token)
{
    ptt_control_lock();
    if (s_inject_owner == PTT_OWNER_SDR && s_inject_token == token) {
        voice_inject_reset();
        s_inject_silent = false;
    }
    if (s_sdr_tail_token == token) {
        s_sdr_tail_active = false;
        s_sdr_tail_block_pending = false;
        s_sdr_tail_id = TAIL_OFF;
        s_sdr_tail_pos = 0;
        s_sdr_tail_token = 0;
        s_sdr_tail_done_token = 0;
        s_sdr_net_tx = false;
    }
    if (ptt_owned(PTT_OWNER_SDR, token)) {
        s_inject_silent = false;
        ptt_release(PTT_OWNER_SDR, token);
    }
    ptt_control_unlock();
}

/* Level of the injection ring (samples waiting for the monitor).  The browser
 * uses it as the error signal of a slow rate trim, so the microphone stream
 * arrives at the playback clock instead of drifting against it. */
size_t voice_bridge_level(void)
{
    portENTER_CRITICAL(&s_bridge_inj_mux);
    const size_t n = s_bridge_inj_head - s_bridge_inj_tail;
    portEXIT_CRITICAL(&s_bridge_inj_mux);
    return n;
}

/* Monitor blocks that found the injection ring empty while the transmitter was
 * keyed: every one of them is a silent hole in the transmitted audio. */
uint32_t voice_bridge_starve(void)
{
    return s_bridge_starve;
}

uint32_t voice_bridge_blocks(void)
{
    return s_bridge_blocks;
}

uint32_t voice_bridge_trim(void)
{
    return s_bridge_trim;
}

/*
 * Fractional read from the injection ring with a variable ratio.
 *
 * Playing at exactly the producer's rate leaves a real-time buffer sitting at
 * zero, so it dips empty once per block.  Playing at a fixed ratio drifts.
 * A slow ratio adaptation (the elastic-buffer idea, cf. esp_elastic_pcm_buffer
 * and the AudioWorklet players) keeps the level on target without the phase
 * jumps that dropping/duplicating single samples produces.
 */
/* s_inj_frac is reset together with the producer/session state above. */

size_t voice_bridge_take_resampled(int16_t *out, size_t n, float ratio)
{
    if (!out || n == 0) {
        return 0;
    }
    if (ratio < 0.98f) { ratio = 0.98f; }
    if (ratio > 1.02f) { ratio = 1.02f; }
    size_t produced = 0;
    portENTER_CRITICAL(&s_bridge_inj_mux);
    for (size_t i = 0; i < n; ++i) {
        if ((s_bridge_inj_head - s_bridge_inj_tail) < 2) {
            break;
        }
        const size_t i0 = s_bridge_inj_tail % BRIDGE_INJ_SAMPLES;
        const size_t i1 = (s_bridge_inj_tail + 1) % BRIDGE_INJ_SAMPLES;
        const float v = (float)s_bridge_inj[i0] * (1.0f - s_inj_frac) +
                        (float)s_bridge_inj[i1] * s_inj_frac;
        out[produced++] = (int16_t)v;
        s_inj_frac += ratio;
        while (s_inj_frac >= 1.0f && s_bridge_inj_head > s_bridge_inj_tail) {
            s_inj_frac -= 1.0f;
            ++s_bridge_inj_tail;
        }
    }
    if (s_bridge_inj_head == s_bridge_inj_tail) {
        s_inj_frac = 0.0f;
    }
    portEXIT_CRITICAL(&s_bridge_inj_mux);
    return produced;
}

size_t voice_bridge_play(int16_t *out, size_t n)
{
    ptt_control_lock();
    const bool browser = ptt_owner() == PTT_OWNER_SDR;
    const bool network = s_inject_owner == PTT_OWNER_NETWORK;
    s_last_bridge_tx_processed = false;
    const size_t lvl = voice_bridge_level();
    if (network && !voice_bridge_network_ready()) {
        ptt_control_unlock(); return 0;
    }
    if (network && lvl < 2) { s_inj_primed = false; }
    /* The last interpolation point is real audio, not disposable padding.
     * Drain it exactly once after input ends, then the next over must prefill. */
    if (network && !s_net_feeding && lvl > 0 && lvl < 2) {
        const size_t got = voice_bridge_take(out, n);
        s_net_write_pending = got != 0;
        ptt_control_unlock(); return got;
    }
    if (network && !s_net_feeding && lvl >= 2) { s_inj_primed = true; }
    if (!s_inj_primed && lvl >= 4800) {
        s_inj_primed = true;
    } else if (s_inj_primed && (browser ? lvl < 2 : lvl == 0)) {
        s_inj_primed = false;
        ++s_bridge_starve;
    }
    size_t got = 0;
    if (s_inj_primed) {
        const float ratio = 1.0f + ((float)lvl - 4800.0f) / 24000.0f;
        got = voice_bridge_take_resampled(out, n, ratio);
        if (got > 0) {
            s_last_bridge_tx_processed = browser && s_inject_tx_processed;
            if (network) { s_net_write_pending = true; }
        }
        if (lvl > 7400 || lvl < 3600) { ++s_bridge_trim; }
        if (got < n && browser) {
            s_inj_primed = false; ++s_bridge_starve;
        }
    }
    ptt_control_unlock();
    return got;
}

bool voice_bridge_last_injection_preprocessed(void)
{
    return s_last_bridge_tx_processed;
}

size_t voice_bridge_take(int16_t *out, size_t cap)
{
    if (!out || cap == 0) {
        return 0;
    }
    portENTER_CRITICAL(&s_bridge_inj_mux);
    size_t n = s_bridge_inj_head - s_bridge_inj_tail;
    if (n > cap) {
        n = cap;
    }
    for (size_t i = 0; i < n; ++i) {
        out[i] = s_bridge_inj[s_bridge_inj_tail % BRIDGE_INJ_SAMPLES];
        s_bridge_inj_tail++;
    }
    if (s_bridge_inj_head == s_bridge_inj_tail) {
        s_inj_frac = 0.0f;
    }
    portEXIT_CRITICAL(&s_bridge_inj_mux);
    return n;
}

/*
 * SDR: the web-audio TX feeds the same injection ring, but its audio must reach
 * the SA818's MIC_IN only.  Playing the browser's microphone on the board's own
 * speaker would close an acoustic loop through the browser microphone (browser
 * mic -> board -> local speaker -> air -> browser mic), so while this flag is
 * set the monitor task keeps the LM4871 shut during the injection.
 */
void voice_bridge_set_silent(bool silent)
{
    s_inject_silent = silent;
}

bool voice_dac_held_by_talk(void)
{
    return s_dac_talk;
}

voice_mode_t voice_mode(void) { return s_mode; }

const char *voice_mode_name_of(voice_mode_t mode)
{
    switch (mode) {
    case VOICE_OFF:     return "off";
    case VOICE_MONITOR: return "MONITOR (radio -> speaker)";
    case VOICE_TALK:
        /* Say which path the audio takes: the old hard-coded "radio, keyed" was
         * printed during network talks too, which made a silent network talk
         * look like an RF transmission. */
        return (netcom_mode() != NETCOM_MODE_LOCAL)
               ? "TALK (microphone -> network)"
               : "TALK (microphone -> radio, keyed)";
    }
    return "?";
}

const char *voice_mode_name(void) { return voice_mode_name_of(s_mode); }

void voice_get_stats(voice_stats_t *out)
{
    if (out) {
        *out = s_stats;
        out->gain = s_gain;
        out->mod_sq = s_mod_sq_open ? 1 : 0;
    }
}

esp_err_t voice_speaker_diag_command(const char *op, unsigned ms)
{
    control_lock();
    /* Same lock as TX admission and clock-mute control: readiness checks and
     * the local DAC reservation must be one operation across both cores. */
    ptt_control_lock();
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (op && !strcmp(op, "stop")) {
        err = speaker_diag_command(op, ms);
    } else if (s_mode == VOICE_MONITOR && !s_stop &&
               ptt_owner() == PTT_OWNER_NONE && ptt_get_state() == PTT_IDLE &&
               audio_i2s_clock_running() && !s_preview_amp && !tail_preview_active() &&
               !s_bridge_inj_amp && !voice_sdr_tail_active() && voice_bridge_level() == 0) {
        err = speaker_diag_command(op, ms);
    }
    ptt_control_unlock();
    control_unlock();
    return err;
}

void voice_set_gain(int gain)
{
    if (gain < 1)   { gain = 1; }
    if (gain > 255) { gain = 255; }
    s_gain = gain;
}

int voice_get_gain(void) { return s_gain; }

void voice_set_ab_gate(int period_ms)
{
    s_ab_gate_ms = (period_ms > 0) ? period_ms : 0;
}

int voice_get_ab_gate(void) { return s_ab_gate_ms; }

void voice_capture_begin(int16_t *buf, size_t cap)
{
    (void)buf;
    (void)cap;
}

size_t voice_capture_count(void) { return 0; }

size_t voice_capture_recent(int16_t *dst, size_t n)
{
    if (!dst || n == 0) {
        return 0;
    }
    if (!s_cap_ring) {
        return 0;
    }
    if (n > CAP_SAMPLES) {
        n = CAP_SAMPLES;
    }
    const size_t w = s_cap_w;
    /* Oldest first: start n samples behind the write pointer. */
    size_t idx = (w + CAP_SAMPLES - n) % CAP_SAMPLES;
    for (size_t i = 0; i < n; ++i) {
        dst[i] = s_cap_ring[idx];
        idx = (idx + 1) % CAP_SAMPLES;
    }
    return n;
}

void voice_set_quiet(bool quiet) { s_quiet = quiet; }

bool voice_take_level_line(char *out, size_t n)
{
    if (!s_level_pending || !out || n == 0) {
        return false;
    }
    snprintf(out, n, "%s", s_level_line);
    s_level_pending = false;
    return true;
}

void voice_set_amp_noise_test(bool on)
{
    s_amp_noise_test = on;
    if (on) {
        /* The noise-floor test needs the amplifier on no matter what. */
        board_amp_enable(true);
    }
}
bool voice_get_amp_noise_test(void) { return s_amp_noise_test; }

void voice_set_bypass_squelch(bool on)
{
    /*
     * Legacy flag: with the amplifier no longer gated by the software
     * squelch, "bypassing the squelch" is simply the normal monitoring
     * path.  Kept so the console and API keep working.
     */
    s_bypass_squelch = on;
}

bool voice_get_bypass_squelch(void) { return s_bypass_squelch; }

bool voice_squelch_open(void) { return s_sq_hold > 0; }

void voice_set_squelch_threshold(int counts)
{
    if (counts < 1)   { counts = 1; }
    if (counts > 2000) { counts = 2000; }
    s_sq_open_counts = counts;
}

int voice_get_squelch_threshold(void) { return s_sq_open_counts; }

/* AF_OUT bias now and the learned baseline: the pair that decides the
 * module squelch.  Exposed so a false trigger can be diagnosed remotely. */
void voice_set_sq_gpio(int gpio)
{
    /* 0 or out of range = disabled.  The NVS record keeps 0 so an explicit
     * "use the audio inference" choice survives a reboot, while a missing or
     * -1 record means "never configured" and leaves SQ_GPIO_DEFAULT in force. */
    if (gpio <= 0 || gpio > 48) {
        s_sq_gpio = -1;
    } else {
        s_sq_gpio = gpio;
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        (void)gpio_config(&io);
    }
    s_sq_gpio_run = 0;
    nvs_handle_t h;
    if (nvs_open("orl", NVS_READWRITE, &h) == ESP_OK) {
        const int32_t stored = (gpio > 0 && gpio <= 48) ? (int32_t)gpio : 0;
        if (nvs_set_i32(h, "sqgp", stored) == ESP_OK) {
            (void)nvs_commit(h);
        }
        nvs_close(h);
    }
}

int voice_get_sq_gpio(void) { return s_sq_gpio; }

bool voice_module_squelch_open(void) { return s_mod_sq_open; }

bool voice_rf_carrier_present(void)
{
    return s_mod_sq_open || (s_sq_gpio >= 0 && gpio_get_level((gpio_num_t)s_sq_gpio) == 0);
}

bool voice_network_busy(void)
{
    ptt_control_lock();
    const bool busy = s_net_feeding || s_net_write_pending ||
        (s_inject_owner == PTT_OWNER_NETWORK && voice_bridge_level() != 0);
    ptt_control_unlock();
    return busy;
}


int voice_get_bias(void) { return s_bias_raw; }

/* Pre-gain block peak since the last read: the number the squelch
 * thresholds are expressed in. */
int voice_take_block_peak(void)
{
    const int v = s_bpk_hold;
    s_bpk_hold = 0;
    return v;
}
int voice_get_bias_base(void) { return (int)s_bias_base; }

int voice_get_squelch_peak(void) { return s_stats.peak_in; }

/* ------------------------------------------------------- autostart setting */

#define NVS_NS_VOICE  "orl"
#define NVS_KEY_AUTO  "vauto"
#define NVS_KEY_TXG   "vtxg"
#define NVS_KEY_NTXG  "vntg"
#define NVS_KEY_MICG  "vmic"

static bool s_autostart = true;

void voice_set_autostart(bool on)
{
    s_autostart = on;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_VOICE, NVS_READWRITE, &h) == ESP_OK) {
        uint8_t v = on ? 1 : 0;
        if (nvs_set_blob(h, NVS_KEY_AUTO, &v, sizeof(v)) == ESP_OK) {
            (void)nvs_commit(h);
        }
        nvs_close(h);
    }
}

bool voice_get_autostart(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_VOICE, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        size_t len = sizeof(v);
        if (nvs_get_blob(h, NVS_KEY_AUTO, &v, &len) == ESP_OK) {
            s_autostart = (v != 0);
        }
        nvs_close(h);
    }
    return s_autostart;
}

/* ------------------------------------------------------ transmit (TX) gain */

/*
 * Network/phone transmit gain, fixed-point at 1/16 as well.
 *
 * Measured on 2026-11-05: with the shared TALK gain at 3.0x the phone stream
 * reached the SA818 well below the level the bridge path produces, so the far
 * end heard it "small"; raising the shared gain to 6.0x fixed the loudness but
 * made the handset capsule clip.  96 (6.0x) is the value the operator accepted
 * for the phone path, so it becomes this path's own default.
 */
static int s_net_tx_gain = 254;

void voice_set_net_tx_gain(int g)
{
    if (g < 1)   { g = 1; }
    if (g > 255) { g = 255; }
    s_net_tx_gain = g;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_VOICE, NVS_READWRITE, &h) == ESP_OK) {
        int32_t v = g;
        if (nvs_set_i32(h, NVS_KEY_NTXG, v) == ESP_OK) {
            (void)nvs_commit(h);
        }
        nvs_close(h);
    }
}

int voice_get_net_tx_gain(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_VOICE, NVS_READONLY, &h) == ESP_OK) {
        int32_t v = 0;
        if (nvs_get_i32(h, NVS_KEY_NTXG, &v) == ESP_OK && v >= 1 && v <= 255) {
            s_net_tx_gain = (int)v;
        }
        nvs_close(h);
    }
    return s_net_tx_gain;
}

/*
 * The TALK path's own gain, fixed-point at 1/16 so the range covers both
 * attenuation and boost.
 *
 * It used to be derived from the receive gain (`s_gain / 4`), which is simply
 * the wrong quantity: how hot the microphone is, and how much level the SA818's
 * MIC_IN wants, have nothing to do with how loud the speaker should be.  With
 * the receive gain at 80 that formula applied 20x to a microphone signal whose
 * peaks measure about 12000 counts, i.e. 240000 against a 16-bit ceiling of
 * 32767 -- the transmission was hard-clipped by construction.
 *
 * Measured microphone peaks at the vendor's own codec defaults (30 dB PGA,
 * ADC volume 0xC8) are 920-12837 counts with no clipping, so a factor near 2
 * puts the peaks just under full scale.  32/16 = 2.0 is the default.
 */
static int s_tx_gain = 36;

/* Handset microphone PGA (ES8311 REG16, 0/6/.../42 dB).
 *
 * It used to live only in the codec, so every reboot silently returned to the
 * vendor default and any tuning was lost -- which is exactly what made the
 * capsule clip again after a power cycle.  Persist it like the other gains. */
void voice_set_mic_gain_index(int idx)
{
    if (idx < 0) { idx = 0; }
    if (idx > 7) { idx = 7; }
    if (audio_codec_set_mic_gain(idx) != ESP_OK) { return; }
    nvs_handle_t h;
    if (nvs_open(NVS_NS_VOICE, NVS_READWRITE, &h) == ESP_OK) {
        uint8_t v = (uint8_t)idx;
        if (nvs_set_blob(h, NVS_KEY_MICG, &v, sizeof(v)) == ESP_OK) {
            (void)nvs_commit(h);
        }
        nvs_close(h);
    }
}

void voice_mic_gain_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_VOICE, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        size_t len = sizeof(v);
        if (nvs_get_blob(h, NVS_KEY_MICG, &v, &len) == ESP_OK && v <= 7) {
            (void)audio_codec_set_mic_gain((int)v);
        }
        nvs_close(h);
    }
}

void voice_set_tx_gain(int g)
{
    if (g < 1)   { g = 1; }
    if (g > 255) { g = 255; }
    s_tx_gain = g;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_VOICE, NVS_READWRITE, &h) == ESP_OK) {
        int32_t v = g;
        if (nvs_set_i32(h, NVS_KEY_TXG, v) == ESP_OK) {
            (void)nvs_commit(h);
        }
        nvs_close(h);
    }
}

void voice_set_net_rx_gain(int g) {
    if (g < 0) { g = 0; }
    if (g > 255) { g = 255; }
    s_net_rx_gain = g;
    nvs_handle_t h;
    if (nvs_open("orl", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_i32(h, "rxnetgain", g) == ESP_OK) { (void)nvs_commit(h); }
        nvs_close(h);
    }
}
int voice_get_net_rx_gain(void) { return s_net_rx_gain; }

void voice_set_tx_preemph(bool on) {
    s_tx_preemph = on;
    nvs_handle_t h;
    if (nvs_open("orl", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u8(h, "txpre", on ? 1 : 0) == ESP_OK) { (void)nvs_commit(h); }
        nvs_close(h);
    }
}
bool voice_get_tx_preemph(void) { return s_tx_preemph; }

int voice_get_tx_gain(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_VOICE, NVS_READONLY, &h) == ESP_OK) {
        int32_t v = 0;
        if (nvs_get_i32(h, NVS_KEY_TXG, &v) == ESP_OK && v >= 1 && v <= 255) {
            s_tx_gain = (int)v;
        }
        int32_t n = 0;
        if (nvs_get_i32(h, "rxnetgain", &n) == ESP_OK && n >= 0 && n <= 255) {
            s_net_rx_gain = (int)n;
        }
        uint8_t pre = 1;
        if (nvs_get_u8(h, "txpre", &pre) == ESP_OK) {
            s_tx_preemph = pre != 0;
        }
        nvs_close(h);
    }
    return s_tx_gain;
}

/* -------------------------------------------------------------------- EQ */

/*
 * One chain per direction.  The speaker chain runs at the end of the MONITOR
 * path (after gain, before the DAC); the microphone chain runs at the start
 * of the TALK path (before the TX gain and limiter).  Both are 8 kHz.
 * (s_eq_spk / s_eq_mic are declared up top with the other audio state.)
 */

#define NVS_KEY_EQ   "veq"

/* Factory transmit curve (150/400/900/2k/3.2k/3.8k Hz).  The receive chain still
 * starts flat; the web panel's "全部归 0" zeroes both chains. */
static const int EQ_MIC_DEFAULT[EQ6_BANDS] = { 0, 0, 0, 0, 0, 0 };

typedef struct {
    int8_t db[2][EQ6_BANDS];   /* [0] = speaker, [1] = microphone */
} eq_nvs_blob_t;

static eq6_t *eq_chain(int dir)
{
    return dir ? &s_eq_mic : &s_eq_spk;
}

static void voice_eq_save(void)
{
    eq_nvs_blob_t blob;
    for (int d = 0; d < 2; ++d) {
        for (int b = 0; b < EQ6_BANDS; ++b) {
            blob.db[d][b] = (int8_t)eq_chain(d)->db[b];
        }
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS_VOICE, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_blob(h, NVS_KEY_EQ, &blob, sizeof(blob)) == ESP_OK) {
            (void)nvs_commit(h);
        }
        nvs_close(h);
    }
}

void voice_eq_load(void)
{
    /*
     * The 3-band build stored a 6-byte value under the same key.  If that is
     * what comes back, carry its three bands over as bands 0-2 (150 Hz shelf,
     * 400 Hz, 900 Hz) and start the top of the range flat; anything else is
     * treated as empty.
     */
    eq_nvs_blob_t blob;
    bool have = false;
    memset(&blob, 0, sizeof(blob));
    nvs_handle_t h;
    if (nvs_open(NVS_NS_VOICE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(blob);
        if (nvs_get_blob(h, NVS_KEY_EQ, &blob, &len) == ESP_OK) {
            have = true;
            if (len == 6) {
                for (int d = 0; d < 2; ++d) {
                    const int8_t old[3] = { blob.db[d][0], blob.db[d][1], blob.db[d][2] };
                    memset(blob.db[d], 0, EQ6_BANDS);
                    memcpy(blob.db[d], old, 3);
                }
            } else if (len != sizeof(blob)) {
                memset(&blob, 0, sizeof(blob));
            }
        } else {
            memset(&blob, 0, sizeof(blob));
        }
        nvs_close(h);
    }
    for (int d = 0; d < 2; ++d) {
        bool bad = false;
        for (int b = 0; b < EQ6_BANDS; ++b) {
            if (blob.db[d][b] < EQ6_MIN_DB || blob.db[d][b] > EQ6_MAX_DB) {
                bad = true;
                break;
            }
        }
        if (bad) { have = false; memset(blob.db[d], 0, sizeof(blob.db[d])); }
        int db6[EQ6_BANDS];
        for (int b = 0; b < EQ6_BANDS; ++b) {
            db6[b] = blob.db[d][b];
        }
        /* No stored curve on a fresh board: transmit starts on the factory curve,
         * receive stays flat. */
        if (!have && d == 1) {
            for (int b = 0; b < EQ6_BANDS; ++b) { db6[b] = EQ_MIC_DEFAULT[b]; }
        }
        eq6_set(eq_chain(d), db6);
    }
    ESP_LOGI(TAG, "EQ loaded: spk %d/%d/%d/%d/%d/%d dB, mic %d/%d/%d/%d/%d/%d dB",
             blob.db[0][0], blob.db[0][1], blob.db[0][2],
             blob.db[0][3], blob.db[0][4], blob.db[0][5],
             blob.db[1][0], blob.db[1][1], blob.db[1][2],
             blob.db[1][3], blob.db[1][4], blob.db[1][5]);
}

void voice_eq_set(int dir, const int db[EQ6_BANDS])
{
    if (dir < 0 || dir > 1) {
        return;
    }
    eq6_set(eq_chain(dir), db);
    voice_eq_save();
    ESP_LOGI(TAG, "EQ set %s: %d/%d/%d/%d/%d/%d dB (persisted)",
             dir ? "mic" : "spk",
             db[0], db[1], db[2], db[3], db[4], db[5]);
}

void voice_eq_get(int dir, int db[EQ6_BANDS])
{
    const eq6_t *e = eq_chain(dir ? 1 : 0);
    memcpy(db, e->db, EQ6_BANDS * sizeof(int));
}

void voice_eq_reset(void)
{
    const int zero[EQ6_BANDS] = { 0 };
    voice_eq_set(0, zero);
    voice_eq_set(1, zero);
}

/*
 * Codec DAC volume (kept as a bench control, NOT the operator volume).
 *
 * See the long note in voice.h: the ES8311's single DAC output is wired to both
 * the speaker amplifier and the radio's transmit input, so this register is a
 * *system* level, not a speaker level.  The operator's volume is the five-step
 * speaker control below, which never touches it; this stays where the board was
 * tuned (60) and is reachable only from the console/API for bench work.
 */
#define NVS_KEY_VOL  "vvol"

static int s_output_volume = 60;

void voice_set_output_volume(int vol)
{
    if (vol < 0)   { vol = 0; }
    if (vol > 100) { vol = 100; }
    if (audio_codec_set_volume(vol) != ESP_OK) {
        ESP_LOGW(TAG, "output volume %d: codec refused it", vol);
        return;
    }
    s_output_volume = vol;

    nvs_handle_t h;
    if (nvs_open(NVS_NS_VOICE, NVS_READWRITE, &h) == ESP_OK) {
        int32_t v = vol;
        if (nvs_set_i32(h, NVS_KEY_VOL, v) == ESP_OK) {
            (void)nvs_commit(h);
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "codec DAC volume -> %d/100 (system level, not the "
                  "speaker volume)", vol);
}

int voice_get_output_volume(void)
{
    return s_output_volume;
}

void voice_output_volume_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_VOICE, NVS_READONLY, &h) == ESP_OK) {
        int32_t v = 0;
        if (nvs_get_i32(h, NVS_KEY_VOL, &v) == ESP_OK && v >= 0 && v <= 100) {
            s_output_volume = (int)v;
        }
        nvs_close(h);
    }
    (void)audio_codec_set_volume(s_output_volume);
}

/* ------------------------------------------------- five-step speaker volume */

/*
 * See voice.h for why this is an attenuation on the speaker-bound audio rather
 * than the codec's DAC volume: the DAC output is shared with the SA818's
 * transmit input, so moving that register would move our transmitted audio too.
 *
 * The step numbers (40..70) are the codec DAC volume values: step N must sound
 * exactly like "DAC output volume = N" used to, so the operator's ear keeps
 * its calibration from the old 0-100 volume slider.  The multipliers are that
 * loudness difference relative to the 70 step (unity), computed from the
 * ES8311's DAC volume register curve -- 0.5 dB per register step, 0xBF = 0 dB
 * (datasheet) -- and the driver's linear 0-100 -> register mapping
 * (es8311_voice_volume_set: reg = v*256/100 - 1):
 *
 *   step    40    45    50    55     60    65    70
 *   reg    101   114   127   139    152   165   178
 *   codec  -45  -38.5  -32  -26  -19.5  -13  -6.5   (dB)
 *   step   -38.5 -32 -25.5 -19.5  -13  -6.5   0     (dB vs the 70 step)
 */
#define NVS_KEY_SPK  "spk"
#define NVS_KEY_SPKV "spkv"     /* step-numbering stamp, see load below */

static const float SPK_MUL[VOICE_SPK_MAX + 1] = {
    1.0000f,      /* 0: amplifier shut down, level does not matter */
    0.01188f,     /* 1: 40  -38.5 dB */
    0.02512f,     /* 2: 45  -32   dB */
    0.05309f,     /* 3: 50  -25.5 dB */
    0.10593f,     /* 4: 55  -19.5 dB */
    0.22387f,     /* 5: 60  -13   dB */
    0.47315f,     /* 6: 65  -6.5  dB */
    1.0000f,      /* 7: 70   0    dB  (unity) */
};

/*
 * Step-numbering stamp for one-time migrations:
 *   1 (absent): five-step scale, 0 = off, 1..4 = 40/50/60/70 (60-centered dB)
 *   2:         eight-step scale, 0 = off, 1..7 = 40..70 (60-centered dB)
 *   3:         eight-step scale calibrated to the codec register curve
 *              (70-centered, unity at the codec's own volume)
 */
#define SPK_NUMBERING 3

static int s_speaker_level = VOICE_SPK_REF;

void voice_set_speaker_level(int level)
{
    if (level < VOICE_SPK_MIN) { level = VOICE_SPK_MIN; }
    if (level > VOICE_SPK_MAX) { level = VOICE_SPK_MAX; }
    s_speaker_level = level;

    /* Step 0 is the amplifier, not the signal: shut it down and hold it there
     * (the audio gates keep asking for it while audio plays). */
    board_amp_mute_set(level == 0);

    nvs_handle_t h;
    if (nvs_open(NVS_NS_VOICE, NVS_READWRITE, &h) == ESP_OK) {
        int32_t v = level;
        if (nvs_set_i32(h, NVS_KEY_SPK, v) == ESP_OK) {
            (void)nvs_set_i32(h, NVS_KEY_SPKV, SPK_NUMBERING);
            (void)nvs_commit(h);
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "speaker level -> step %d/%d (%s)", level, VOICE_SPK_MAX,
             level == 0 ? "amplifier off" : "attenuation step");
}

int voice_get_speaker_level(void)
{
    return s_speaker_level;
}

void voice_speaker_level_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_VOICE, NVS_READONLY, &h) == ESP_OK) {
        int32_t v = VOICE_SPK_REF;
        int32_t stamp = 0;
        const bool stamped =
            (nvs_get_i32(h, NVS_KEY_SPKV, &stamp) == ESP_OK &&
             stamp >= SPK_NUMBERING);
        if (nvs_get_i32(h, NVS_KEY_SPK, &v) == ESP_OK && v >= 0) {
            if (!stamped) {
                /*
                 * Older numbering: re-map at the nearest loudness so the
                 * speaker stays where the operator left it.
                 */
                static const int from_stamp2[8] = { 0, 5, 6, 6, 7, 7, 7, 7 };
                static const int from_stamp1[5] = { 0, 5, 6, 7, 7 };
                const int *old_to_new;
                int        old_max;
                if (stamp == 2) {
                    old_to_new = from_stamp2;   /* 8-step, 60-centered dB */
                    old_max    = 8;
                } else {
                    old_to_new = from_stamp1;   /* 5-step, 60-centered dB */
                    old_max    = 5;
                }
                const int nv = (v < old_max) ? old_to_new[v] : VOICE_SPK_REF;
                ESP_LOGW(TAG, "speaker step %d uses the old scale -> %d",
                         (int)v, nv);
                v = nv;
            }
            if (v >= VOICE_SPK_MIN && v <= VOICE_SPK_MAX) {
                s_speaker_level = (int)v;
            }
        }
        nvs_close(h);
    }
    board_amp_mute_set(s_speaker_level == 0);
    ESP_LOGI(TAG, "speaker level restored: step %d/%d", s_speaker_level,
             VOICE_SPK_MAX);
}

bool voice_speaker_muted(void)
{
    return s_speaker_level == 0;
}

void voice_speaker_apply(int16_t *pcm, size_t n)
{
    if (!pcm || n == 0 || s_speaker_level == VOICE_SPK_REF) {
        return;             /* the 70 step is unity: leave the samples untouched */
    }
    const float mul = SPK_MUL[s_speaker_level];
    for (size_t i = 0; i < n; ++i) {
        int v = (int)((float)pcm[i] * mul);
        /* Plain saturation: the speaker boost must not wrap the int16 range. */
        v = clamp16(v);
        pcm[i] = (int16_t)v;
    }
}

/*
 * Factory defaults for the whole audio chain.
 *
 * The values are exactly the file-scope initialisers above (s_gain 80, s_tx_gain
 * 48 = 3.0x, SQ_OPEN_COUNTS_DEF 10, fixed gain, speaker step 7, codec DAC volume
 * 60), so a factory reset and a fresh boot land in the same place.  Everything
 * that has an NVS shadow (TX gain, EQ, autostart, speaker level, DAC volume) is
 * written through its own setter so the reset survives a reboot.
 */
void voice_restore_defaults(void)
{
    voice_set_gain(96);
    voice_set_tx_gain(36);
    voice_set_net_tx_gain(254);
    voice_set_net_rx_gain(16);
    voice_set_tx_preemph(true);
    voice_set_squelch_threshold(SQ_OPEN_COUNTS_DEF);
    voice_set_autostart(true);
    voice_set_ab_gate(0);
    voice_set_quiet(false);
    voice_set_amp_noise_test(false);
    voice_set_bypass_squelch(false);
    const int flat[EQ6_BANDS] = { 0 };
    voice_eq_set(0, flat);
    voice_eq_set(1, EQ_MIC_DEFAULT);
    voice_set_speaker_level(VOICE_SPK_REF);

    /* Codec-side defaults (ES8311 DAC volume, mic PGA, ADC volume). */
    voice_set_output_volume(60);
    voice_set_mic_gain_index(4);             /* 24 dB: 30 dB clipped the capsule */
    (void)audio_codec_set_adc_volume(0xC8);

    ESP_LOGW(TAG, "audio defaults restored: gain 80, txgain 3.0x, "
                  "ntxgain 6.0x, AGC off, LED threshold %d, EQ flat, "
                  "speaker step %d, DAC volume 60, mic 24 dB",
             SQ_OPEN_COUNTS_DEF, VOICE_SPK_REF);
}

/* A generation prevents an expiring TALK task from restarting MONITOR after
 * another caller has already stopped it or started a replacement session. */
esp_err_t voice_stop(void)
{
    control_lock();
    if (ptt_owner() == PTT_OWNER_SDR || ptt_owner() == PTT_OWNER_NETWORK) {
        control_unlock(); return ESP_ERR_INVALID_STATE;
    }
    ++s_control_generation;
    const esp_err_t err = voice_stop_locked();
    control_unlock();
    return err;
}

esp_err_t voice_start(voice_mode_t mode)
{
    control_lock();
    /* Check readiness and reserve the transmitter before changing the DAC.
     * A refused press must leave the existing receiver/transmission intact. */
    const uint32_t token = s_control_generation == UINT32_MAX ? 1 : s_control_generation + 1;
    const bool talk = mode == VOICE_TALK;
    if ((talk && ((netcom_mode() != NETCOM_MODE_LOCAL && !netcom_tx_ready()) ||
                  ((netcom_mode() == NETCOM_MODE_LOCAL || netcom_rf_tx()) && !sa818_cfg_tx_ready()) ||
                  !ptt_claim(PTT_OWNER_TALK, token))) ||
        (!talk && ptt_owner() != PTT_OWNER_NONE)) {
        control_unlock(); return ESP_ERR_INVALID_STATE;
    }
    s_control_generation = token;
    if (talk) { voice_inject_reset(); }
    const esp_err_t err = voice_start_locked(mode);
    if (talk && err != ESP_OK) {
        ptt_release(PTT_OWNER_TALK, token);
        if (s_mode == VOICE_OFF) { (void)voice_start_locked(VOICE_MONITOR); }
    }
    control_unlock();
    return err;
}

/* ---------------------------------------------------- network courtesy tone */

/*
 * The network paths (bridge, P2P, relay) end an over without playing the tone,
 * because they release the carrier as soon as the last packet is out.  They now
 * request it here; the monitor task -- which owns the DAC while the bridge is
 * keyed -- plays it through the same tail_play() loop the local TALK path uses
 * and releases the carrier afterwards.  Requesting is cheap and cannot fail, so
 * there is no start/refuse race left.
 */
static volatile bool s_bridge_tail_req;
static volatile bool s_bridge_tail_run;   /* kept true while the tone plays */
static int s_bridge_tail_id;

bool voice_net_tail_start(void)
{
    /*
     * Plan A (2026-11-06): the courtesy tone is generated once, at the end that
     * is doing the talking (handset mic, browser, app).  A relay must not add
     * a second one on top of the tone already inside the stream.
     */
    return false;
    const int id = tail_get();
    if (!tail_should_play(netcom_mode()) || id <= TAIL_OFF ||
        id >= TAIL_COUNT || tail_samples(id) == 0) {
        return false;
    }
    s_bridge_tail_id = id;
    s_bridge_tail_req = true;
    return true;
}

bool voice_net_tail_active(void) { return s_bridge_tail_req || s_bridge_tail_run; }

void voice_net_tail_stop(void) { s_bridge_tail_req = false; s_bridge_tail_run = false; }

static void bridge_tail_sink(int16_t *b, size_t n, void *ctx)
{
    (void)ctx;
    (void)gain_mul(b, n, s_bgtx);
    (void)audio_i2s_write(b, n);
}

/* Runs in the monitor task: writes the tone straight to the DAC while the
 * bridge keeps the radio keyed, then releases the carrier. */
void voice_net_tail_run(size_t frames)
{
    if (!s_bridge_tail_req) { return; }
    s_bridge_tail_req = false;
    s_bridge_tail_run = true;      /* the network task must keep the carrier up */
    int16_t buf[BLOCK_FRAMES];
    const size_t block = frames < BLOCK_FRAMES ? frames : BLOCK_FRAMES;
    (void)tail_play(s_bridge_tail_id, buf, block, bridge_tail_sink, NULL);
    ptt_bridge_key(false);         /* only now may the carrier drop */
    s_bridge_tail_run = false;
}
