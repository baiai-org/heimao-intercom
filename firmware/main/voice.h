/*
 * Local voice paths -- OpenRadioLink-v0.3.
 *
 * Two real-time audio routes, both of which the network stage will reuse by
 * swapping one end for the codec:
 *
 *   MONITOR  SA818 AF_OUT (GPIO1, ADC1_CH0) -> speaker (ES8311 DAC -> LM4871)
 *            "listen to the radio on the board's speaker".  Receive only; PTT
 *            stays released.
 *
 *   TALK     microphone (ES8311 ADC) -> I2S loopback -> ES8311 DAC -> the
 *            SA818's MIC_IN divider, with the transmitter keyed.
 *            The local LM4871 is kept OFF here: it hangs off the same DAC node,
 *            so leaving it on would put the operator's own voice back into the
 *            room and straight into the microphone.
 *
 * Both routes must produce exactly 8 kHz of samples.  That is why MONITOR uses
 * the ADC continuous (DMA) driver rather than a polled oneshot loop: a jittery
 * sample clock shows up as warbling audio.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_OFF = 0,
    VOICE_MONITOR,      /* radio -> speaker            */
    VOICE_TALK          /* microphone -> radio (keyed) */
} voice_mode_t;

/*
 * Start a route.  Safe to call when another route is running (it is stopped
 * first).  VOICE_TALK keys the transmitter, so it must only be reached through
 * the console's two-step confirmation.
 */
esp_err_t voice_start(voice_mode_t mode);

/* Stop everything: release PTT, silence the amplifier, stop the streams. */
esp_err_t voice_stop(void);

/*
 * End a running TALK session without releasing the PTT immediately: the talk
 * task plays the end-of-transmission tail tone with the transmitter still
 * keyed and releases the PTT only after the tone is complete.  The panel's
 * PTT release and the web talk timer use this so the tail actually goes to
 * the air.  Safe from any context (it sets a flag); a request with no talk
 * running is consumed by the next voice_start().
 */
void voice_request_talk_end(void);
uint32_t voice_talk_token(void);
bool voice_request_talk_end_for(uint32_t token);

/*
 * Monitor mode without the post-TX settle fade: for playing a tail preview
 * when no voice path is running (the 500 ms ramp would swallow short tones).
 */
esp_err_t voice_monitor_instant(void);

/*
 * Monitor-task borrowing (tail preview and the analog bridge): start the
 * monitor when nothing owns the DAC, remember who borrowed it, and stop it
 * again when the last borrower is done -- unless the operator took over in
 * the meantime.  `who` is "preview" or "bridge"; two borrowers can coexist.
 */
void voice_monitor_borrow(const char *who);
void voice_monitor_release(const char *who);

/* The operator used the explicit monitor on/off: clear every borrow. */
void voice_monitor_operator_takeover(void);

/*
 * Analog-bridge injection (net -> RF direction): while the monitor task owns
 * the DAC, a network engine's playback task pushes room audio here; the
 * monitor task plays it through the same output chain, where it reaches the
 * SA818's MIC_IN on the keyed transmitter.  `voice_bridge_take()` returns
 * the number of samples consumed (0 = nothing pending).
 */
/* Browser injection is admitted only for this transmission token. */
bool voice_sdr_begin(uint32_t token);
void voice_sdr_inject(uint32_t token, const int16_t *pcm, size_t n);
/* Browser PCM after the common MIC EQ + fixed TX gain.  The monitor skips its
 * second MIC EQ pass and does not apply network->RF bgtx to this data. */
void voice_sdr_inject_processed(uint32_t token, const int16_t *pcm, size_t n);
void voice_sdr_end(uint32_t token);
/* Keep the SDR owner keyed while the monitor emits a fixed tail tone. */
bool voice_sdr_tail_configured(void);
uint32_t voice_sdr_tail_ms(void);
bool voice_sdr_tail_start(uint32_t token);
void voice_sdr_tail_stop(uint32_t token);

/* Courtesy tone for a network over that keys the radio (bridge / P2P / relay):
 * called when the stream ends, keeps the carrier up until the tone is done. */
bool   voice_net_tail_start(void);
bool   voice_net_tail_active(void);
void   voice_net_tail_stop(void);
void   voice_net_tail_run(size_t frames);   /* monitor task only */
size_t voice_sdr_tail_fetch(int16_t *out, size_t n);
bool voice_sdr_tail_active(void);
/* Called by the monitor after the I2S write containing the final tail block. */
void voice_sdr_tail_block_commit(void);
/* Returns and clears a tail completion token after the block reached I2S. */
uint32_t voice_sdr_tail_done(void);
void voice_inject_reset(void);
size_t voice_bridge_play(int16_t *out, size_t n);
bool voice_bridge_last_injection_preprocessed(void);
void   voice_bridge_inject(const int16_t *pcm, size_t n);
size_t voice_bridge_take(int16_t *out, size_t cap);
/* Fractional read with a slow ratio adaptation (elastic playback). */
size_t voice_bridge_take_resampled(int16_t *out, size_t n, float ratio);
/* Samples waiting in the injection ring (clock-recovery error signal). */
size_t voice_bridge_level(void);
/* Blocks the monitor had to fill with silence while keyed. */
uint32_t voice_bridge_starve(void);
/* Monitor blocks played through the injection path (rate measurement). */
uint32_t voice_bridge_blocks(void);
/* Rate-trim actions taken (should stay ~0 in steady state). */
uint32_t voice_bridge_trim(void);

/*
 * Silent injection (SDR mode): the injected samples still reach the SA818's
 * MIC_IN while the bridge key is asserted, but the local LM4871 is kept shut,
 * so the browser's microphone never echoes out of the board's own speaker.
 */
void voice_bridge_set_silent(bool silent);

/*
 * True while the talk task owns the codec DAC.
 *
 * Network modes normally leave the DAC to the engine's playback loop (remote
 * audio or silence).  When "network + local RF" is selected, the talk task has
 * to feed the DAC itself so the SA818 has something to modulate -- and two
 * blocking writers would interleave blocks and wreck the sample clock.  The
 * engines check this flag and skip their write (keeping their cadence) while
 * it is set.
 */
bool voice_dac_held_by_talk(void);

/* Peak meters, cleared on read: the level the receive chain produces and the
 * level that leaves for the SA818's MIC_IN. */
/* Hardware COS: SA818 pin 1 (Audio ON), active low.  Defaults to IO40 on
 * the field boards (1 kΩ link, docs/HANDOVER-SUMMARY.md); 0 disables it and
 * falls back to the AF_OUT inference. */
void voice_set_sq_gpio(int gpio);
int voice_get_sq_gpio(void);
bool voice_module_squelch_open(void);

/* RF -> network level (1/16x) and the transmit pre-emphasis switch. */
void voice_set_net_rx_gain(int g);
int  voice_get_net_rx_gain(void);
void voice_set_tx_preemph(bool on);
bool voice_get_tx_preemph(void);

int voice_get_bias(void);
int voice_take_block_peak(void);
int voice_get_bias_base(void);
int voice_take_meter_in(void);

/* Forward gain for audio that arrives from the network and is re-transmitted:
 * 16 = unity, 48 = 3x.  See the note in voice.c. */
void voice_set_bgtx(int v);
int voice_get_bgtx(void);
int voice_take_meter_out(void);

voice_mode_t voice_mode(void);
const char  *voice_mode_name(void);
/* Name any mode value, not just the current one. */
const char  *voice_mode_name_of(voice_mode_t mode);

/* Live counters for the status display. */
typedef struct {
    unsigned blocks;        /* audio blocks processed        */
    unsigned samples;       /* ADC samples actually consumed (rate probe) */
    unsigned underruns;     /* ADC reads that timed out      */
    int      peak_in;       /* input AC peak, last ~1 s, raw counts */
    int      peak_max;      /* input AC peak since start           */
    int      sample_rate;   /* nominal                       */
    int      gain;          /* current software gain         */
    int      mod_sq;        /* SA818 RF squelch open (AF_OUT bias) */
} voice_stats_t;

void voice_get_stats(voice_stats_t *out);

/* Volatile speaker diagnosis; requires MONITOR, never keys PTT. */
esp_err_t voice_speaker_diag_command(const char *op, unsigned ms);

/* Software gain on the MONITOR path, 1..255. */
void voice_set_gain(int gain);
int  voice_get_gain(void);

/*
 * Audible self-test: gate the monitor output on/off every `period_ms` while
 * leaving the input untouched.  0 disables it.
 *
 * This exists because an A/B test that depends on the operator and the console
 * being synchronised is a test that gets missed (it did, the first time).  A
 * slow, repeating on/off pattern can be listened to at any moment and proves
 * in one go that the sound coming out of the speaker really is produced by this
 * pipeline rather than picked up somewhere else.
 */
void voice_set_ab_gate(int period_ms);
int  voice_get_ab_gate(void);

/*
 * Recent-audio ring.
 *
 * The monitor path always keeps the last few seconds of filtered audio in a
 * ring, and this copies the most recent `n` samples out (oldest first, zero
 * filled if not enough history yet).
 *
 * A ring rather than a triggered capture because a triggered capture has to be
 * synchronised with the operator, and that coordination kept failing: by the
 * time the command is typed, the sentence is over.  With a ring, the recording
 * is always already there and can be pulled afterwards.
 */
size_t voice_capture_recent(int16_t *dst, size_t n);

/*
 * Suppress the periodic level log.  The hex audio dump travels over the same
 * console, and an interleaved log line in the middle of it would corrupt the
 * capture.
 */
void voice_set_quiet(bool quiet);
/* Copies the pending level line (formatted by the audio task) and clears it.
 * Printing happens in the caller, never in the audio task. */
bool voice_take_level_line(char *out, size_t n);

/*
 * Output-chain noise-floor test.
 *
 * Forces the amplifier ON while feeding the DAC digital silence and bypassing
 * the squelch.  Anything audible then is produced by the ES8311, the LM4871 or
 * the supply -- i.e. by the output stage itself, not by the radio audio.  This
 * separates "the signal is too quiet for the noise" from "the noise is on the
 * audio path at all".
 */
void voice_set_amp_noise_test(bool on);
bool voice_get_amp_noise_test(void);

/*
 * Pass the monitor audio straight through, holding the amplifier on and
 * bypassing the software squelch entirely.
 *
 * This is the condition that actually matches "listening to the radio": the
 * codec keeps its I2S clocks, valid samples keep flowing, and nothing gates the
 * output.  Output noise and output signal can then be compared on a scope under
 * representative conditions, which is what the earlier amp tests failed to do --
 * stopping the I2S clocks (the old amp-alone test) drives the codec into an
 * undefined state that saturates the amplifier, so it measured a fault it had
 * itself created.
 */
void voice_set_bypass_squelch(bool on);
bool voice_get_bypass_squelch(void);

/* True while the input-level indicator is lit (audio present on the LED).
 * Drives the status LED; the amplifier itself follows the SA818's RF
 * squelch and is no longer gated by this. */
bool voice_squelch_open(void);

/* LED indicator threshold in raw band-passed counts.  See the note in voice.c. */
void voice_set_squelch_threshold(int counts);
int  voice_get_squelch_threshold(void);
int  voice_get_squelch_peak(void);

/* Diagnostic count of samples outside the int16 representation boundary. */
uint32_t voice_hard_clip_total(void);

/* Overflow count since boot, preserved across TALK/MONITOR transitions.
 * flush_pool is disabled: each failed pool insertion loses the new frame. */
uint32_t voice_pool_ovf_total(void);

typedef struct {
    bool running;
    bool clock_exact;
    uint32_t session;
    uint32_t ovf_total;
    uint32_t ovf_session;
    uint32_t pool_clears;
    uint32_t requested_hz;
    uint32_t configured_hz;
    uint32_t source_hz;
    uint32_t div_num, div_a, div_b, interval;
    uint32_t produced_hz, read_hz;
    uint32_t elapsed_ms;
    uint32_t i2s_write_max_us;
    uint64_t produced_samples, read_samples;
} voice_adc_diag_t;

/* Session rates count DMA completions separately from successful pool reads.
 * They freeze when capture stops; ovf_total and pool_clears persist until boot. */
void voice_adc_diag_get(voice_adc_diag_t *out);

/*
 * Transmit (TALK path) gain, fixed-point at 1/16: the applied multiplier is
 * value/16, so 16 = unity, 32 = 2x, 8 = 0.5x.  Persisted in NVS.
 */
void voice_set_tx_gain(int g);
int  voice_get_tx_gain(void);

/*
 * Network/app transmit gain: the same fixed-point scale, a separate multiplier.
 * The phone stream and the handset capsule need different amounts, so tuning one
 * must not drag the other into clipping.  Persisted in NVS; 96 (6.0x) default.
 */
void voice_set_net_tx_gain(int g);
int  voice_get_net_tx_gain(void);

/*
 * Handset microphone PGA (ES8311 REG16): index 0..7 = 0/6/.../42 dB, persisted.
 * voice_mic_gain_load() restores it at boot; the codec alone forgot on reboot.
 */
void voice_set_mic_gain_index(int idx);
void voice_mic_gain_load(void);

/* Apply the ordinary TALK microphone processing to browser PCM.  Returns the
 * processed block peak; only fixed EQ/gain and integer saturation are used. */
int voice_apply_tx_processing(int16_t *pcm, size_t n);
/* Phone/browser PCM: same microphone EQ, network TX multiplier (not the TALK one). */
int voice_apply_net_tx_processing(int16_t *pcm, size_t n);

/* Peak of the band-passed input since the last call, then reset.  Lets the
 * gain sweep tell "no limiter hits" from "the operator was not talking". */
int voice_take_peak(void);

/*
 * Persisted preference: start MONITOR automatically at boot.
 *
 * Only MONITOR can auto-start, never TALK: monitor is receive-only and cannot
 * key the transmitter, so leaving it enabled is safe.  This exists so the board
 * is useful as a standalone receiver on a power bank, with no console attached.
 */
void voice_set_autostart(bool on);
bool voice_get_autostart(void);

/*
 * Global 6-band voice EQ, one chain per direction (see eq6.h):
 *
 *   0  low-shelf   150 Hz    depth / rumble
 *   1  peaking     400 Hz    warmth
 *   2  peaking     900 Hz    voice body
 *   3  peaking     2 kHz     clarity
 *   4  peaking     3.2 kHz   presence
 *   5  high-shelf  3.8 kHz   air / sibilance
 *
 * Each band takes -12..+12 dB (0 = wire).  `dir` is 0 for the speaker
 * (MONITOR output, before the DAC) and 1 for the microphone (TALK input,
 * before the TX gain).  Global means: set once, applies to every future
 * session of that direction, and is persisted in NVS -- a restart restores
 * the operator's tuning.  voice_eq_load() restores the persisted values;
 * call it once at boot.  Setting a band while a session runs takes effect
 * immediately (coefficient recomputation + state reset).
 */
void voice_eq_load(void);
void voice_eq_set(int dir, const int db[6]);
void voice_eq_get(int dir, int db[6]);
void voice_eq_reset(void);

/*
 * Speaker volume, eight steps (0..7).  This is the volume the operator changes
 * with the front-panel keys and the panel's "喇叭音量" control:
 *
 *   0  amplifier shut down (speaker silent; radio and network keep running)
 *   1  40  -38.5 dB   2  45  -32 dB    3  50  -25.5 dB   4  55  -19.5 dB
 *   5  60  -13 dB     6  65  -6.5 dB   7  70   0 dB (unity)
 *
 * The step numbers are the codec DAC volume values: step N sounds exactly like
 * "DAC output volume = N" used to (the old panel's 0-100 slider, of which the
 * operator kept 70).  The multipliers are computed from the ES8311's DAC
 * register curve (0.5 dB per step, 0xBF = 0 dB) plus the driver's linear
 * 0-100 -> register mapping -- see the table in voice.c.
 *
 * WHY IT IS NOT THE CODEC'S DAC VOLUME
 *
 * Step 7 (70) is unity at the codec's own volume; every other step attenuates
 * the speaker-bound audio relative to it.  The codec register itself is left
 * alone on purpose: on this board the ES8311's one DAC output
 * (net DAC_P) is wired to BOTH the speaker amplifier (R13 -> AMP_IN -> LM4871)
 * and the radio's transmit audio input (C30/R16 -> SA818_MIC, U5.18).  Turning
 * the DAC volume down would therefore turn our own transmitted microphone and
 * tail tone down as well -- measured in the netlist and stated in talk_task's
 * own comment ("the amplifier stays off as during talk, so the DAC output
 * reaches only the SA818 MIC_IN").
 *
 * So the level is applied as an attenuation on the audio that is bound for the
 * LOUDSPEAKER only, and the outbound paths (talk/mic, the transmitted tail, the
 * analog bridge's RF retransmission) are left untouched.  Callers mark their
 * buffer with voice_speaker_apply() only when it is speaker-bound.
 *
 * Persisted in NVS ("orl", key "spk"); voice_speaker_level_load() runs at boot.
 */
#define VOICE_SPK_MIN 0
#define VOICE_SPK_MAX 7
#define VOICE_SPK_REF 7          /* 0 dB: the 70 step, unity at the codec's own volume */

void voice_set_speaker_level(int level);
int  voice_get_speaker_level(void);
void voice_speaker_level_load(void);

/*
 * Attenuate `n` samples of a buffer that is about to be written to the DAC for
 * the LOUDSPEAKER.  Must NOT be called for audio that leaves the board (the
 * talk path, the transmitted tail tone, an analog-bridge retransmission):
 * those have to keep the level the operator tuned.
 */
void voice_speaker_apply(int16_t *pcm, size_t n);

/* True at step 0: the amplifier is held shut whatever the audio gates ask for. */
bool voice_speaker_muted(void);

/*
 * Codec DAC volume (0..100) -- a BENCH control, not the operator's volume.
 *
 * Because the codec DAC output also feeds the radio's transmit input (see the
 * speaker-volume note above), this register is a system level: moving it changes
 * what we transmit as well.  It stays where the board was tuned (60); the panel
 * does not show it and the volume keys do not touch it.  Persisted in NVS
 * ("orl", "vvol"); voice_output_volume_load() runs at boot.
 */
void voice_set_output_volume(int vol);
int  voice_get_output_volume(void);
void voice_output_volume_load(void);

/*
 * Restore every audio parameter to its factory default (receive gain, TX gain,
 * speaker level, AGC off, LED threshold, both 6-band EQ chains), and persist the
 * ones that live in NVS.  Used by the panel's "restore panel defaults".
 */
void voice_restore_defaults(void);

#ifdef __cplusplus
}
#endif
