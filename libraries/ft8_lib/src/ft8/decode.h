#ifndef _INCLUDE_DECODE_H_
#define _INCLUDE_DECODE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "constants.h"
#include "message.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    float mag;
    float phase;
} waterfall_cpx_t;

// #define WATERFALL_USE_PHASE

#ifdef WATERFALL_USE_PHASE
#define WF_ELEM_T          waterfall_cpx_t
#define WF_ELEM_MAG(x)     ((x).mag)
#define WF_ELEM_MAG_INT(x) (int)(2 * ((x).mag + 120.0f))
#else
static inline uint8_t wf_read_u8(const uint8_t *ptr)
{
    uintptr_t addr = (uintptr_t)ptr;
    if (addr >= 0x40000000UL)
    {
        uint32_t w = *(const volatile uint32_t *)(addr & ~3UL);
        return (uint8_t)(w >> ((addr & 3UL) * 8U));
    }
    return *ptr;
}

static inline void wf_write_u8(uint8_t *ptr, uint8_t val)
{
    uintptr_t addr = (uintptr_t)ptr;
    if (addr >= 0x40000000UL)
    {
        volatile uint32_t *wptr = (volatile uint32_t *)(addr & ~3UL);
        uint32_t shift = (addr & 3UL) * 8U;
        *wptr = (*wptr & ~(0xFFUL << shift)) | ((uint32_t)val << shift);
        return;
    }
    *ptr = val;
}

#define WF_ELEM_T          uint8_t

// WF_NIBBLE: each waterfall byte packs two 4-bit oversampling sub-planes (low nibble = even
// sub index, high nibble = odd), so time_osr*freq_osr = 2 costs no more RAM than 1x1. A nibble
// is a companded level above the local noise floor (ftx_nib_level[], half-dB units: 2 dB steps
// around the noise where weak signals live, wider steps up to +37 dB so strong signals and their
// window leakage stay apart). The decoder only uses magnitude differences.
#ifndef WF_NIBBLE
#define WF_NIBBLE 1
#endif

#if WF_NIBBLE
extern uint8_t ftx_wf_shift;              // nibble selected by the last get_cand_mag() (0 or 4)
extern const int8_t ftx_nib_level[16];    // level -> half-dB above the noise median
#define WF_ELEM_MAG_INT(x) ((int)ftx_nib_level[(wf_read_u8(&(x)) >> ftx_wf_shift) & 0x0F])
#define WF_ELEM_MAG(x)     ((float)WF_ELEM_MAG_INT(x) * 0.5f)
#else
#define WF_ELEM_MAG(x)     ((float)wf_read_u8(&(x))*0.5f - 120.0f)
#define WF_ELEM_MAG_INT(x) ((int)wf_read_u8(&(x)))
#endif
#endif

/// Input structure to ftx_find_sync() function. This structure describes stored waterfall data over the whole message slot.
/// Fields time_osr and freq_osr specify additional oversampling rate for time and frequency resolution.
/// If time_osr=1, FFT magnitude data is collected once for every symbol transmitted, i.e. every 1/6.25 = 0.16 seconds.
/// Values time_osr > 1 mean each symbol is further subdivided in time.
/// If freq_osr=1, each bin in the FFT magnitude data corresponds to 6.25 Hz, which is the tone spacing.
/// Values freq_osr > 1 mean the tone spacing is further subdivided by FFT analysis.
typedef struct
{
    int max_blocks;          ///< number of blocks (symbols) allocated in the mag array
    int num_blocks;          ///< number of blocks (symbols) stored in the mag array
    int num_bins;            ///< number of FFT bins in terms of 6.25 Hz
    int time_osr;            ///< number of time subdivisions
    int freq_osr;            ///< number of frequency subdivisions
    WF_ELEM_T* mag;          ///< FFT magnitudes; WF_NIBBLE: (time_osr*freq_osr+1)/2 planes of max_blocks*num_bins bytes
    int block_stride;        ///< Helper value = freq_osr * num_bins (WF_NIBBLE: num_bins)
    ftx_protocol_t protocol; ///< Indicate if using FT4 or FT8
} ftx_waterfall_t;

/// Output structure of ftx_find_sync() and input structure of ftx_decode().
/// Holds the position of potential start of a message in time and frequency.
typedef struct
{
    int16_t score;       ///< Candidate score (non-negative number; higher score means higher likelihood)
    int16_t time_offset; ///< Index of the time block
    int16_t freq_offset; ///< Index of the frequency bin
    uint8_t time_sub;    ///< Index of the time subdivision used
    uint8_t freq_sub;    ///< Index of the frequency subdivision used
} ftx_candidate_t;

/// Structure that contains the status of various steps during decoding of a message
typedef struct
{
    float freq;
    float time;
    int ldpc_errors;         ///< Number of LDPC errors during decoding
    uint16_t crc_extracted;  ///< CRC value recovered from the message
    uint16_t crc_calculated; ///< CRC value calculated over the payload
    // int unpack_status;       ///< Return value of the unpack routine
    bool used_ap;     ///< decoded only with the a-priori CQ pass (ftx_decode_candidate_ap)
} ftx_decode_status_t;

/// Localize top N candidates in frequency and time according to their sync strength (looking at Costas symbols)
/// We treat and organize the candidate list as a min-heap (empty initially).
/// @param[in] power Waterfall data collected during message slot
/// @param[in] sync_pattern Synchronization pattern
/// @param[in] num_candidates Number of maximum candidates (size of heap array)
/// @param[in,out] heap Array of ftx_candidate_t type entries (with num_candidates allocated entries)
/// @param[in] min_score Minimal score allowed for pruning unlikely candidates (can be zero for no effect)
/// @return Number of candidates filled in the heap
int ftx_find_candidates(const ftx_waterfall_t* power, int num_candidates, ftx_candidate_t heap[], int min_score);

/// Attempt to decode a message candidate. Extracts the bit probabilities, runs LDPC decoder, checks CRC and unpacks the message in plain text.
/// @param[in] power Waterfall data collected during message slot
/// @param[in] cand Candidate to decode
/// @param[in] max_iterations Maximum allowed LDPC iterations (lower number means faster decode, but less precise)
/// @param[out] message ftx_message_t structure that will receive the decoded message
/// @param[out] status ftx_decode_status_t structure that will be filled with the status of various decoding steps
/// @return True if the decoding was successful, false otherwise (check status for details)
bool ftx_decode_candidate(const ftx_waterfall_t* power, const ftx_candidate_t* cand, int max_iterations, ftx_message_t* message, ftx_decode_status_t* status);

/// Same, plus an a-priori "CQ" pass when normal BP ends with 1..ap_max_errors parity errors
/// (ap_max_errors < 1 disables it). status->used_ap tells whether the AP pass produced the decode.
bool ftx_decode_candidate_ap(const ftx_waterfall_t* power, const ftx_candidate_t* cand, int max_iterations, int ap_max_errors, ftx_message_t* message, ftx_decode_status_t* status);


/// Estimate the SNR of a decoded message in a 2500 Hz bandwidth (WSJT-X style): the message is
/// re-encoded and the power of the transmitted tone in each symbol is compared with a tone 4 bins
/// away (FT8) or 2 bins away (FT4), which carries only noise. Clamped to -24..+30 dB.
/// @param[in] wf Waterfall data collected during message slot
/// @param[in] cand Candidate the message was decoded from
/// @param[in] message Decoded message
/// @return SNR in dB
float ftx_message_snr(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, const ftx_message_t* message);

#ifdef __cplusplus
}
#endif

#endif // _INCLUDE_DECODE_H_
