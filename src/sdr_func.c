/*---------------------------------------------------------------------------*/
/**
 *  Pocket SDR C Library - Fundamental GNSS SDR Functions.
 *
 *  Author:
 *  T.TAKASU
 *
 *  History:
 *  2022-01-15  1.0  new
 *
 */
#include <math.h>
#include <fftw3.h>
#include "pocket.h"

#ifdef AVX2
#include <immintrin.h>
#endif

/* constants ----------------------------------------------------------------*/
#define NTBL     256        /* carrier lookup table size */

/* global variables ---------------------------------------------------------*/
static float cos_tbl[NTBL] = {0};
static float sin_tbl[NTBL] = {0};
static fftwf_plan plan[2] = {0};
static int N_plan = 0;

/* initialize library --------------------------------------------------------*/
void init_sdr_func(void)
{
    int i;
    
    for (i = 0; i < NTBL; i++) {
        cos_tbl[i] = cosf(-2.0f * (float)PI * i / NTBL);
        sin_tbl[i] = sinf(-2.0f * (float)PI * i / NTBL);
    }
}

/* mix carrier (N = 2 * n) ---------------------------------------------------*/
void mix_carr(const float *data, int ix, int N, double fs, double fc,
              double phi, float *data_carr)
{
    double p, step = fc / fs / 2.0 * NTBL;
    int i, j, k, m;
    
    phi = fmod(phi, 1.0) * NTBL;
    
    for (i = 0, j = ix * 2; i < N * 2; i += 4, j += 4) {
        p = phi + step * i;
        k = (uint8_t)p;
        m = (uint8_t)(p + step * 2.0);
        data_carr[i  ] = data[j  ] * cos_tbl[k] - data[j+1] * sin_tbl[k];
        data_carr[i+1] = data[j  ] * sin_tbl[k] + data[j+1] * cos_tbl[k];
        data_carr[i+2] = data[j+2] * cos_tbl[m] - data[j+3] * sin_tbl[m];
        data_carr[i+3] = data[j+2] * sin_tbl[m] + data[j+3] * cos_tbl[m];
    }
}

/* inner product of complex and real -----------------------------------------*/
void dot_cpx_real(const float *a, const float *b, int N, float s, float *c)
{
#ifndef AVX2
    int i;
    
    c[0] = c[1] = 0.0f;
    
    for (i = 0; i < N * 2; i += 2) {
        c[0] += a[i  ] * b[i];
        c[1] += a[i+1] * b[i];
    }
    c[0] *= s;
    c[1] *= s;
#else
    __m256 ymm1, ymm2, ymm3, ymm4, ymm5, ymm6, ymm7;
    float d[8], e[8];
    int i;
    
    ymm1 = _mm256_setzero_ps();
    ymm2 = _mm256_setzero_ps();
    
    for (i = 0; i < N * 2 - 15; i += 16) {
        ymm3 = _mm256_loadu_ps(a + i);
        ymm4 = _mm256_loadu_ps(a + i + 8);
        ymm5 = _mm256_shuffle_ps(ymm3, ymm4, 0x88); /* a.real */
        ymm6 = _mm256_shuffle_ps(ymm3, ymm4, 0xDD); /* a.imag */
        ymm3 = _mm256_loadu_ps(b + i);
        ymm4 = _mm256_loadu_ps(b + i + 8);
        ymm7 = _mm256_shuffle_ps(ymm3, ymm4, 0x88); /* b.real */
        ymm1 = _mm256_fmadd_ps(ymm5, ymm7, ymm1);   /* c.real */
        ymm2 = _mm256_fmadd_ps(ymm6, ymm7, ymm2);   /* c.imag */
    }
    _mm256_storeu_ps(d, ymm1);
    _mm256_storeu_ps(e, ymm2);
    c[0] = (d[0] + d[1] + d[2] + d[3] + d[4] + d[5] + d[6] + d[7]) * s;
    c[1] = (e[0] + e[1] + e[2] + e[3] + e[4] + e[5] + e[6] + e[7]) * s;
    
    for ( ; i < N * 2; i += 2) {
        c[0] += a[i  ] * b[i] * s;
        c[1] += a[i+1] * b[i] * s;
    }
#endif /* AVX2 */
}

/* standard correlator ------------------------------------------------------*/
void corr_std(const float *data, const float *code, int N, const int *pos,
              int n, float *corr)
{
    int i, j, M;
    
    for (i = j = 0; i < n; i++, j += 2) {
        if (pos[i] > 0) {
            M = N - pos[i];
            dot_cpx_real(data + pos[i] * 2, code, M, 1.0f / M, corr + j);
        }
        else if (pos[i] < 0) {
            M = N + pos[i];
            dot_cpx_real(data, code - pos[i] * 2, M, 1.0f / M, corr + j);
        }
        else {
            dot_cpx_real(data, code, N, 1.0f / N, corr + j);
        }
    }
}

/* multiplication of complex64 (N = 2 * n) ----------------------------------*/
void mul_cpx(const float *a, const float *b, int N, float s, float *c)
{
    int i;
    
    for (i = 0; i < N * 2; i += 4) {
        c[i  ] = (a[i  ] * b[i  ] - a[i+1] * b[i+1]) * s;
        c[i+1] = (a[i  ] * b[i+1] + a[i+1] * b[i  ]) * s;
        c[i+2] = (a[i+2] * b[i+2] - a[i+3] * b[i+3]) * s;
        c[i+3] = (a[i+2] * b[i+3] + a[i+3] * b[i+2]) * s;
    }
}

/* FFT correlator (N = 2 * n) -----------------------------------------------*/
void corr_fft(const float *data, const float *code_fft, int N, float *corr)
{
    fftwf_complex *c1, *c2;
    
    c1 = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * N);
    c2 = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * N);
    
    /* generate FFTW plan */
    if (N != N_plan) {
        if (plan[0]) {
            fftwf_destroy_plan(plan[0]);
            fftwf_destroy_plan(plan[1]);
        }
        plan[0] = fftwf_plan_dft_1d(N, c1, c2, FFTW_FORWARD,  FFTW_ESTIMATE);
        plan[1] = fftwf_plan_dft_1d(N, c2, c1, FFTW_BACKWARD, FFTW_ESTIMATE);
        N_plan = N;
    }
    /* ifft(fft(data) * code_fft) / N^2 */
    fftwf_execute_dft(plan[0], (fftwf_complex *)data, c1);
    mul_cpx((const float *)c1, code_fft, N, 1.0f / (N * N), (float *)c2);
    fftwf_execute_dft(plan[1], c2, (fftwf_complex *)corr);
    
    fftwf_free(c1);
    fftwf_free(c2);
}
