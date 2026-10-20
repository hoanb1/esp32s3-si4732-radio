#include "monitor.h"
#include <common/common.h>

// #define LOG_LEVEL LOG_INFO
#include <ft8/debug.h>

#include <stdlib.h>
#include <string.h>
#if defined(ESP32) || defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

static float hann_i(int i, int N)
{
    float x = sinf((float)M_PI * i / N);
    return x * x;
}

// Word buffer for float bits; on ESP32 prefer the 32-bit-only IRAM heap, which WiFi and the web
// server cannot use. IRAM only takes 32-bit integer loads/stores (no FPU loads, no memset).
static uint32_t* alloc_words(size_t n)
{
    uint32_t* p = NULL;
#if defined(ESP32) || defined(ESP_PLATFORM)
    p = (uint32_t*)heap_caps_malloc(n * 4, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
    if (!p) p = (uint32_t*)heap_caps_malloc(n * 4, MALLOC_CAP_32BIT);
#endif
    if (!p) p = (uint32_t*)malloc(n * 4);
    if (p)
        for (size_t i = 0; i < n; ++i) ((volatile uint32_t*)p)[i] = 0;
    return p;
}

static inline void half_put(uint32_t* q, int i, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    ((volatile uint32_t*)q)[i] = u;
}

static inline float half_get(const uint32_t* q, int i)
{
    uint32_t u = ((const volatile uint32_t*)q)[i];
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static void waterfall_init(ftx_waterfall_t* me, int max_blocks, int num_bins, int time_osr, int freq_osr)
{
#if WF_NIBBLE
    int num_planes = (time_osr * freq_osr + 1) / 2;   // two sub-planes per byte
    me->block_stride = num_bins;
#else
    int num_planes = time_osr;
    me->block_stride = (freq_osr * num_bins);
#endif
    size_t plane_size = (size_t)(num_planes * max_blocks * me->block_stride) * sizeof(me->mag[0]);
    me->max_blocks = max_blocks;
    me->num_blocks = 0;
    me->num_bins = num_bins;
    me->time_osr = time_osr;
    me->freq_osr = freq_osr;
    me->mag = NULL;
#if defined(ESP32) || defined(ESP_PLATFORM)
    size_t aligned_size = (plane_size + 3U) & ~3U;
    me->mag = (WF_ELEM_T*)heap_caps_malloc(aligned_size, MALLOC_CAP_32BIT);
#endif
    if (!me->mag)
    {
        me->mag = (WF_ELEM_T*)malloc(plane_size);
    }
    LOG(LOG_DEBUG, "Waterfall plane size = %zu\n", plane_size);
}

static void waterfall_free(ftx_waterfall_t* me)
{
    free(me->mag);
}

void monitor_init(monitor_t* me, const monitor_config_t* cfg)
{
    float slot_time = (cfg->protocol == FTX_PROTOCOL_FT4) ? FT4_SLOT_TIME : FT8_SLOT_TIME;
    float symbol_period = (cfg->protocol == FTX_PROTOCOL_FT4) ? FT4_SYMBOL_PERIOD : FT8_SYMBOL_PERIOD;
    // Compute DSP parameters that depend on the sample rate
    me->block_size = (int)(cfg->sample_rate * symbol_period); // samples corresponding to one FSK symbol
    me->subblock_size = me->block_size / cfg->time_osr;
    me->nfft = me->block_size;                                 // one-symbol Hann window; sub-bins come from a half-bin shift
    me->fft_norm = 2.0f / me->nfft;

    me->window = NULL;
    me->last_frame = NULL;
    me->aux_bins = NULL;
    me->row_db8 = NULL;
    me->half_buf = NULL;
    me->half_idx = 0;
#if WF_NIBBLE
    // time_osr=2: half_buf keeps the previous block's second half (ping-pong) and the current
    // first half, as raw float bits in 32-bit-only IRAM when available (see half_put/half_get).
    // freq_osr=2: last_frame holds the windowed frame for the cos/sin-modulated FFTs and
    // aux_bins the first of them.
    if (cfg->time_osr > 1)
        me->half_buf = alloc_words(3 * (me->block_size / 2));
    if (cfg->freq_osr > 1)
        me->last_frame = (float*)calloc(me->nfft + 2, sizeof(me->last_frame[0]));
#endif

    LOG(LOG_INFO, "Block size = %d\n", me->block_size);
    LOG(LOG_INFO, "Subblock size = %d\n", me->subblock_size);

    size_t fft_work_size = 0;
    kiss_fftr_alloc(me->nfft, 0, 0, &fft_work_size);
    me->fft_work = malloc(fft_work_size);
    me->fft_cfg = kiss_fftr_alloc(me->nfft, 0, me->fft_work, &fft_work_size);

    LOG(LOG_INFO, "N_FFT = %d\n", me->nfft);
    LOG(LOG_DEBUG, "FFT work area = %zu\n", fft_work_size);

    // Allocate enough blocks to fit the 85-block (13.6s) FT8 capture window in memory
    int max_blocks = (int)(slot_time / symbol_period);
    if (max_blocks > 85) max_blocks = 85;
    // Keep only FFT bins in the specified frequency range (f_min/f_max)
    me->min_bin = (int)(cfg->f_min * symbol_period);
    me->max_bin = (int)(cfg->f_max * symbol_period) + 1;
    const int num_bins = me->max_bin - me->min_bin;

#if WF_NIBBLE
    if (cfg->freq_osr > 1)
        me->aux_bins = (float*)malloc(2 * num_bins * sizeof(float));
    me->row_db8 = (uint8_t*)calloc(num_bins, 1);
#endif

    waterfall_init(&me->wf, max_blocks, num_bins, cfg->time_osr, cfg->freq_osr);
    me->wf.protocol = cfg->protocol;

    me->symbol_period = symbol_period;

    me->max_mag = -120.0f;
}

void monitor_free(monitor_t* me)
{
    waterfall_free(&me->wf);
    free(me->fft_work);
    if (me->last_frame) free(me->last_frame);
    if (me->half_buf) free(me->half_buf);
    if (me->aux_bins) free(me->aux_bins);
    if (me->row_db8) free(me->row_db8);
    if (me->window) free(me->window);
}

void monitor_reset(monitor_t* me)
{
    me->wf.num_blocks = 0;
    me->max_mag = -120.0f;
}

static inline uint8_t db_to_u8(monitor_t* me, float mag2)
{
    float db = 10.0f * log10f(1E-12f + mag2);
    if (db > me->max_mag) me->max_mag = db;
    int scaled = (int)(2 * db + 240);
    return (uint8_t)((scaled < 0) ? 0 : ((scaled > 255) ? 255 : scaled));
}

#if WF_NIBBLE
#define NIB_GROUP 16   // bins per noise-floor estimate (100 Hz)

static int median16(const uint8_t* v, int n)
{
    uint8_t s[NIB_GROUP];
    for (int i = 0; i < n; ++i)
    {
        uint8_t x = v[i];
        int j = i;
        while (j > 0 && s[j - 1] > x) { s[j] = s[j - 1]; --j; }
        s[j] = x;
    }
    return s[n / 2];
}

void monitor_store_row(ftx_waterfall_t* wf, int block, int sub, const uint8_t* db8, int keep_other)
{
    const int nb = wf->num_bins;
    const int ngroups = (nb + NIB_GROUP - 1) / NIB_GROUP;
    int med[(512 + NIB_GROUP - 1) / NIB_GROUP];
    if (nb > 512) return;
    for (int g = 0; g < ngroups; ++g)
    {
        int n = (g == ngroups - 1) ? nb - g * NIB_GROUP : NIB_GROUP;
        med[g] = median16(&db8[g * NIB_GROUP], n);
    }
    WF_ELEM_T* row = wf->mag + (size_t)(sub >> 1) * wf->max_blocks * wf->block_stride + (size_t)block * wf->block_stride;
    const int shift = (sub & 1) * 4;
    const int other_shift = 4 - shift;
    for (int b = 0; b < nb; ++b)
    {
        // Linear interpolation of the noise floor between group centers (in 1/16 bin steps)
        int pos = b - NIB_GROUP / 2;
        int base;
        if (pos <= 0 || ngroups == 1)
            base = med[0] * NIB_GROUP;
        else
        {
            int g = pos / NIB_GROUP;
            int frac = pos % NIB_GROUP;
            if (g >= ngroups - 1)
                base = med[ngroups - 1] * NIB_GROUP;
            else
                base = med[g] * (NIB_GROUP - frac) + med[g + 1] * frac;
        }
        // Nearest companded level: thresholds halfway between ftx_nib_level[] entries
        int d2 = db8[b] * NIB_GROUP * 2 - base * 2; // 1/32 half-dB units
        int q = 0;
        while (q < 15 && d2 > (ftx_nib_level[q] + ftx_nib_level[q + 1]) * NIB_GROUP) ++q;
        uint8_t other = keep_other ? ((wf_read_u8(&row[b]) >> other_shift) & 0x0F) : 4;   // 4 = noise-floor level
        wf_write_u8(&row[b], (uint8_t)((q << shift) | (other << other_shift)));
    }
}

int monitor_row_level(const ftx_waterfall_t* wf, int block, int bin)
{
    // 8-bit view (0.5 dB units, noise floor near 100) of sub-plane 0, for waterfall displays
    const WF_ELEM_T* row = wf->mag + (size_t)block * wf->block_stride;
    return 100 + ftx_nib_level[wf_read_u8(&row[bin]) & 0x0F];
}

// Windowed FFT of me->nfft samples in buf (nfft+2 floats, in place); bins min..max -> db8
static void fft_row(monitor_t* me, float* buf, uint8_t* db8)
{
    kiss_fftr(me->fft_cfg, buf, (kiss_fft_cpx*)buf);
    const kiss_fft_cpx* fd = (const kiss_fft_cpx*)buf;
    for (int bin = me->min_bin, i = 0; bin < me->max_bin; ++bin, ++i)
        db8[i] = db_to_u8(me, fd[bin].r * fd[bin].r + fd[bin].i * fd[bin].i);
}

static inline float win(monitor_t* me, int pos)
{
    return me->fft_norm * hann_i(pos, me->nfft);
}

// Process one block (block_size new samples); frame must hold block_size + 2 floats and is
// used as FFT work space.
void monitor_process(monitor_t* me, const float* frame)
{
    if (!me || !me->wf.mag || !me->fft_cfg || !me->row_db8)
        return;
    if (me->wf.num_blocks >= me->wf.max_blocks)
        return;

    const int n = me->nfft;
    const int half = n / 2;
    const int blk = me->wf.num_blocks;
    float* work = (float*)frame;
    uint8_t* db8 = me->row_db8;   // ends up holding sub-plane 0 of this block

    if (me->wf.time_osr == 2 && me->half_buf)
    {
        // Windows: sub 1 of the previous row = [prev second half | cur first half] (half a symbol
        // after it starts), sub 0 of this row = [cur first half | cur second half].
        uint32_t* prev = me->half_buf + me->half_idx * half;
        uint32_t* next = me->half_buf + (1 - me->half_idx) * half;
        uint32_t* cur0 = me->half_buf + 2 * half;
        for (int pos = 0; pos < half; ++pos)
        {
            half_put(cur0, pos, work[pos]);
            half_put(next, pos, work[half + pos]);
        }
        if (blk > 0)
        {
            for (int pos = 0; pos < half; ++pos)
            {
                work[pos] = half_get(prev, pos) * win(me, pos);
                work[half + pos] = half_get(cur0, pos) * win(me, half + pos);
            }
            fft_row(me, work, db8);
            monitor_store_row(&me->wf, blk - 1, 1, db8, 1);
        }
        for (int pos = 0; pos < half; ++pos)
        {
            work[pos] = half_get(cur0, pos) * win(me, pos);
            work[half + pos] = half_get(next, pos) * win(me, half + pos);
        }
        me->half_idx = 1 - me->half_idx;
        fft_row(me, work, db8);
        monitor_store_row(&me->wf, blk, 0, db8, 0);
    }
    else if (me->wf.freq_osr == 2 && me->last_frame && me->aux_bins)
    {
        // Half-bin shifted spectrum X(k+1/2) = FFT(xw*cos)(k) - j*FFT(xw*sin)(k), xw = windowed frame
        float* lf = me->last_frame;
        const float ph = (float)M_PI / n;
        for (int pos = 0; pos < n; ++pos)
        {
            float xw = work[pos] * win(me, pos);
            work[pos] = xw;
            lf[pos] = xw * sinf(ph * pos);
        }
        kiss_fftr(me->fft_cfg, lf, (kiss_fft_cpx*)lf);   // B = FFT(xw*sin)
        const kiss_fft_cpx* fb = (const kiss_fft_cpx*)lf;
        for (int bin = me->min_bin, i = 0; bin < me->max_bin; ++bin, ++i)
        {
            me->aux_bins[2 * i] = fb[bin].r;
            me->aux_bins[2 * i + 1] = fb[bin].i;
        }
        for (int pos = 0; pos < n; ++pos)
            lf[pos] = work[pos] * cosf(ph * pos);
        kiss_fftr(me->fft_cfg, lf, (kiss_fft_cpx*)lf);   // A = FFT(xw*cos)
        const kiss_fft_cpx* fa = (const kiss_fft_cpx*)lf;
        for (int bin = me->min_bin, i = 0; bin < me->max_bin; ++bin, ++i)
        {
            float re = fa[bin].r + me->aux_bins[2 * i + 1];
            float im = fa[bin].i - me->aux_bins[2 * i];
            db8[i] = db_to_u8(me, re * re + im * im);
        }
        monitor_store_row(&me->wf, blk, 1, db8, 0);
        fft_row(me, work, db8);
        monitor_store_row(&me->wf, blk, 0, db8, 1);
    }
    else
    {
        for (int pos = 0; pos < n; ++pos)
            work[pos] *= win(me, pos);
        fft_row(me, work, db8);
        monitor_store_row(&me->wf, blk, 0, db8, 0);
    }

    ++me->wf.num_blocks;
}
#else
// 8-bit magnitudes, time_osr = freq_osr = 1 only
void monitor_process(monitor_t* me, const float* frame)
{
    if (!me || !me->wf.mag || !me->fft_cfg)
        return;
    if (me->wf.num_blocks >= me->wf.max_blocks)
        return;
    float* work = (float*)frame;
    for (int pos = 0; pos < me->nfft; ++pos)
        work[pos] *= me->fft_norm * hann_i(pos, me->nfft);
    kiss_fftr(me->fft_cfg, work, (kiss_fft_cpx*)work);
    const kiss_fft_cpx* fd = (const kiss_fft_cpx*)work;
    WF_ELEM_T* dst = &me->wf.mag[me->wf.num_blocks * me->wf.block_stride];
    for (int bin = me->min_bin, i = 0; bin < me->max_bin; ++bin, ++i)
        wf_write_u8(&dst[i], db_to_u8(me, fd[bin].r * fd[bin].r + fd[bin].i * fd[bin].i));
    ++me->wf.num_blocks;
}
#endif

#ifdef WATERFALL_USE_PHASE
void monitor_resynth(const monitor_t* me, const candidate_t* candidate, float* signal)
{
    const int num_ifft = me->nifft;
    const int num_shift = num_ifft / 2;
    const int taper_width = 4;
    const int num_tones = 8;

    // Starting offset is 3 subblocks due to analysis buffer loading
    int offset = 1;                          // candidate->time_offset;
    offset = (offset * me->wf.time_osr) + 1; // + candidate->time_sub;
    offset = (offset * me->wf.freq_osr);     // + candidate->freq_sub;
    offset = (offset * me->wf.num_bins);     // + candidate->freq_offset;

    WF_ELEM_T* el = me->wf.mag + offset;

    // DFT frequency data - initialize to zero
    kiss_fft_cpx freqdata[num_ifft];
    for (int i = 0; i < num_ifft; ++i)
    {
        freqdata[i].r = 0;
        freqdata[i].i = 0;
    }

    int pos = 0;
    for (int num_block = 1; num_block < me->wf.num_blocks; ++num_block)
    {
        // Extract frequency data around the selected candidate only
        for (int i = candidate->freq_offset - taper_width - 1; i < candidate->freq_offset + 8 + taper_width - 1; ++i)
        {
            if ((i >= 0) && (i < me->wf.num_bins))
            {
                int tgt_bin = (me->wf.freq_osr * (i - candidate->freq_offset) + num_ifft) % num_ifft;
                float weight = 1.0f;
                if (i < candidate->freq_offset)
                {
                    weight = ((i - candidate->freq_offset) + taper_width) / (float)taper_width;
                }
                else if (i > candidate->freq_offset + 7)
                {
                    weight = ((candidate->freq_offset + 7 - i) + taper_width) / (float)taper_width;
                }

                // Convert (dB magnitude, phase) to (real, imaginary)
                float mag = powf(10.0f, el[i].mag / 20) / 2 * weight;
                freqdata[tgt_bin].r = mag * cosf(el[i].phase);
                freqdata[tgt_bin].i = mag * sinf(el[i].phase);

                int i2 = i + me->wf.num_bins;
                tgt_bin = (tgt_bin + 1) % num_ifft;
                float mag2 = powf(10.0f, el[i2].mag / 20) / 2 * weight;
                freqdata[tgt_bin].r = mag2 * cosf(el[i2].phase);
                freqdata[tgt_bin].i = mag2 * sinf(el[i2].phase);
            }
        }

        // Compute inverse DFT and overlap-add the waveform
        kiss_fft_cpx timedata[num_ifft];
        kiss_fft(me->ifft_cfg, freqdata, timedata);
        for (int i = 0; i < num_ifft; ++i)
        {
            signal[pos + i] += timedata[i].i;
        }

        // Move to the next symbol
        el += me->wf.block_stride;
        pos += num_shift;
    }
}
#endif
