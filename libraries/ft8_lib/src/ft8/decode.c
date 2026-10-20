#include "decode.h"
#include "constants.h"
#include "crc.h"
#include "ldpc.h"
#include "encode.h"

#include <stdbool.h>
#include <math.h>

// #define LOG_LEVEL LOG_DEBUG
// #include "debug.h"

// Lookup table for y = 10*log10(1 + 10^(x/10)), where
//   y - increase in signal level dB when adding a weaker independent signal
//   x - specific relative strength of the weaker signal in dB
// Table index corresponds to x in dB (index 0: 0 dB, index 1: -1 dB etc)
static const float db_power_sum[40] = {
    3.01029995663981f, 2.53901891043867f, 2.1244260279434f, 1.76434862436485f, 1.45540463109294f,
    1.19331048066095f, 0.973227937086954f, 0.790097496525665f, 0.638920341433796f, 0.514969420252302f,
    0.413926851582251f, 0.331956199884278f, 0.265723755961025f, 0.212384019142551f, 0.16954289279533f,
    0.135209221080382f, 0.10774225511957f, 0.085799992300358f, 0.06829128312453f, 0.054333142200458f,
    0.043213737826426f, 0.034360947517284f, 0.027316043349389f, 0.021711921641451f, 0.017255250287928f,
    0.013711928326833f, 0.010895305999614f, 0.008656680827934f, 0.006877654943187f, 0.005464004928574f,
    0.004340774793186f, 0.003448354310253f, 0.002739348814965f, 0.002176083232619f, 0.001728613409904f,
    0.001373142636584f, 0.001090761428665f, 0.000866444976964f, 0.000688255828734f, 0.000546709946839f
};

/// Compute log likelihood log(p(1) / p(0)) of 174 message bits for later use in soft-decision LDPC decoding
/// @param[in] wf Waterfall data collected during message slot
/// @param[in] cand Candidate to extract the message from
/// @param[in] code_map Symbol encoding map
/// @param[out] log174 Output of decoded log likelihoods for each of the 174 message bits
static void ft4_extract_likelihood(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, float* log174);
static void ft8_extract_likelihood(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, float* log174);

/// Packs a string of bits each represented as a zero/non-zero byte in bit_array[],
/// as a string of packed bits starting from the MSB of the first byte of packed[]
/// @param[in] plain Array of bits (0 and nonzero values) with num_bits entires
/// @param[in] num_bits Number of bits (entries) passed in bit_array
/// @param[out] packed Byte-packed bits representing the data in bit_array
static void pack_bits(const uint8_t bit_array[], int num_bits, uint8_t packed[]);

static float max2(float a, float b);
static float max4(float a, float b, float c, float d);
static void heapify_down(ftx_candidate_t heap[], int heap_size);
static void heapify_up(ftx_candidate_t heap[], int heap_size);

static void ftx_normalize_logl(float* log174);
static void ft4_extract_symbol(const WF_ELEM_T* wf, float* logl);
static void ft8_extract_symbol(const WF_ELEM_T* wf, float* logl);
static void ft8_decode_multi_symbols(const WF_ELEM_T* wf, int num_bins, int n_syms, int bit_idx, float* log174);

#ifndef FTX_NO_FASTSYNC
#define FTX_NO_FASTSYNC 0
#endif

#if WF_NIBBLE
uint8_t ftx_wf_shift = 0;
const int8_t ftx_nib_level[16] = { -16, -10, -6, -2, 2, 6, 10, 14, 18, 22, 27, 33, 40, 49, 60, 74 };

static const WF_ELEM_T* get_cand_mag(const ftx_waterfall_t* wf, const ftx_candidate_t* candidate)
{
    int sub = (candidate->time_sub * wf->freq_osr) + candidate->freq_sub;
    ftx_wf_shift = (uint8_t)((sub & 1) * 4);
    const WF_ELEM_T* plane = wf->mag + (size_t)(sub >> 1) * wf->max_blocks * wf->block_stride;
    return plane + (candidate->time_offset * wf->block_stride) + candidate->freq_offset;
}
#else
static const WF_ELEM_T* get_cand_mag(const ftx_waterfall_t* wf, const ftx_candidate_t* candidate)
{
    int offset = candidate->time_offset;
    offset = (offset * wf->time_osr) + candidate->time_sub;
    offset = (offset * wf->freq_osr) + candidate->freq_sub;
    offset = (offset * wf->num_bins) + candidate->freq_offset;
    return wf->mag + offset;
}
#endif

static int ft8_sync_score(const ftx_waterfall_t* wf, const ftx_candidate_t* candidate)
{
    int score = 0;
    int num_average = 0;

    // Get the pointer to symbol 0 of the candidate
    const WF_ELEM_T* mag_cand = get_cand_mag(wf, candidate);

    // Compute average score over sync symbols (m+k = 0-7, 36-43, 72-79)
    for (int m = 0; m < FT8_NUM_SYNC; ++m)
    {
        for (int k = 0; k < FT8_LENGTH_SYNC; ++k)
        {
            int block = (FT8_SYNC_OFFSET * m) + k;          // relative to the message
            int block_abs = candidate->time_offset + block; // relative to the captured signal
            // Check for time boundaries
            if (block_abs < 0)
                continue;
            if (block_abs >= wf->num_blocks)
                break;

            // Get the pointer to symbol 'block' of the candidate
            const WF_ELEM_T* p8 = mag_cand + (block * wf->block_stride);

            // Weighted difference between the expected and all other symbols
            // Does not work as well as the alternative score below
            // score += 8 * p8[kFT8_Costas_pattern[k]] -
            //          p8[0] - p8[1] - p8[2] - p8[3] -
            //          p8[4] - p8[5] - p8[6] - p8[7];
            // ++num_average;

            // Check only the neighbors of the expected symbol frequency- and time-wise
            int sm = kFT8_Costas_pattern[k]; // Index of the expected bin
            if (sm > 0)
            {
                // look at one frequency bin lower
                score += WF_ELEM_MAG_INT(p8[sm]) - WF_ELEM_MAG_INT(p8[sm - 1]);
                ++num_average;
            }
            if (sm < 7)
            {
                // look at one frequency bin higher
                score += WF_ELEM_MAG_INT(p8[sm]) - WF_ELEM_MAG_INT(p8[sm + 1]);
                ++num_average;
            }
            if ((k > 0) && (block_abs > 0))
            {
                // look one symbol back in time
                score += WF_ELEM_MAG_INT(p8[sm]) - WF_ELEM_MAG_INT(p8[sm - wf->block_stride]);
                ++num_average;
            }
            if (((k + 1) < FT8_LENGTH_SYNC) && ((block_abs + 1) < wf->num_blocks))
            {
                // look one symbol forward in time
                score += WF_ELEM_MAG_INT(p8[sm]) - WF_ELEM_MAG_INT(p8[sm + wf->block_stride]);
                ++num_average;
            }
        }
    }

    if (num_average > 0)
        score /= num_average;

    return score;
}

static int ft4_sync_score(const ftx_waterfall_t* wf, const ftx_candidate_t* candidate)
{
    int score = 0;
    int num_average = 0;

    // Get the pointer to symbol 0 of the candidate
    const WF_ELEM_T* mag_cand = get_cand_mag(wf, candidate);

    // Compute average score over sync symbols (block = 1-4, 34-37, 67-70, 100-103)
    for (int m = 0; m < FT4_NUM_SYNC; ++m)
    {
        for (int k = 0; k < FT4_LENGTH_SYNC; ++k)
        {
            int block = 1 + (FT4_SYNC_OFFSET * m) + k;
            int block_abs = candidate->time_offset + block;
            // Check for time boundaries
            if (block_abs < 0)
                continue;
            if (block_abs >= wf->num_blocks)
                break;

            // Get the pointer to symbol 'block' of the candidate
            const WF_ELEM_T* p4 = mag_cand + (block * wf->block_stride);

            int sm = kFT4_Costas_pattern[m][k]; // Index of the expected bin

            // score += (4 * p4[sm]) - p4[0] - p4[1] - p4[2] - p4[3];
            // num_average += 4;

            // Check only the neighbors of the expected symbol frequency- and time-wise
            if (sm > 0)
            {
                // look at one frequency bin lower
                score += WF_ELEM_MAG_INT(p4[sm]) - WF_ELEM_MAG_INT(p4[sm - 1]);
                ++num_average;
            }
            if (sm < 3)
            {
                // look at one frequency bin higher
                score += WF_ELEM_MAG_INT(p4[sm]) - WF_ELEM_MAG_INT(p4[sm + 1]);
                ++num_average;
            }
            if ((k > 0) && (block_abs > 0))
            {
                // look one symbol back in time
                score += WF_ELEM_MAG_INT(p4[sm]) - WF_ELEM_MAG_INT(p4[sm - wf->block_stride]);
                ++num_average;
            }
            if (((k + 1) < FT4_LENGTH_SYNC) && ((block_abs + 1) < wf->num_blocks))
            {
                // look one symbol forward in time
                score += WF_ELEM_MAG_INT(p4[sm]) - WF_ELEM_MAG_INT(p4[sm + wf->block_stride]);
                ++num_average;
            }
        }
    }

    if (num_average > 0)
        score /= num_average;

    return score;
}

#if WF_NIBBLE
static void heap_push(ftx_candidate_t heap[], int* heap_size, int num_candidates, const ftx_candidate_t* candidate)
{
    // If the heap is full AND the current candidate is better than the worst in the heap,
    // remove the worst and make space
    if ((*heap_size == num_candidates) && (candidate->score > heap[0].score))
    {
        --*heap_size;
        heap[0] = heap[*heap_size];
        heapify_down(heap, *heap_size);
    }
    if (*heap_size < num_candidates)
    {
        heap[*heap_size] = *candidate;
        ++*heap_size;
        heapify_up(heap, *heap_size);
    }
}

// Same result as the generic search with ft8_sync_score(), but scores every frequency offset of a
// (sub-plane, time offset) at once: the per-symbol bounds checks are hoisted out of the bin loop
// and each pass is a straight run over one waterfall row.
static int ft8_find_candidates_nib(const ftx_waterfall_t* wf, int num_candidates, ftx_candidate_t heap[], int min_score)
{
    enum { MAX_F = 512 };
    int32_t acc[MAX_F];
    int8_t lv[16];
    for (int i = 0; i < 16; ++i)
        lv[i] = ftx_nib_level[i];
    const int nf = wf->num_bins - 7;   // freq_offset + 7 < num_bins
    if (nf <= 0 || nf > MAX_F)
        return 0;
    const int stride = wf->block_stride;

    int heap_size = 0;
    ftx_candidate_t candidate;
    int min_time_offset = (wf->num_blocks <= 86) ? -5 : -15;
    int max_time_offset = (wf->num_blocks <= 86) ? 16 : 28;
    for (int ts = 0; ts < wf->time_osr; ++ts)
    {
        for (int fs = 0; fs < wf->freq_osr; ++fs)
        {
            int sub = ts * wf->freq_osr + fs;
            const int sh = (sub & 1) * 4;
            const uint8_t* plane = wf->mag + (size_t)(sub >> 1) * wf->max_blocks * stride;
            for (int t = min_time_offset; t < max_time_offset; ++t)
            {
                for (int f = 0; f < nf; ++f)
                    acc[f] = 0;
                int num_average = 0;
                for (int m = 0; m < FT8_NUM_SYNC; ++m)
                {
                    for (int k = 0; k < FT8_LENGTH_SYNC; ++k)
                    {
                        int block_abs = t + (FT8_SYNC_OFFSET * m) + k;
                        if (block_abs < 0)
                            continue;
                        if (block_abs >= wf->num_blocks)
                            break;
                        int sm = kFT8_Costas_pattern[k];
                        const uint8_t* p = plane + block_abs * stride + sm;
                        int lo = (sm > 0), hi = (sm < 7);
                        int back = (k > 0) && (block_abs > 0);
                        int fwd = ((k + 1) < FT8_LENGTH_SYNC) && ((block_abs + 1) < wf->num_blocks);
                        int n = lo + hi + back + fwd;
                        num_average += n;
                        for (int f = 0; f < nf; ++f)
                            acc[f] += n * lv[(p[f] >> sh) & 15];
                        if (lo)
                            for (int f = 0; f < nf; ++f) acc[f] -= lv[(p[f - 1] >> sh) & 15];
                        if (hi)
                            for (int f = 0; f < nf; ++f) acc[f] -= lv[(p[f + 1] >> sh) & 15];
                        if (back)
                            for (int f = 0; f < nf; ++f) acc[f] -= lv[(p[f - stride] >> sh) & 15];
                        if (fwd)
                            for (int f = 0; f < nf; ++f) acc[f] -= lv[(p[f + stride] >> sh) & 15];
                    }
                }
                candidate.time_sub = ts;
                candidate.freq_sub = fs;
                candidate.time_offset = t;
                for (int f = 0; f < nf; ++f)
                {
                    int score = (num_average > 0) ? acc[f] / num_average : 0;
                    if (score < min_score)
                        continue;
                    candidate.freq_offset = f;
                    candidate.score = score;
                    heap_push(heap, &heap_size, num_candidates, &candidate);
                }
            }
        }
    }
    return heap_size;
}
#endif

int ftx_find_candidates(const ftx_waterfall_t* wf, int num_candidates, ftx_candidate_t heap[], int min_score)
{
    int heap_size = 0;
#if WF_NIBBLE
#if defined(ESP32) || defined(ESP_PLATFORM)
    const bool in_iram = (uintptr_t)wf->mag >= 0x40000000UL;   // IRAM needs 32-bit loads (wf_read_u8)
#else
    const bool in_iram = false;
#endif
    if (wf->protocol == FTX_PROTOCOL_FT8 && !in_iram && !FTX_NO_FASTSYNC)
    {
        heap_size = ft8_find_candidates_nib(wf, num_candidates, heap, min_score);
        goto sort;
    }
#endif
    {
    int (*sync_fun)(const ftx_waterfall_t*, const ftx_candidate_t*) = (wf->protocol == FTX_PROTOCOL_FT4) ? ft4_sync_score : ft8_sync_score;
    int num_tones = (wf->protocol == FTX_PROTOCOL_FT4) ? 4 : 8;

    ftx_candidate_t candidate;

    // Here we allow time offsets that exceed signal boundaries, as long as we still have all data bits.
    // I.e. we can afford to skip the first 7 or the last 7 Costas symbols, as long as we track how many
    // sync symbols we included in the score, so the score is averaged.
    int min_time_offset = (wf->num_blocks <= 86) ? -5 : -15;
    int max_time_offset = (wf->num_blocks <= 86) ? 16 : 28;
    for (candidate.time_sub = 0; candidate.time_sub < wf->time_osr; ++candidate.time_sub)
    {
        for (candidate.freq_sub = 0; candidate.freq_sub < wf->freq_osr; ++candidate.freq_sub)
        {
            for (candidate.time_offset = min_time_offset; candidate.time_offset < max_time_offset; ++candidate.time_offset)
            {
                for (candidate.freq_offset = 0; (candidate.freq_offset + num_tones - 1) < wf->num_bins; ++candidate.freq_offset)
                {
                    candidate.score = sync_fun(wf, &candidate);

                    if (candidate.score < min_score)
                        continue;

                    // If the heap is full AND the current candidate is better than
                    // the worst in the heap, we remove the worst and make space
                    if ((heap_size == num_candidates) && (candidate.score > heap[0].score))
                    {
                        --heap_size;
                        heap[0] = heap[heap_size];
                        heapify_down(heap, heap_size);
                    }

                    // If there's free space in the heap, we add the current candidate
                    if (heap_size < num_candidates)
                    {
                        heap[heap_size] = candidate;
                        ++heap_size;
                        heapify_up(heap, heap_size);
                    }
                }
            }
        }
    }

    }
sort:;
    // Sort the candidates by sync strength - here we benefit from the heap structure
    int len_unsorted = heap_size;
    while (len_unsorted > 1)
    {
        // Take the top (index 0) element which is guaranteed to have the smallest score,
        // exchange it with the last element in the heap, and decrease the heap size.
        // Then restore the heap property in the new, smaller heap.
        // At the end the elements will be sorted in descending order.
        ftx_candidate_t tmp = heap[len_unsorted - 1];
        heap[len_unsorted - 1] = heap[0];
        heap[0] = tmp;
        len_unsorted--;
        heapify_down(heap, len_unsorted);
    }

    return heap_size;
}

static void ft4_extract_likelihood(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, float* log174)
{
    const WF_ELEM_T* mag = get_cand_mag(wf, cand); // Pointer to 4 magnitude bins of the first symbol

    // Go over FSK tones and skip Costas sync symbols
    for (int k = 0; k < FT4_ND; ++k)
    {
        // Skip either 5, 9 or 13 sync symbols
        // TODO: replace magic numbers with constants
        int sym_idx = k + ((k < 29) ? 5 : ((k < 58) ? 9 : 13));
        int bit_idx = 2 * k;

        // Check for time boundaries
        int block = cand->time_offset + sym_idx;
        if ((block < 0) || (block >= wf->num_blocks))
        {
            log174[bit_idx + 0] = 0;
            log174[bit_idx + 1] = 0;
        }
        else
        {
            ft4_extract_symbol(mag + (sym_idx * wf->block_stride), log174 + bit_idx);
        }
    }
}

static void ft8_extract_likelihood(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, float* log174)
{
    const WF_ELEM_T* mag = get_cand_mag(wf, cand); // Pointer to 8 magnitude bins of the first symbol

    // Go over FSK tones and skip Costas sync symbols
    for (int k = 0; k < FT8_ND; ++k)
    {
        // Skip either 7 or 14 sync symbols
        // TODO: replace magic numbers with constants
        int sym_idx = k + ((k < 29) ? 7 : 14);
        int bit_idx = 3 * k;

        // Check for time boundaries
        int block = cand->time_offset + sym_idx;
        if ((block < 0) || (block >= wf->num_blocks))
        {
            log174[bit_idx + 0] = 0;
            log174[bit_idx + 1] = 0;
            log174[bit_idx + 2] = 0;
        }
        else
        {
            ft8_extract_symbol(mag + (sym_idx * wf->block_stride), log174 + bit_idx);
        }
    }
}

static void ftx_normalize_logl(float* log174)
{
    // Compute the variance of log174
    float sum = 0;
    float sum2 = 0;
    for (int i = 0; i < FTX_LDPC_N; ++i)
    {
        sum += log174[i];
        sum2 += log174[i] * log174[i];
    }
    float inv_n = 1.0f / FTX_LDPC_N;
    float variance = (sum2 - (sum * sum * inv_n)) * inv_n;

    // Normalize log174 distribution and scale it with experimentally found coefficient
    float norm_factor = sqrtf(24.0f / variance);
    for (int i = 0; i < FTX_LDPC_N; ++i)
    {
        log174[i] *= norm_factor;
    }
}

static bool decode_candidate_impl(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, int max_iterations, int ap_max_errors, ftx_message_t* message, ftx_decode_status_t* status);

bool ftx_decode_candidate(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, int max_iterations, ftx_message_t* message, ftx_decode_status_t* status)
{
    status->used_ap = false;
    return decode_candidate_impl(wf, cand, max_iterations, -1, message, status);
}

bool ftx_decode_candidate_ap(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, int max_iterations, int ap_max_errors, ftx_message_t* message, ftx_decode_status_t* status)
{
    status->used_ap = false;
    return decode_candidate_impl(wf, cand, max_iterations, ap_max_errors, message, status);
}

// A-priori "CQ" pass (like WSJT-X AP type 1): for a standard message "CQ <call> <grid>", payload bits
// 0-27 are c28 = 2 (CQ), bit 28 is ip = 0 and bits 74-76 are i3 = 001. Those LLRs are pinned to a strong
// value (4x the mean magnitude; >0 means bit 1) and BP runs again. Tested offline on real 15 m slots:
// 1 -> 5 of 9 jt9 decodes, no false decodes in 2,700 attempts on real and pure-noise candidates.
static void apply_ap_cq(float* log174)
{
    float mean = 0;
    for (int k = 0; k < FTX_LDPC_N; ++k)
        mean += fabsf(log174[k]);
    float big = 4.0f * mean / FTX_LDPC_N;
    for (int k = 0; k < 29; ++k)
        log174[k] = (k == 26) ? big : -big;
    log174[74] = -big;
    log174[75] = -big;
    log174[76] = big;
}

static bool decode_candidate_impl(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, int max_iterations, int ap_max_errors, ftx_message_t* message, ftx_decode_status_t* status)
{
    float log174[FTX_LDPC_N]; // message bits encoded as likelihood
    if (wf->protocol == FTX_PROTOCOL_FT4)
    {
        ft4_extract_likelihood(wf, cand, log174);
    }
    else
    {
        ft8_extract_likelihood(wf, cand, log174);
    }

    ftx_normalize_logl(log174);

    uint8_t plain174[FTX_LDPC_N]; // message bits (0/1)
    bp_decode(log174, max_iterations, plain174, &status->ldpc_errors);

    // If standard BP + syndrome bit-flip is close to convergence (1..5 parity errors),
    // retry once with damped LLR temperature (0.72f)
    if (status->ldpc_errors > 0 && status->ldpc_errors <= 5)
    {
        float log174_try[FTX_LDPC_N];
        for (int i = 0; i < FTX_LDPC_N; ++i)
            log174_try[i] = log174[i] * 0.72f;
        uint8_t plain_try[FTX_LDPC_N];
        int err_try = 99;
        bp_decode(log174_try, max_iterations, plain_try, &err_try);
        if (err_try < status->ldpc_errors)
        {
            status->ldpc_errors = err_try;
            for (int i = 0; i < FTX_LDPC_N; ++i)
                plain174[i] = plain_try[i];
        }
    }

    // Close but not converged: try again assuming the message is a CQ (FT8 only)
    if (status->ldpc_errors > 0 && status->ldpc_errors <= ap_max_errors && wf->protocol == FTX_PROTOCOL_FT8)
    {
        apply_ap_cq(log174);
        uint8_t plain_ap[FTX_LDPC_N];
        int err_ap = 99;
        bp_decode(log174, max_iterations, plain_ap, &err_ap);
        if (err_ap == 0)
        {
            status->ldpc_errors = 0;
            status->used_ap = true;
            for (int i = 0; i < FTX_LDPC_N; ++i)
                plain174[i] = plain_ap[i];
        }
    }

    if (status->ldpc_errors > 0)
    {
        return false;
    }


    // Extract payload + CRC (first FTX_LDPC_K bits) packed into a byte array
    uint8_t a91[FTX_LDPC_K_BYTES];
    pack_bits(plain174, FTX_LDPC_K, a91);

    // Extract CRC and check it
    status->crc_extracted = ftx_extract_crc(a91);
    // [1]: 'The CRC is calculated on the source-encoded message, zero-extended from 77 to 82 bits.'
    a91[9] &= 0xF8;
    a91[10] &= 0x00;
    status->crc_calculated = ftx_compute_crc(a91, 96 - 14);

    if (status->crc_extracted != status->crc_calculated)
    {
        return false;
    }

    // Reuse CRC value as a hash for the message (TODO: 14 bits only, should perhaps use full 16 or 32 bits?)
    message->hash = status->crc_calculated;

    if (wf->protocol == FTX_PROTOCOL_FT4)
    {
        // '[..] for FT4 only, in order to avoid transmitting a long string of zeros when sending CQ messages,
        // the assembled 77-bit message is bitwise exclusive-OR’ed with [a] pseudorandom sequence before computing the CRC and FEC parity bits'
        for (int i = 0; i < 10; ++i)
        {
            message->payload[i] = a91[i] ^ kFT4_XOR_sequence[i];
        }
    }
    else
    {
        for (int i = 0; i < 10; ++i)
        {
            message->payload[i] = a91[i];
        }
    }

    // LOG(LOG_DEBUG, "Decoded message (CRC %04x), trying to unpack...\n", status->crc_extracted);
    return true;
}

static float max2(float a, float b)
{
    return (a >= b) ? a : b;
}

static float max4(float a, float b, float c, float d)
{
    return max2(max2(a, b), max2(c, d));
}

static void heapify_down(ftx_candidate_t heap[], int heap_size)
{
    // heapify from the root down
    int current = 0; // root node
    while (true)
    {
        int left = 2 * current + 1;
        int right = left + 1;

        // Find the smallest value of (parent, left child, right child)
        int smallest = current;
        if ((left < heap_size) && (heap[left].score < heap[smallest].score))
        {
            smallest = left;
        }
        if ((right < heap_size) && (heap[right].score < heap[smallest].score))
        {
            smallest = right;
        }

        if (smallest == current)
        {
            break;
        }

        // Exchange the current node with the smallest child and move down to it
        ftx_candidate_t tmp = heap[smallest];
        heap[smallest] = heap[current];
        heap[current] = tmp;
        current = smallest;
    }
}

static void heapify_up(ftx_candidate_t heap[], int heap_size)
{
    // heapify from the last node up
    int current = heap_size - 1;
    while (current > 0)
    {
        int parent = (current - 1) / 2;
        if (!(heap[current].score < heap[parent].score))
        {
            break;
        }

        // Exchange the current node with its parent and move up
        ftx_candidate_t tmp = heap[parent];
        heap[parent] = heap[current];
        heap[current] = tmp;
        current = parent;
    }
}

// Compute unnormalized log likelihood log(p(1) / p(0)) of 2 message bits (1 FSK symbol)
static void ft4_extract_symbol(const WF_ELEM_T* wf, float* logl)
{
    // Cleaned up code for the simple case of n_syms==1
    float s2[4];

    for (int j = 0; j < 4; ++j)
    {
        s2[j] = WF_ELEM_MAG(wf[kFT4_Gray_map[j]]);
    }

    logl[0] = max2(s2[2], s2[3]) - max2(s2[0], s2[1]);
    logl[1] = max2(s2[1], s2[3]) - max2(s2[0], s2[2]);
}

// Compute unnormalized log likelihood log(p(1) / p(0)) of 3 message bits (1 FSK symbol)
static void ft8_extract_symbol(const WF_ELEM_T* wf, float* logl)
{
    // Cleaned up code for the simple case of n_syms==1
#if 1
    float s2[8];

    for (int j = 0; j < 8; ++j)
    {
        s2[j] = WF_ELEM_MAG(wf[kFT8_Gray_map[j]]);
    }

    logl[0] = max4(s2[4], s2[5], s2[6], s2[7]) - max4(s2[0], s2[1], s2[2], s2[3]);
    logl[1] = max4(s2[2], s2[3], s2[6], s2[7]) - max4(s2[0], s2[1], s2[4], s2[5]);
    logl[2] = max4(s2[1], s2[3], s2[5], s2[7]) - max4(s2[0], s2[2], s2[4], s2[6]);
#else
    float a[7] = {
        // (float)wf[7] - (float)wf[0], // 0: p(111) / p(000)
        (float)wf[5] - (float)wf[2], // 0: p(100) / p(011)
        (float)wf[3] - (float)wf[0], // 1: p(010) / p(000)
        (float)wf[6] - (float)wf[3], // 2: p(101) / p(010)
        (float)wf[6] - (float)wf[2], // 3: p(101) / p(011)
        (float)wf[7] - (float)wf[4], // 4: p(111) / p(110)
        (float)wf[4] - (float)wf[1], // 5: p(110) / p(001)
        (float)wf[5] - (float)wf[1]  // 6: p(100) / p(001)
    };
    float k = 1.0f;

    // logl[0] = k * (a[0] + a[2] + a[3] + a[5] + a[6]) / 5;
    // logl[1] = k * (a[0] / 4 + (a[1] - a[3]) * 5 / 24 + (a[5] - a[2]) / 6 + (a[4] - a[6]) / 24);
    // logl[2] = k * (a[0] / 4 + (a[1] - a[3]) / 24 + (a[2] - a[5]) / 6 + (a[4] - a[6]) * 5 / 24);
    logl[0] = k * (a[1] / 6 + a[2] / 3 + a[3] / 6 + a[4] / 6 + a[5] / 3 + a[6] / 6);
    logl[1] = k * (-a[0] / 4 + a[1] * 7 / 24 + (a[4] - a[3]) / 8 + a[5] / 3 + a[6] / 24);
    logl[2] = k * (-a[0] / 4 + (a[1] - a[6]) / 8 + a[2] / 3 + a[3] / 24 + a[4] * 7 / 24 - a[5] * 5 / 18);
#endif
    // for (int i = 0; i < 8; ++i)
    //     printf("%d ", WF_ELEM_MAG_INT(wf[i]));
    // for (int i = 0; i < 3; ++i)
    //     printf("%.1f ", logl[i]);
    // printf("\n");
}

// Compute unnormalized log likelihood log(p(1) / p(0)) of bits corresponding to several FSK symbols at once
static void ft8_decode_multi_symbols(const WF_ELEM_T* wf, int num_bins, int n_syms, int bit_idx, float* log174)
{
    const int n_bits = 3 * n_syms;
    const int n_tones = (1 << n_bits);

    float s2[n_tones];

    for (int j = 0; j < n_tones; ++j)
    {
        int j1 = j & 0x07;
        if (n_syms == 1)
        {
            s2[j] = WF_ELEM_MAG(wf[kFT8_Gray_map[j1]]);
            continue;
        }
        int j2 = (j >> 3) & 0x07;
        if (n_syms == 2)
        {
            s2[j] = WF_ELEM_MAG(wf[kFT8_Gray_map[j2]]);
            s2[j] += WF_ELEM_MAG(wf[kFT8_Gray_map[j1] + 4 * num_bins]);
            continue;
        }
        int j3 = (j >> 6) & 0x07;
        s2[j] = WF_ELEM_MAG(wf[kFT8_Gray_map[j3]]);
        s2[j] += WF_ELEM_MAG(wf[kFT8_Gray_map[j2] + 4 * num_bins]);
        s2[j] += WF_ELEM_MAG(wf[kFT8_Gray_map[j1] + 8 * num_bins]);
    }

    // Extract bit significance (and convert them to float)
    // 8 FSK tones = 3 bits
    for (int i = 0; i < n_bits; ++i)
    {
        if (bit_idx + i >= FTX_LDPC_N)
        {
            // Respect array size
            break;
        }

        uint16_t mask = (n_tones >> (i + 1));
        float max_zero = -1000, max_one = -1000;
        for (int n = 0; n < n_tones; ++n)
        {
            if (n & mask)
            {
                max_one = max2(max_one, s2[n]);
            }
            else
            {
                max_zero = max2(max_zero, s2[n]);
            }
        }

        log174[bit_idx + i] = max_one - max_zero;
    }
}

// Packs a string of bits each represented as a zero/non-zero byte in plain[],
// as a string of packed bits starting from the MSB of the first byte of packed[]
static void pack_bits(const uint8_t bit_array[], int num_bits, uint8_t packed[])
{
    int num_bytes = (num_bits + 7) / 8;
    for (int i = 0; i < num_bytes; ++i)
    {
        packed[i] = 0;
    }

    uint8_t mask = 0x80;
    int byte_idx = 0;
    for (int i = 0; i < num_bits; ++i)
    {
        if (bit_array[i])
        {
            packed[byte_idx] |= mask;
        }
        mask >>= 1;
        if (!mask)
        {
            mask = 0x80;
            ++byte_idx;
        }
    }
}
#ifndef FTX_SNR_OFFSET_DB
#define FTX_SNR_OFFSET_DB (-24.3f)   // 10*log10(9.375 Hz Hann noise bandwidth / 2500 Hz); jt9 agrees within ~2 dB below 2.4 kHz
#endif

float ftx_message_snr(const ftx_waterfall_t* wf, const ftx_candidate_t* cand, const ftx_message_t* message)
{
    uint8_t tones[FT4_NN];
    const bool is_ft4 = (wf->protocol == FTX_PROTOCOL_FT4);
    const int nn = is_ft4 ? FT4_NN : FT8_NN;
    const int ntones = is_ft4 ? 4 : 8;
    if (is_ft4)
        ft4_encode(message->payload, tones);
    else
        ft8_encode(message->payload, tones);

    // Sum the linear power of the sent tone and of the tone ntones/2 bins away (noise only) along
    // the decoded path. Both go through the same quantizer, so its bias mostly cancels in the ratio.
    const WF_ELEM_T* mag = get_cand_mag(wf, cand);
    float sig = 0, noise = 0;
    for (int i = 0; i < nn; ++i)
    {
        int block = cand->time_offset + i;
        if ((block < 0) || (block >= wf->num_blocks))
            continue;
        const WF_ELEM_T* p = mag + i * wf->block_stride;
        int t = tones[i];
        sig += powf(10.0f, 0.1f * WF_ELEM_MAG(p[t]));
        noise += powf(10.0f, 0.1f * WF_ELEM_MAG(p[(t + ntones / 2) % ntones]));
    }
    float arg = (noise > 0) ? sig / noise - 1.0f : 0.0f;
    if (arg < 0.001f)
        arg = 0.001f;
    float snr = 10.0f * log10f(arg) + FTX_SNR_OFFSET_DB;
    if (snr < -24.0f)
        snr = -24.0f;
    if (snr > 30.0f)
        snr = 30.0f;
    return snr;
}
