// The ESP32 FPU has no divide instruction: fast-math lets GCC inline reciprocal-based division
// for fast_tanh/fast_atanh instead of calling the libgcc soft-float routine.
#pragma GCC optimize("O2", "fast-math")
//
// LDPC decoder for FT8.
//
// given a 174-bit codeword as an array of log-likelihood of zero,
// return a 174-bit corrected codeword, or zero-length array.
// last 87 bits are the (systematic) plain-text.
// this is an implementation of the sum-product algorithm
// from Sarah Johnson's Iterative Error Correction book.
// codeword[i] = log ( P(x=0) / P(x=1) )
//

#include "ldpc.h"
#include "constants.h"

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <stdbool.h>

static int ldpc_check(uint8_t codeword[]);
static float fast_tanh(float x);
static float fast_atanh(float x);

// codeword is 174 log-likelihoods.
// plain is a return value, 174 ints, to be 0 or 1.
// max_iters is how hard to try.
// ok == 87 means success.
void ldpc_decode(float codeword[], int max_iters, uint8_t plain[], int* ok)
{
    float m[FTX_LDPC_M][FTX_LDPC_N]; // ~60 kB
    float e[FTX_LDPC_M][FTX_LDPC_N]; // ~60 kB
    int min_errors = FTX_LDPC_M;

    for (int j = 0; j < FTX_LDPC_M; j++)
    {
        for (int i = 0; i < FTX_LDPC_N; i++)
        {
            m[j][i] = codeword[i];
            e[j][i] = 0.0f;
        }
    }

    for (int iter = 0; iter < max_iters; iter++)
    {
        for (int j = 0; j < FTX_LDPC_M; j++)
        {
            for (int ii1 = 0; ii1 < kFTX_LDPC_Num_rows[j]; ii1++)
            {
                int i1 = kFTX_LDPC_Nm[j][ii1] - 1;
                float a = 1.0f;
                for (int ii2 = 0; ii2 < kFTX_LDPC_Num_rows[j]; ii2++)
                {
                    int i2 = kFTX_LDPC_Nm[j][ii2] - 1;
                    if (i2 != i1)
                    {
                        a *= fast_tanh(-m[j][i2] / 2.0f);
                    }
                }
                e[j][i1] = -2.0f * fast_atanh(a);
            }
        }

        for (int i = 0; i < FTX_LDPC_N; i++)
        {
            float l = codeword[i];
            for (int j = 0; j < 3; j++)
                l += e[kFTX_LDPC_Mn[i][j] - 1][i];
            plain[i] = (l > 0) ? 1 : 0;
        }

        int errors = ldpc_check(plain);

        if (errors < min_errors)
        {
            // Update the current best result
            min_errors = errors;

            if (errors == 0)
            {
                break; // Found a perfect answer
            }
        }

        for (int i = 0; i < FTX_LDPC_N; i++)
        {
            for (int ji1 = 0; ji1 < 3; ji1++)
            {
                int j1 = kFTX_LDPC_Mn[i][ji1] - 1;
                float l = codeword[i];
                for (int ji2 = 0; ji2 < 3; ji2++)
                {
                    if (ji1 != ji2)
                    {
                        int j2 = kFTX_LDPC_Mn[i][ji2] - 1;
                        l += e[j2][i];
                    }
                }
                m[j1][i] = l;
            }
        }
    }

    *ok = min_errors;
}

//
// does a 174-bit codeword pass the FT8's LDPC parity checks?
// returns the number of parity errors.
// 0 means total success.
//
static int ldpc_check(uint8_t codeword[])
{
    int errors = 0;

    for (int m = 0; m < FTX_LDPC_M; ++m)
    {
        uint8_t x = 0;
        for (int i = 0; i < kFTX_LDPC_Num_rows[m]; ++i)
        {
            x ^= codeword[kFTX_LDPC_Nm[m][i] - 1];
        }
        if (x != 0)
        {
            ++errors;
        }
    }
    return errors;
}


// Parity syndrome of codeword (1 = failed check); returns the number of failed checks
static int syndrome(const uint8_t codeword[], uint8_t synd[])
{
    int errors = 0;
    for (int m = 0; m < FTX_LDPC_M; ++m)
    {
        uint8_t x = 0;
        for (int i = 0; i < kFTX_LDPC_Num_rows[m]; ++i)
            x ^= codeword[kFTX_LDPC_Nm[m][i] - 1];
        synd[m] = x;
        errors += x;
    }
    return errors;
}

// Change in failed checks if bit v is flipped
static inline int flip_delta(const uint8_t synd[], int v)
{
    int d = 0;
    for (int j = 0; j < 3; ++j)
        d += synd[kFTX_LDPC_Mn[v][j] - 1] ? -1 : 1;
    return d;
}

static inline void flip_synd(uint8_t synd[], int v)
{
    for (int j = 0; j < 3; ++j)
        synd[kFTX_LDPC_Mn[v][j] - 1] ^= 1;
}

extern float g_ft8_dspFrame[];

void bp_decode(float codeword[], int max_iters, uint8_t plain[], int* ok)
{
    // Flooding belief propagation. tov[n][j]: message from check kFTX_LDPC_Mn[n][j] to bit n.
    // Each iteration takes the bit totals once (also the hard decision), then per check derives
    // every bit->check message as total - own message and every check->bit message from
    // prefix/suffix products of the others: the same schedule as the textbook loops, ~3x cheaper.
    float (*tov)[3] = (float (*)[3])g_ft8_dspFrame;
    float* tot = g_ft8_dspFrame + 522;
    uint8_t best_plain[FTX_LDPC_N];

    int min_errors = FTX_LDPC_M;

    // initialize message data
    for (int n = 0; n < FTX_LDPC_N; ++n)
    {
        tov[n][0] = tov[n][1] = tov[n][2] = 0;
        best_plain[n] = 0;
    }

    for (int iter = 0; iter < max_iters; ++iter)
    {
        // Do a hard decision guess (tov=0 in iter 0)
        int plain_sum = 0;
        for (int n = 0; n < FTX_LDPC_N; ++n)
        {
            tot[n] = codeword[n] + tov[n][0] + tov[n][1] + tov[n][2];
            plain[n] = (tot[n] > 0) ? 1 : 0;
            plain_sum += plain[n];
        }

        if (plain_sum == 0)
        {
            // message converged to all-zeros, which is prohibited
            break;
        }

        // Check to see if we have a codeword (check before we do any iter)
        int errors = ldpc_check(plain);

        if (errors == 0 || (plain_sum >= 60 && errors < min_errors))
        {
            // we have a better guess - save best_plain!
            min_errors = errors;
            for (int n = 0; n < FTX_LDPC_N; ++n)
            {
                best_plain[n] = plain[n];
            }

            if (errors == 0)
            {
                break; // Found a perfect answer
            }
        }

        // Fast early exit on pure noise candidates so all 100 candidates finish within 500 ms
        if (iter == 12 && min_errors > 28)
        {
            break;
        }
        if (iter == 20 && min_errors > 16)
        {
            break;
        }

        for (int m = 0; m < FTX_LDPC_M; ++m)
        {
            const int k = kFTX_LDPC_Num_rows[m];
            const uint8_t* nm = kFTX_LDPC_Nm[m];
            const uint8_t* slot = kFTX_LDPC_Nm_slot[m];
            float t[7], suf[8];
            for (int i = 0; i < k; ++i)
            {
                int n = nm[i] - 1;
                t[i] = fast_tanh(-(tot[n] - tov[n][slot[i]]) / 2);
            }
            suf[k] = 1.0f;
            for (int i = k - 1; i >= 0; --i)
                suf[i] = suf[i + 1] * t[i];
            float pre = 1.0f;
            for (int i = 0; i < k; ++i)
            {
                int n = nm[i] - 1;
                tov[n][slot[i]] = -2 * fast_atanh(pre * suf[i + 1]);
                pre *= t[i];
            }
        }
    }

    // Restore the best codeword found across all BP iterations, then try 1- and 2-bit flips of
    // the bits in failing parity checks. Flipping bit v toggles only its 3 checks, so each trial
    // updates the error count from the syndrome instead of re-running ldpc_check().
    if (min_errors > 0 && min_errors < FTX_LDPC_M)
    {
        uint8_t synd[FTX_LDPC_M];
        for (int pass = 0; pass < 3 && min_errors > 0 && min_errors <= 6; ++pass)
        {
            int prev_errors = min_errors;
            int base = syndrome(best_plain, synd);

            // Collect candidate variable nodes participating in currently failed parity checks
            uint8_t cand_vars[48];
            int num_cand_vars = 0;
            for (int m = 0; m < FTX_LDPC_M; ++m)
            {
                if (!synd[m])
                    continue;
                for (int i = 0; i < kFTX_LDPC_Num_rows[m]; ++i)
                {
                    uint8_t v = kFTX_LDPC_Nm[m][i] - 1;
                    bool seen = false;
                    for (int k = 0; k < num_cand_vars; ++k)
                    {
                        if (cand_vars[k] == v) { seen = true; break; }
                    }
                    if (!seen && num_cand_vars < 48)
                        cand_vars[num_cand_vars++] = v;
                }
            }

            // Try 1-bit flips among involved variable nodes
            int best_v1 = -1;
            for (int i = 0; i < num_cand_vars; ++i)
            {
                int e = base + flip_delta(synd, cand_vars[i]);
                if (e < min_errors)
                {
                    min_errors = e;
                    best_v1 = cand_vars[i];
                    if (min_errors == 0) break;
                }
            }
            if (best_v1 >= 0)
                best_plain[best_v1] ^= 1;
            if (min_errors == 0) break;

            // Try 2-bit flips among involved variable nodes on top of the best word so far
            if (best_v1 >= 0)
                base = syndrome(best_plain, synd);
            int b1 = -1, b2 = -1;
            for (int i = 0; i < num_cand_vars && min_errors > 0; ++i)
            {
                uint8_t v1 = cand_vars[i];
                int e1 = base + flip_delta(synd, v1);
                flip_synd(synd, v1);
                for (int j = i + 1; j < num_cand_vars; ++j)
                {
                    int e = e1 + flip_delta(synd, cand_vars[j]);
                    if (e < min_errors)
                    {
                        min_errors = e;
                        b1 = v1;
                        b2 = cand_vars[j];
                        if (min_errors == 0) break;
                    }
                }
                flip_synd(synd, v1);
            }
            if (b1 >= 0)
            {
                best_plain[b1] ^= 1;
                best_plain[b2] ^= 1;
            }

            if (min_errors >= prev_errors) break;
        }

        for (int n = 0; n < FTX_LDPC_N; ++n)
            plain[n] = best_plain[n];
    }

    *ok = min_errors;
}


// Ideas for approximating tanh/atanh:
// * https://varietyofsound.wordpress.com/2011/02/14/efficient-tanh-computation-using-lamberts-continued-fraction/
// * http://functions.wolfram.com/ElementaryFunctions/ArcTanh/10/0001/
// * https://mathr.co.uk/blog/2017-09-06_approximating_hyperbolic_tangent.html
// * https://math.stackexchange.com/a/446411

static float fast_tanh(float x)
{
    if (x < -4.97f)
    {
        return -1.0f;
    }
    if (x > 4.97f)
    {
        return 1.0f;
    }
    float x2 = x * x;
    // float a = x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2)));
    // float b = 135135.0f + x2 * (62370.0f + x2 * (3150.0f + x2 * 28.0f));
    // float a = x * (10395.0f + x2 * (1260.0f + x2 * 21.0f));
    // float b = 10395.0f + x2 * (4725.0f + x2 * (210.0f + x2));
    float a = x * (945.0f + x2 * (105.0f + x2));
    float b = 945.0f + x2 * (420.0f + x2 * 15.0f);
    return a / b;
}

static float fast_atanh(float x)
{
    float x2 = x * x;
    // float a = x * (-15015.0f + x2 * (19250.0f + x2 * (-5943.0f + x2 * 256.0f)));
    // float b = (-15015.0f + x2 * (24255.0f + x2 * (-11025.0f + x2 * 1225.0f)));
    // float a = x * (-1155.0f + x2 * (1190.0f + x2 * -231.0f));
    // float b = (-1155.0f + x2 * (1575.0f + x2 * (-525.0f + x2 * 25.0f)));
    float a = x * (945.0f + x2 * (-735.0f + x2 * 64.0f));
    float b = (945.0f + x2 * (-1050.0f + x2 * 225.0f));
    return a / b;
}
