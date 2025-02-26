// Copyright 2011 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the COPYING file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS. All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
// -----------------------------------------------------------------------------
//
//   frame coding and analysis
//
// Author: Skal (pascal.massimino@gmail.com)
#include <hls_stream.h>
#include <ap_int.h>
#include "../../src_syn/vp8_hls_syn.h"

#include <string.h>
#include <math.h>
#include <stdio.h>

#include "./cost.h"
#include "./vp8enci.h"
#include "../dsp/dsp.h"
#include "../webp/format_constants.h"  // RIFF constants
#include "../utils/profiling.h"
#include "../../examples/create_kernel.h"

// #include "sys/time.h"

//#include "../../examples/my_syn.h"

#define SEGMENT_VISU 0
#define DEBUG_SEARCH 0    // useful to track search convergence
#define DEBUG_PROBAS 0

#include <stdio.h>

void debug_probas(const VP8EncProba* const probas) {
#if DEBUG_PROBAS
    printf("simple variable\n");
    printf("%d %d %d %d %d %d %d\n",
           probas->segments_[0],
           probas->segments_[1],
           probas->segments_[2],
           probas->skip_proba_,
           probas->dirty_,
           probas->use_skip_proba_,
           probas->nb_skip_);
    printf("coeffs_===========================\n");
    int t, b, c, p;
    for (t = 0; t < NUM_TYPES; ++t) {
        for (b = 0; b < NUM_BANDS; ++b) {
            for (c = 0; c < NUM_CTX; ++c) {
                for (p = 0; p < NUM_PROBAS; ++p) {
                    const uint8_t p0 = probas->coeffs_[t][b][c][p];
                    printf("coeffs_[%d][%d][%d][%d]:%d ",
                           t, b, c, p, p0);
                }
                printf("\n");
            }
        }
    }
    printf("stats===============================\n");
    for (t = 0; t < NUM_TYPES; ++t) {
        for (b = 0; b < NUM_BANDS; ++b) {
            for (c = 0; c < NUM_CTX; ++c) {
                for (p = 0; p < NUM_PROBAS; ++p) {
                    // const proba_t stats = proba->stats_[t][b][c][p];
                    //uint32_t stats_p[NUM_TYPES * NUM_BANDS * NUM_CTX * NUM_PROBAS];
                    const proba_t stats = probas->stats_[t][b][c][p];
                    printf("Stats[%d][%d][%d][%d]:%d ",
                           t, b, c, p, stats);
                }
                printf("\n");
            }
        }
    }
#endif
}

void debug_log_stats_p(
    VP8EncProba *proba
    // VP8EncLoopPointer* proba
    ) {
    int t, b, c, p;
    for (t = 0; t < NUM_TYPES; ++t) {
        for (b = 0; b < NUM_BANDS; ++b) {
            for (c = 0; c < NUM_CTX; ++c) {
                for (p = 0; p < NUM_PROBAS; ++p) {
                    // const proba_t stats = proba->stats_[t][b][c][p];
                    //uint32_t stats_p[NUM_TYPES * NUM_BANDS * NUM_CTX * NUM_PROBAS];
                    const proba_t stats = proba->stats_[t][b][c][p];
                    printf("Stats[%d][%d][%d][%d]=%d\n",
                           t, b, c, p, stats);
                }
            }
        }
    }
}

//------------------------------------------------------------------------------
// multi-pass convergence

#define HEADER_SIZE_ESTIMATE (RIFF_HEADER_SIZE + CHUNK_HEADER_SIZE +    \
                              VP8_FRAME_HEADER_SIZE)
#define DQ_LIMIT 0.4  // convergence is considered reached if dq < DQ_LIMIT
// we allow 2k of extra head-room in PARTITION0 limit.
#define PARTITION0_SIZE_LIMIT ((VP8_MAX_PARTITION0_SIZE - 2048ULL) << 11)

typedef struct {  // struct for organizing convergence in either size or PSNR
    int is_first;
    float dq;
    float q, last_q;
    double value, last_value;   // PSNR or size
    double target;
    int do_size_search;
} PassStats;

static int InitPassStats(const VP8Encoder* const enc, PassStats* const s) {
    const uint64_t target_size = (uint64_t)enc->config_->target_size;
    const int do_size_search = (target_size != 0);
    const float target_PSNR = enc->config_->target_PSNR;

    s->is_first = 1;
    s->dq = 10.f;
    s->q = s->last_q = enc->config_->quality;
    s->target = do_size_search ? (double)target_size
        : (target_PSNR > 0.) ? target_PSNR
        : 40.;   // default, just in case
    s->value = s->last_value = 0.;
    s->do_size_search = do_size_search;
    return do_size_search;
}

static float Clamp(float v, float min, float max) {
    return (v < min) ? min : (v > max) ? max : v;
}

static float ComputeNextQ(PassStats* const s) {
    float dq;
    if (s->is_first) {
        dq = (s->value > s->target) ? -s->dq : s->dq;
        s->is_first = 0;
    } else if (s->value != s->last_value) {
        const double slope = (s->target - s->value) / (s->last_value - s->value);
        dq = (float)(slope * (s->last_q - s->q));
    } else {
        dq = 0.;  // we're done?!
    }
    // Limit variable to avoid large swings.
    s->dq = Clamp(dq, -30.f, 30.f);
    s->last_q = s->q;
    s->last_value = s->value;
    s->q = Clamp(s->q + s->dq, 0.f, 100.f);
    return s->q;
}

//------------------------------------------------------------------------------
// Tables for level coding

const uint8_t VP8Cat3[] = { 173, 148, 140 };
const uint8_t VP8Cat4[] = { 176, 155, 140, 135 };
const uint8_t VP8Cat5[] = { 180, 157, 141, 134, 130 };
const uint8_t VP8Cat6[] =
{ 254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129 };

//------------------------------------------------------------------------------
// Reset the statistics about: number of skips, token proba, level cost,...

static void ResetStats(VP8Encoder* const enc) {
    VP8EncProba* const proba = &enc->proba_;
    //VP8CalculateLevelCosts(proba);
    proba->nb_skip_ = 0;
}

//------------------------------------------------------------------------------
// Skip decision probability

#define SKIP_PROBA_THRESHOLD 250  // value below which using skip_proba is OK.

static int CalcSkipProba(uint64_t nb, uint64_t total) {
    return (int)(total ? (total - nb) * 255 / total : 255);
}

// Returns the bit-cost for coding the skip probability.
static int FinalizeSkipProba(VP8Encoder* const enc) {
    VP8EncProba* const proba = &enc->proba_;
    const int nb_mbs = enc->mb_w_ * enc->mb_h_;
    const int nb_events = proba->nb_skip_;
    int size;
    proba->skip_proba_ = CalcSkipProba(nb_events, nb_mbs);
    proba->use_skip_proba_ = (proba->skip_proba_ < SKIP_PROBA_THRESHOLD);
    size = 256;   // 'use_skip_proba' bit
    if (proba->use_skip_proba_) {
        size +=  nb_events * VP8BitCost(1, proba->skip_proba_)
            + (nb_mbs - nb_events) * VP8BitCost(0, proba->skip_proba_);
        size += 8 * 256;   // cost of signaling the skip_proba_ itself.
    }
    return size;
}

// Collect statistics and deduce probabilities for next coding pass.
// Return the total bit-cost for coding the probability updates.
static int CalcTokenProba(int nb, int total) {// in fact return value range from 0~255, only needs  8 bits
    assert(nb <= total);
    return nb ? (255 - nb * 255 / total) : 255;
}

// Cost of coding 'nb' 1's and 'total-nb' 0's using 'proba' probability.
static int BranchCost(int nb, int total, int proba) {
    return nb * VP8BitCost(1, proba) + (total - nb) * VP8BitCost(0, proba);
}

static void ResetTokenStats(VP8Encoder* const enc) {
    VP8EncProba* const proba = &enc->proba_;
    memset(proba->stats_, 0, sizeof(proba->stats_));
}

static int FinalizeTokenProbas(VP8EncProba* const proba) {
    int has_changed = 0;
    int size = 0;
    int t, b, c, p;
    for (t = 0; t < NUM_TYPES; ++t) {
        for (b = 0; b < NUM_BANDS; ++b) {
            for (c = 0; c < NUM_CTX; ++c) {
                for (p = 0; p < NUM_PROBAS; ++p) {
                    const proba_t stats = proba->stats_[t][b][c][p];
                    //wr//if(stats!=0)// printf("%s [%d][%d][%d][%d]stats:%d\n",
                    //wr//  printf("t=%d, b=%d, c=%d, p= %d, stats=%x\n",t,b,c,p,stats);//     __FUNCTION__, t, b, c, p, stats);
                    const int nb = (stats >> 0) & 0xffff;
                    const int total = (stats >> 16) & 0xffff;
                    const int update_proba = VP8CoeffsUpdateProba[t][b][c][p];
                    const int old_p = VP8CoeffsProba0[t][b][c][p];
                    const int new_p = CalcTokenProba(nb, total);
                    const int old_cost = BranchCost(nb, total, old_p)
                        + VP8BitCost(0, update_proba);
                    const int new_cost = BranchCost(nb, total, new_p)
                        + VP8BitCost(1, update_proba)
                        + 8 * 256;
                    const int use_new_p = (old_cost > new_cost);
                    // printf("%s use_new_p:%d old_cost:%d new_cost:%d\n",
                    //     __FUNCTION__, use_new_p, old_cost, new_cost);
                    size += VP8BitCost(use_new_p, update_proba);
                    if (use_new_p) {  // only use proba that seem meaningful enough.
                        proba->coeffs_[t][b][c][p] = new_p;
                        has_changed |= (new_p != old_p);
                        // printf("%s has_changed:%d new_p:%d old_p:%d\n",
                        //   __FUNCTION__, has_changed, new_p, old_p);
                        size += 8 * 256;
                    } else {
                        proba->coeffs_[t][b][c][p] = old_p;
                    }
                }
            }
        }
    }
    // printf("%d %d==========================\n", __LINE__, has_changed);
    proba->dirty_ = has_changed;
    return size;
}

//------------------------------------------------------------------------------
// Finalize Segment probability based on the coding tree

static int GetProba(int a, int b) {
    const int total = a + b;
    return (total == 0) ? 255     // that's the default probability.
        : (255 * a + total / 2) / total;  // rounded proba
}

static void SetSegmentProbas(VP8Encoder* const enc) {
    int p[NUM_MB_SEGMENTS] = { 0 };
    int n;

    for (n = 0; n < enc->mb_w_ * enc->mb_h_; ++n) {
        const VP8MBInfo* const mb = &enc->mb_info_[n];
        p[mb->segment_]++;
    }
    if (enc->pic_->stats != NULL) {
        for (n = 0; n < NUM_MB_SEGMENTS; ++n) {
            enc->pic_->stats->segment_size[n] = p[n];
        }
    }
    if (enc->segment_hdr_.num_segments_ > 1) {
        uint8_t* const probas = enc->proba_.segments_;
        probas[0] = GetProba(p[0] + p[1], p[2] + p[3]);
        probas[1] = GetProba(p[0], p[1]);
        probas[2] = GetProba(p[2], p[3]);

        enc->segment_hdr_.update_map_ =
            (probas[0] != 255) || (probas[1] != 255) || (probas[2] != 255);
        enc->segment_hdr_.size_ =
            p[0] * (VP8BitCost(0, probas[0]) + VP8BitCost(0, probas[1])) +
            p[1] * (VP8BitCost(0, probas[0]) + VP8BitCost(1, probas[1])) +
            p[2] * (VP8BitCost(1, probas[0]) + VP8BitCost(0, probas[2])) +
            p[3] * (VP8BitCost(1, probas[0]) + VP8BitCost(1, probas[2]));
    } else {
        enc->segment_hdr_.update_map_ = 0;
        enc->segment_hdr_.size_ = 0;
    }
}

//------------------------------------------------------------------------------
// Coefficient coding

static int PutCoeffs(VP8BitWriter* const bw, int ctx, const VP8Residual* res) {

    int n = res->first;
    // should be prob[VP8EncBands[n]], but it's equivalent for n=0 or 1
    const uint8_t* p = res->prob[n][ctx];
    if (!VP8PutBit(bw, res->last >= 0, p[0])) {
        return 0;
    }

    while (n < 16) {
        const int c = res->coeffs[n++];
        const int sign = c < 0;
        int v = sign ? -c : c;
        if (!VP8PutBit(bw, v != 0, p[1])) {
            p = res->prob[VP8EncBands[n]][0];
            continue;
        }
        if (!VP8PutBit(bw, v > 1, p[2])) {
            p = res->prob[VP8EncBands[n]][1];
        } else {
            if (!VP8PutBit(bw, v > 4, p[3])) {
                if (VP8PutBit(bw, v != 2, p[4]))
                    VP8PutBit(bw, v == 4, p[5]);
            } else if (!VP8PutBit(bw, v > 10, p[6])) {
                if (!VP8PutBit(bw, v > 6, p[7])) {
                    VP8PutBit(bw, v == 6, 159);
                } else {
                    VP8PutBit(bw, v >= 9, 165);
                    VP8PutBit(bw, !(v & 1), 145);
                }
            } else {
                int mask;
                const uint8_t* tab;
                if (v < 3 + (8 << 1)) {          // VP8Cat3  (3b)
                    VP8PutBit(bw, 0, p[8]);
                    VP8PutBit(bw, 0, p[9]);
                    v -= 3 + (8 << 0);
                    mask = 1 << 2;
                    tab = VP8Cat3;
                } else if (v < 3 + (8 << 2)) {   // VP8Cat4  (4b)
                    VP8PutBit(bw, 0, p[8]);
                    VP8PutBit(bw, 1, p[9]);
                    v -= 3 + (8 << 1);
                    mask = 1 << 3;
                    tab = VP8Cat4;
                } else if (v < 3 + (8 << 3)) {   // VP8Cat5  (5b)
                    VP8PutBit(bw, 1, p[8]);
                    VP8PutBit(bw, 0, p[10]);
                    v -= 3 + (8 << 2);
                    mask = 1 << 4;
                    tab = VP8Cat5;
                } else {                         // VP8Cat6 (11b)
                    VP8PutBit(bw, 1, p[8]);
                    VP8PutBit(bw, 1, p[10]);
                    v -= 3 + (8 << 3);
                    mask = 1 << 10;
                    tab = VP8Cat6;
                }
                while (mask) {
                    VP8PutBit(bw, !!(v & mask), *tab++);
                    mask >>= 1;
                }
            }
            p = res->prob[VP8EncBands[n]][2];
        }
        VP8PutBitUniform(bw, sign);
        if (n == 16 || !VP8PutBit(bw, n <= res->last, p[0])) {
            return 1;   // EOB
        }
    }
    return 1;
}

static void CodeResiduals(VP8BitWriter* const bw, VP8EncIterator* const it,
                          const VP8ModeScore* const rd) {
    int x, y, ch;
    VP8Residual res;
    uint64_t pos1, pos2, pos3;
    const int i16 = (it->mb_->type_ == 1);
    const int segment = it->mb_->segment_;
    VP8Encoder* const enc = it->enc_;
    StopProfilingWatch stop_watch;
    StartProfiling(&stop_watch);

    VP8IteratorNzToBytes(it);

    pos1 = VP8BitWriterPos(bw);
    if (i16) {
        VP8InitResidual(0, 1, enc, &res);
        VP8SetResidualCoeffs(rd->y_dc_levels, &res);
        it->top_nz_[8] = it->left_nz_[8] =
            PutCoeffs(bw, it->top_nz_[8] + it->left_nz_[8], &res);
        VP8InitResidual(1, 0, enc, &res);
    } else {
        VP8InitResidual(0, 3, enc, &res);
    }

    // luma-AC
    for (y = 0; y < 4; ++y) {
        for (x = 0; x < 4; ++x) {
            const int ctx = it->top_nz_[x] + it->left_nz_[y];
            VP8SetResidualCoeffs(rd->y_ac_levels[x + y * 4], &res);
            it->top_nz_[x] = it->left_nz_[y] = PutCoeffs(bw, ctx, &res);
        }
    }
    pos2 = VP8BitWriterPos(bw);

    // U/V
    VP8InitResidual(0, 2, enc, &res);
    for (ch = 0; ch <= 2; ch += 2) {
        for (y = 0; y < 2; ++y) {
            for (x = 0; x < 2; ++x) {
                const int ctx = it->top_nz_[4 + ch + x] + it->left_nz_[4 + ch + y];
                VP8SetResidualCoeffs(rd->uv_levels[ch * 2 + x + y * 2], &res);
                it->top_nz_[4 + ch + x] = it->left_nz_[4 + ch + y] =
                    PutCoeffs(bw, ctx, &res);
            }
        }
    }
    pos3 = VP8BitWriterPos(bw);
    it->luma_bits_ = pos2 - pos1;
    it->uv_bits_ = pos3 - pos2;
    it->bit_count_[segment][i16] += it->luma_bits_;
    it->bit_count_[segment][2] += it->uv_bits_;
    VP8IteratorBytesToNz(it);
    StopProfiling(&stop_watch, &timeCodeResiduals, &countCodeResiduals);
}

// Same as CodeResiduals, but doesn't actually write anything.
// Instead, it just records the event distribution.
static void RecordResiduals(VP8EncIterator* const it,
                            const VP8ModeScore* const rd) {
    int x, y, ch;
    VP8Residual res;
    VP8Encoder* const enc = it->enc_;

    VP8IteratorNzToBytes(it);

    if (it->mb_->type_ == 1) {   // i16x16
        VP8InitResidual(0, 1, enc, &res);
        VP8SetResidualCoeffs(rd->y_dc_levels, &res);
        it->top_nz_[8] = it->left_nz_[8] =
            VP8RecordCoeffs(it->top_nz_[8] + it->left_nz_[8], &res);
        VP8InitResidual(1, 0, enc, &res);
    } else {
        VP8InitResidual(0, 3, enc, &res);
    }

    // luma-AC
    for (y = 0; y < 4; ++y) {
        for (x = 0; x < 4; ++x) {
            const int ctx = it->top_nz_[x] + it->left_nz_[y];
            VP8SetResidualCoeffs(rd->y_ac_levels[x + y * 4], &res);
            it->top_nz_[x] = it->left_nz_[y] = VP8RecordCoeffs(ctx, &res);
        }
    }

    // U/V
    VP8InitResidual(0, 2, enc, &res);
    for (ch = 0; ch <= 2; ch += 2) {
        for (y = 0; y < 2; ++y) {
            for (x = 0; x < 2; ++x) {
                const int ctx = it->top_nz_[4 + ch + x] + it->left_nz_[4 + ch + y];
                VP8SetResidualCoeffs(rd->uv_levels[ch * 2 + x + y * 2], &res);
                it->top_nz_[4 + ch + x] = it->left_nz_[4 + ch + y] =
                    VP8RecordCoeffs(ctx, &res);
            }
        }
    }

    VP8IteratorBytesToNz(it);
}

#if !defined(DISABLE_TOKEN_BUFFER)
#if TOKEN_RECONSTRUCT
static int RecordTokens(VP8EncIterator* const it, const VP8ModeScore* const rd,
                        VP8TBufferKernel* const tokens) {
#else
    static int RecordTokens(VP8EncIterator* const it, const VP8ModeScore* const rd,
                            VP8TBuffer* const tokens) {
#endif
        int x, y, ch;
        VP8Residual res;
        VP8Encoder* const enc = it->enc_;

        VP8IteratorNzToBytes(it);
        if (it->mb_->type_ == 1) {   // i16x16
            const int ctx = it->top_nz_[8] + it->left_nz_[8];
            VP8InitResidual(0, 1, enc, &res);
            VP8SetResidualCoeffs(rd->y_dc_levels, &res);
            it->top_nz_[8] = it->left_nz_[8] =
                VP8RecordCoeffTokens(ctx, 1,
                                     res.first, res.last, res.coeffs, tokens);

            // static int i = 0;
            // printf("[%d] ctx:%d res.first:%d res.last:%d it->top_nz_[8]:%d\n",
            //       i++, ctx, res.first, res.last, it->top_nz_[8]);

            VP8RecordCoeffs(ctx, &res);
            VP8InitResidual(1, 0, enc, &res);
        } else {
            VP8InitResidual(0, 3, enc, &res);
        }

        // luma-AC
        for (y = 0; y < 4; ++y) {
            for (x = 0; x < 4; ++x) {
                const int ctx = it->top_nz_[x] + it->left_nz_[y];
                VP8SetResidualCoeffs(rd->y_ac_levels[x + y * 4], &res);
                it->top_nz_[x] = it->left_nz_[y] =
                    VP8RecordCoeffTokens(ctx, res.coeff_type,
                                         res.first, res.last, res.coeffs, tokens);
                VP8RecordCoeffs(ctx, &res);
            }
        }

        // U/V
        VP8InitResidual(0, 2, enc, &res);
        for (ch = 0; ch <= 2; ch += 2) {
            for (y = 0; y < 2; ++y) {
                for (x = 0; x < 2; ++x) {
                    const int ctx = it->top_nz_[4 + ch + x] + it->left_nz_[4 + ch + y];
                    VP8SetResidualCoeffs(rd->uv_levels[ch * 2 + x + y * 2], &res);
                    it->top_nz_[4 + ch + x] = it->left_nz_[4 + ch + y] =
                        VP8RecordCoeffTokens(ctx, 2,
                                             res.first, res.last, res.coeffs, tokens);
                    VP8RecordCoeffs(ctx, &res);
                }
            }
        }
        VP8IteratorBytesToNz(it);
        return !tokens->error_;
    }

#endif    // !DISABLE_TOKEN_BUFFER

    //------------------------------------------------------------------------------
    // ExtraInfo map / Debug function

#if SEGMENT_VISU
    static void SetBlock(uint8_t* p, int value, int size) {
        int y;
        for (y = 0; y < size; ++y) {
            memset(p, value, size);
            p += BPS;
        }
    }
#endif

    static void ResetSSE(VP8Encoder* const enc) {
        enc->sse_[0] = 0;
        enc->sse_[1] = 0;
        enc->sse_[2] = 0;
        // Note: enc->sse_[3] is managed by alpha.c
        enc->sse_count_ = 0;
    }

    static void StoreSSE(const VP8EncIterator* const it) {
        VP8Encoder* const enc = it->enc_;
        const uint8_t* const in = it->yuv_in_;
        const uint8_t* const out = it->yuv_out_;
        // Note: not totally accurate at boundary. And doesn't include in-loop filter.
        enc->sse_[0] += VP8SSE16x16(in + Y_OFF_ENC, out + Y_OFF_ENC);
        enc->sse_[1] += VP8SSE8x8(in + U_OFF_ENC, out + U_OFF_ENC);
        enc->sse_[2] += VP8SSE8x8(in + V_OFF_ENC, out + V_OFF_ENC);
        enc->sse_count_ += 16 * 16;
    }

    static void StoreSideInfo(const VP8EncIterator* const it) {
        VP8Encoder* const enc = it->enc_;
        const VP8MBInfo* const mb = it->mb_;
        WebPPicture* const pic = enc->pic_;

        if (pic->stats != NULL) {
            StoreSSE(it);
            enc->block_count_[0] += (mb->type_ == 0);
            enc->block_count_[1] += (mb->type_ == 1);
            enc->block_count_[2] += (mb->skip_ != 0);
        }

        if (pic->extra_info != NULL) {
            uint8_t* const info = &pic->extra_info[it->x_ + it->y_ * enc->mb_w_];
            switch (pic->extra_info_type) {
            case 1: *info = mb->type_; break;
            case 2: *info = mb->segment_; break;
            case 3: *info = enc->dqm_[mb->segment_].quant_; break;
            case 4: *info = (mb->type_ == 1) ? it->preds_[0] : 0xff; break;
            case 5: *info = mb->uv_mode_; break;
            case 6: {
                const int b = (int)((it->luma_bits_ + it->uv_bits_ + 7) >> 3);
                *info = (b > 255) ? 255 : b; break;
            }
            case 7: *info = mb->alpha_; break;
            default: *info = 0; break;
            }
        }
#if SEGMENT_VISU  // visualize segments and prediction modes
        SetBlock(it->yuv_out_ + Y_OFF_ENC, mb->segment_ * 64, 16);
        SetBlock(it->yuv_out_ + U_OFF_ENC, it->preds_[0] * 64, 8);
        SetBlock(it->yuv_out_ + V_OFF_ENC, mb->uv_mode_ * 64, 8);
#endif
    }

    static double GetPSNR(uint64_t mse, uint64_t size) {
        return (mse > 0 && size > 0) ? 10. * log10(255. * 255. * size / mse) : 99;
    }

    //------------------------------------------------------------------------------
    //  StatLoop(): only collect statistics (number of skips, token usage, ...).
    //  This is used for deciding optimal probabilities. It also modifies the
    //  quantizer value if some target (size, PSNR) was specified.

    static void SetLoopParams(VP8Encoder* const enc, float q) {
        // Make sure the quality parameter is inside valid bounds
        q = Clamp(q, 0.f, 100.f);

        VP8SetSegmentParams(enc, q);      // setup segment quantizations and filters
        SetSegmentProbas(enc);            // compute segment probabilities

        ResetStats(enc);
        ResetSSE(enc);
    }

    static uint64_t OneStatPass(VP8Encoder* const enc, VP8RDLevel rd_opt,
                                int nb_mbs, int percent_delta,
                                PassStats* const s) {
        VP8EncIterator it;
        uint64_t size = 0;
        uint64_t size_p0 = 0;
        uint64_t distortion = 0;
        const uint64_t pixel_count = nb_mbs * 384;

        VP8IteratorInit(enc, &it);
        SetLoopParams(enc, s->q);
        do {
            VP8ModeScore info;
            VP8IteratorImport(&it, NULL);
            if (VP8Decimate(&it, &info, rd_opt)) {
                // Just record the number of skips and act like skip_proba is not used.
                enc->proba_.nb_skip_++;
            }
            RecordResiduals(&it, &info);
            size += info.R + info.H;
            size_p0 += info.H;
            distortion += info.D;
            if (percent_delta && !VP8IteratorProgress(&it, percent_delta))
                return 0;
            VP8IteratorSaveBoundary(&it);
        } while (VP8IteratorNext(&it) && --nb_mbs > 0);

        size_p0 += enc->segment_hdr_.size_;
        if (s->do_size_search) {
            size += FinalizeSkipProba(enc);
            size += FinalizeTokenProbas(&enc->proba_);
            size = ((size + size_p0 + 1024) >> 11) + HEADER_SIZE_ESTIMATE;
            s->value = (double)size;
        } else {
            s->value = GetPSNR(distortion, pixel_count);
        }
        return size_p0;
    }

    static int StatLoop(VP8Encoder* const enc) {
        const int method = enc->method_;
        const int do_search = enc->do_search_;
        const int fast_probe = ((method == 0 || method == 3) && !do_search);
        int num_pass_left = enc->config_->pass;
        const int task_percent = 20;
        const int percent_per_pass =
            (task_percent + num_pass_left / 2) / num_pass_left;
        const int final_percent = enc->percent_ + task_percent;
        const VP8RDLevel rd_opt =
            (method >= 3 || do_search) ? RD_OPT_BASIC : RD_OPT_NONE;
        int nb_mbs = enc->mb_w_ * enc->mb_h_;
        PassStats stats;
        StopProfilingWatch stop_watch;
        StartProfiling(&stop_watch);

        InitPassStats(enc, &stats);
        ResetTokenStats(enc);

        // Fast mode: quick analysis pass over few mbs. Better than nothing.
        if (fast_probe) {
            if (method == 3) {  // we need more stats for method 3 to be reliable.
                nb_mbs = (nb_mbs > 200) ? nb_mbs >> 1 : 100;
            } else {
                nb_mbs = (nb_mbs > 200) ? nb_mbs >> 2 : 50;
            }
        }

        while (num_pass_left-- > 0) {
            const int is_last_pass = (fabs(stats.dq) <= DQ_LIMIT) ||
                (num_pass_left == 0) ||
                (enc->max_i4_header_bits_ == 0);
            const uint64_t size_p0 =
                OneStatPass(enc, rd_opt, nb_mbs, percent_per_pass, &stats);
            if (size_p0 == 0) return 0;
#if (DEBUG_SEARCH > 0)
            printf("#%d value:%.1lf -> %.1lf   q:%.2f -> %.2f\n",
                   num_pass_left, stats.last_value, stats.value, stats.last_q, stats.q);
#endif
            if (enc->max_i4_header_bits_ > 0 && size_p0 > PARTITION0_SIZE_LIMIT) {
                ++num_pass_left;
                enc->max_i4_header_bits_ >>= 1;  // strengthen header bit limitation...
                continue;                        // ...and start over
            }
            if (is_last_pass) {
                break;
            }
            // If no target size: just do several pass without changing 'q'
            if (do_search) {
                ComputeNextQ(&stats);
                if (fabs(stats.dq) <= DQ_LIMIT) break;
            }
        }
        if (!do_search || !stats.do_size_search) {
            // Need to finalize probas now, since it wasn't done during the search.
            FinalizeSkipProba(enc);
            FinalizeTokenProbas(&enc->proba_);
        }
        VP8CalculateLevelCosts(&enc->proba_);  // finalize costs
        StopProfiling(&stop_watch, &timeStatLoop, &countStatLoop);
        return WebPReportProgress(enc->pic_, final_percent, &enc->percent_);
    }

    //------------------------------------------------------------------------------
    // Main loops
    //

    static const int kAverageBytesPerMB[8] = { 50, 24, 16, 9, 7, 5, 3, 2 };

    static int PreLoopInitialize(VP8Encoder* const enc) {
        int p;
        int ok = 1;
        const int average_bytes_per_MB = kAverageBytesPerMB[enc->base_quant_ >> 4];
        const int bytes_per_parts =
            enc->mb_w_ * enc->mb_h_ * average_bytes_per_MB / enc->num_parts_;
        // Initialize the bit-writers
        for (p = 0; ok && p < enc->num_parts_; ++p) {
            ok = VP8BitWriterInit(enc->parts_ + p, bytes_per_parts);
        }
        if (!ok) {
            VP8EncFreeBitWriters(enc);  // malloc error occurred
            WebPEncodingSetError(enc->pic_, VP8_ENC_ERROR_OUT_OF_MEMORY);
        }
        return ok;
    }

    int PostLoopFinalize(VP8EncIterator* const it, int ok) {

        VP8Encoder* const enc = it->enc_;

#if 1
        if (ok) {      // Finalize the partitions, check for extra errors.
            int p;
            for (p = 0; p < enc->num_parts_; ++p) {
                VP8BitWriterFinish(enc->parts_ + p);
                ok &= !enc->parts_[p].error_;
            }
        }

        if (ok) {      // All good. Finish up.
            if (enc->pic_->stats != NULL) {  // finalize byte counters...
                int i, s;
                for (i = 0; i <= 2; ++i) {
                    for (s = 0; s < NUM_MB_SEGMENTS; ++s) {
                        enc->residual_bytes_[i][s] = (int)((it->bit_count_[s][i] + 7) >> 3);
                    }
                }
            }
            VP8AdjustFilterStrength(it);     // ...and store filter stats.
        } else {
            // Something bad happened -> need to do some memory cleanup.
            VP8EncFreeBitWriters(enc);
        }

#endif
        return ok;
    }

    static int PostLoopFinalizeOcl(VP8Encoder* const enc, uint64_t bit_count[4][3], int ok) {
        if (ok) {      // Finalize the partitions, check for extra errors.
            int p;
            for (p = 0; p < enc->num_parts_; ++p) {
                VP8BitWriterFinish(enc->parts_ + p);
                ok &= !enc->parts_[p].error_;
            }
        }

        if (ok) {      // All good. Finish up.
            if (enc->pic_->stats != NULL) {  // finalize byte counters...
                int i, s;
                for (i = 0; i <= 2; ++i) {
                    for (s = 0; s < NUM_MB_SEGMENTS; ++s) {
                        enc->residual_bytes_[i][s] = (int)((bit_count[s][i] + 7) >> 3);
                    }
                }
            }
            VP8AdjustFilterStrengthOcl(enc);     // ...and store filter stats.
        } else {
            // Something bad happened -> need to do some memory cleanup.
            VP8EncFreeBitWriters(enc);
        }
        return ok;
    }

    //------------------------------------------------------------------------------
    //  VP8EncLoop(): does the final bitstream coding.

    static void ResetAfterSkip(VP8EncIterator* const it) {
        if (it->mb_->type_ == 1) {
            *it->nz_ = 0;  // reset all predictors
            it->left_nz_[8] = 0;
        } else {
            *it->nz_ &= (1 << 24);  // preserve the dc_nz bit
        }
    }

    int VP8EncLoop(VP8Encoder* const enc) {

        VP8EncIterator it;
        int ok = PreLoopInitialize(enc);
        StopProfilingWatch stop_watch;
        StartProfiling(&stop_watch);

        if (!ok) return 0;

        StatLoop(enc);  // stats-collection loop
        StatLoopFlag = 0;

        VP8IteratorInit(enc, &it);
        VP8InitFilter(&it);
        do {
            VP8ModeScore info;
            const int dont_use_skip = !enc->proba_.use_skip_proba_;
            const VP8RDLevel rd_opt = enc->rd_opt_level_;

            VP8IteratorImport(&it, NULL);
            // Warning! order is important: first call VP8Decimate() and
            // *then* decide how to code the skip decision if there's one.
            if (!VP8Decimate(&it, &info, rd_opt) || dont_use_skip) {
                CodeResiduals(it.bw_, &it, &info);
            } else {   // reset predictors after a skip
                ResetAfterSkip(&it);
            }
            StoreSideInfo(&it);
            VP8StoreFilterStats(&it);
            VP8IteratorExport(&it);
            ok = VP8IteratorProgress(&it, 20);
            VP8IteratorSaveBoundary(&it);
        } while (ok && VP8IteratorNext(&it));

        StopProfiling(&stop_watch, &timeEncLoop, &countEncLoop);
        return PostLoopFinalize(&it, ok);
    }

    int VP8EncLoopOcl(VP8Encoder* const enc) {
        return 0;
    }

#if !defined(DISABLE_TOKEN_BUFFER)

#define MIN_COUNT 96  // minimum number of macroblocks before updating stats
 
    int VP8EncTokenLoop(VP8Encoder* const enc) {
 
        printf(" In EncTokenLoop in frame.c \n");

        // Roughly refresh the proba eight times per pass
        int max_count = (enc->mb_w_ * enc->mb_h_) >> 3;
        int num_pass_left = enc->config_->pass;
        const int do_search = 0;//enc->do_search_;
  
        VP8EncIterator it;
        VP8EncProba* const proba = &enc->proba_;
        const VP8RDLevel rd_opt = enc->rd_opt_level_;
        const uint64_t pixel_count = enc->mb_w_ * enc->mb_h_ * 384;
  
        PassStats stats;
        int ok;
        StopProfilingWatch stop_watch;
        StartProfiling(&stop_watch);

        InitPassStats(enc, &stats);
        ok = PreLoopInitialize(enc);
        if (!ok) return 0;

        if (max_count < MIN_COUNT) max_count = MIN_COUNT;

        assert(enc->num_parts_ == 1);
        assert(enc->use_tokens_);
        assert(proba->use_skip_proba_ == 0);
        assert(rd_opt >= RD_OPT_BASIC);   // otherwise, token-buffer won't be useful
        assert(num_pass_left > 0);

        while (ok && num_pass_left-- > 0) {

            const int is_last_pass = (fabs(stats.dq) <= DQ_LIMIT) ||
                (num_pass_left == 0) ||
                (enc->max_i4_header_bits_ == 0);
            uint64_t size_p0 = 0;
            uint64_t distortion = 0;
            int cnt = max_count;

            VP8IteratorInit(enc, &it);
            SetLoopParams(enc, stats.q);

            if (is_last_pass) {
                ResetTokenStats(enc);
                VP8InitFilter(&it);  // don't collect stats until last pass (too costly)
            }

            VP8TBufferClear(&enc->tokens_);

#ifdef USE_C_KERNEL
      
            int i, j, index;

            cl_int err;
            EncloopInputData input_data;
            EncloopSegmentData segment_data;
            EncLoopOutputData output_data;
            VP8TBufferKernel output_tokens;

            VP8EncMatrix matrix_y1[NUM_MB_SEGMENTS];
            VP8EncMatrix matrix_y2[NUM_MB_SEGMENTS];
            VP8EncMatrix matrix_uv[NUM_MB_SEGMENTS];

            const int xsize = enc->pic_->width;
            const int ysize = enc->pic_->height;

            const int mb_w = (xsize + 15) >> 4; // nb of blocks in x 
            const int mb_h = (ysize + 15) >> 4; // nb of blocks in y

            const int preds_w = 4 * mb_w + 1; // prediction size in x
            const int preds_h = 4 * mb_h + 1; // prediction size in y

            const int y_width = xsize;
            const int y_height = ysize;

            const int uv_width = (xsize + 1) >> 1;
            const int uv_height = (ysize + 1) >> 1;

            const int y_stride = y_width;
            const int uv_stride = uv_width;

            const int expand_yheight = RoundUp(ysize, 16);
            const int expand_uvheight = RoundUp(uv_height, 8);

            uint64_t y_size = 0;
            uint64_t uv_size = 0;

            int mb_size = 0;
            int preds_size = 0;
            int nz_size = 0;
            int top_data_size = 0;
            int lf_stats_size = 0;
            int quant_matrix_size = 0;
            int coeffs_size = 0;
            int stats_size = 0;
            int level_cost_size = 0;
            int bw_buf_size = 0;
            int sse_size = 0;
            int block_count_size = 0;
            int extra_info_size = 0;
            int max_edge_size = 0;
            int bit_count_size = 0;
            int expand_y_size = 0;
            int expand_uv_size = 0;
            int input_size = 0;

            // bits size
            y_size = y_width * y_height * sizeof(uint8_t);
            uv_size = uv_width * uv_height * sizeof(uint8_t);
            mb_size = mb_w * mb_h * sizeof(uint8_t);
            preds_size = preds_w * preds_h * sizeof(uint8_t) + preds_w + 1;
            nz_size = (mb_w + 1 + 1) * sizeof(uint32_t) /*+ WEBP_ALIGN_CST*/;
            top_data_size = mb_w * 16 * sizeof(uint8_t);
            lf_stats_size = NUM_MB_SEGMENTS * MAX_LF_LEVELS * sizeof(double);
            quant_matrix_size = sizeof(VP8EncMatrix);
            coeffs_size = NUM_TYPES * NUM_BANDS * NUM_CTX * NUM_PROBAS * sizeof(uint8_t);
            stats_size = NUM_TYPES * NUM_BANDS * NUM_CTX * NUM_PROBAS * sizeof(uint32_t);
            level_cost_size = NUM_TYPES * NUM_BANDS * NUM_CTX * (MAX_VARIABLE_LEVEL + 1) * sizeof(uint16_t);
            bw_buf_size = 408000 * sizeof(uint8_t);
            sse_size = 4 * sizeof(uint64_t);
            block_count_size = 3 * sizeof(int);
            extra_info_size = mb_w * mb_h * sizeof(uint8_t);
            max_edge_size = NUM_MB_SEGMENTS * sizeof(int);
            bit_count_size = 4 * 3 * sizeof(uint64_t);
            input_size = sizeof(EncloopInputData);
            int output_size = MAX_NUM_MB_W * MAX_NUM_MB_H * 1024 * sizeof(uint16_t);

            int output_tokens_size = sizeof(uint16_t) * PAGE_COUNT * TOKENS_COUNT_PER_PAGE;

            input_data.width = xsize;
            input_data.height = ysize;
            input_data.filter_sharpness = enc->config_->filter_sharpness;
            input_data.show_compressed = enc->config_->show_compressed;
            input_data.extra_info_type = enc->pic_->extra_info_type;
            input_data.stats_add = enc->pic_->stats;
            input_data.simple = enc->filter_hdr_.simple_;
            input_data.num_parts = enc->num_parts_;
            input_data.max_i4_header_bits = enc->max_i4_header_bits_;

            if (enc->lf_stats_ == NULL) {
                input_data.lf_stats_status = 0;
            } else {
                input_data.lf_stats_status = 1;
            }

            input_data.use_skip_proba = !enc->proba_.use_skip_proba_;
            input_data.method = enc->method_;
            input_data.rd_opt = (int)enc->rd_opt_level_;

            for (i = 0; i < NUM_MB_SEGMENTS; i++) {

                segment_data.quant[i] = enc->dqm_[i].quant_;
                segment_data.fstrength[i] = enc->dqm_[i].fstrength_;
                segment_data.max_edge[i] = enc->dqm_[i].max_edge_;
                segment_data.min_disto[i] = enc->dqm_[i].min_disto_;
                segment_data.lambda_i16[i] = enc->dqm_[i].lambda_i16_;
                segment_data.lambda_i4[i] = enc->dqm_[i].lambda_i4_;
                segment_data.lambda_uv[i] = enc->dqm_[i].lambda_uv_;
                segment_data.lambda_mode[i] = enc->dqm_[i].lambda_mode_;
                segment_data.tlambda[i] = enc->dqm_[i].tlambda_;
                segment_data.lambda_trellis_i16[i] = enc->dqm_[i].lambda_trellis_i16_;
                segment_data.lambda_trellis_i4[i] = enc->dqm_[i].lambda_trellis_i4_;
                segment_data.lambda_trellis_uv[i] = enc->dqm_[i].lambda_trellis_uv_;
            }

            expand_y_size = (expand_yheight - ysize) * xsize;
            uint8_t expand_y[expand_y_size];
            if (expand_yheight > ysize) {
                for (i = 0; i < expand_yheight - ysize; i++) {
                    memcpy(expand_y + i * xsize, enc->pic_->y + xsize * (ysize - 1), xsize);
                }
            }

            // copy expanded block
            expand_uv_size = (expand_uvheight - uv_height) * uv_width;
            uint8_t expand_u[expand_uv_size];
            uint8_t expand_v[expand_uv_size];
            if (expand_uvheight > uv_height) {
                for (i = 0; i < expand_uvheight - uv_height; i++) {
                    memcpy(expand_u + i * uv_width, enc->pic_->u + uv_width * (uv_height - 1), uv_width);
                    memcpy(expand_v + i * uv_width, enc->pic_->v + uv_width * (uv_height - 1), uv_width);
                }
            }

            uint8_t mb_info[5 * mb_w * mb_h];
            for (index = 0; index < mb_size; index++) {
                mb_info[5 * index + 0] = enc->mb_info_[index].type_;
                mb_info[5 * index + 1] = enc->mb_info_[index].uv_mode_;
                mb_info[5 * index + 2] = enc->mb_info_[index].skip_;
                mb_info[5 * index + 3] = enc->mb_info_[index].segment_;
                mb_info[5 * index + 4] = enc->mb_info_[index].alpha_;
            }

            for (i = 0; i < NUM_MB_SEGMENTS; i++) {
                VP8Matrix* matrix = &(enc->dqm_[i].y1_);
                for (j = 0; j < 16; j++) {
                    matrix_y1[i].q_[j] = matrix->q_[j];
                    matrix_y1[i].iq_[j] = matrix->iq_[j];
                    matrix_y1[i].bias_[j] = matrix->bias_[j];
                    matrix_y1[i].zthresh_[j] = matrix->zthresh_[j];
                    matrix_y1[i].sharpen_[j] = matrix->sharpen_[j];
                }
            }

            for (i = 0; i < NUM_MB_SEGMENTS; i++) {
                VP8Matrix* matrix = &(enc->dqm_[i].y2_);
                for (j = 0; j < 16; j++) {
                    matrix_y2[i].q_[j] = matrix->q_[j];
                    matrix_y2[i].iq_[j] = matrix->iq_[j];
                    matrix_y2[i].bias_[j] = matrix->bias_[j];
                    matrix_y2[i].zthresh_[j] = matrix->zthresh_[j];
                    matrix_y2[i].sharpen_[j] = matrix->sharpen_[j];
                }
            }

            for (i = 0; i < NUM_MB_SEGMENTS; i++) {
                VP8Matrix* matrix = &(enc->dqm_[i].uv_);
                for (j = 0; j < 16; j++) {
                    matrix_uv[i].q_[j] = matrix->q_[j];
                    matrix_uv[i].iq_[j] = matrix->iq_[j];
                    matrix_uv[i].bias_[j] = matrix->bias_[j];
                    matrix_uv[i].zthresh_[j] = matrix->zthresh_[j];
                    matrix_uv[i].sharpen_[j] = matrix->sharpen_[j];
                }
            }

            output_data.range = enc->parts_[0].range_;
            output_data.value = enc->parts_[0].value_;
            output_data.run = enc->parts_[0].run_;
            output_data.nb_bits = enc->parts_[0].nb_bits_;
            output_data.pos = enc->parts_[0].pos_;
            output_data.max_pos = enc->parts_[0].max_pos_;
            output_data.error = enc->parts_[0].error_;

            uint8_t y_top[mb_w * 16];
            uint8_t uv_top[mb_w * 16];

            memset(y_top, 127, top_data_size);
            memset(uv_top, 127, top_data_size);

            int max_edge_data[NUM_MB_SEGMENTS];
            for (i = 0; i < NUM_MB_SEGMENTS; i++) {
                max_edge_data[i] = enc->dqm_[i].max_edge_;
            }
            uint64_t bit_count[4][3];

            size_t globalSize[] = {1, 1, 1};
            size_t localSize[] = {1, 1, 1};


            // copy buffer

            err = clEnqueueWriteBuffer(hardware.mQueue, enclooppara.input, CL_FALSE, 0, sizeof(EncloopInputData), &input_data, 0, NULL, NULL);
            if(CL_SUCCESS != err) {
                fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
                ok = 0;
                goto Err;
            }

            err = clEnqueueWriteBuffer(hardware.mQueue, enclooppara.y, CL_FALSE, 0, y_size, enc->pic_->y, 0, NULL, NULL);
            if(CL_SUCCESS != err) {
                fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
                ok = 0;
                goto Err;
            }
            /*
              if (expand_yheight > y_height) {
              err = clEnqueueWriteBuffer(hardware.mQueue, enclooppara.y, CL_FALSE, y_size, expand_y_size, expand_y, 0, NULL, NULL);
              if(CL_SUCCESS != err) {
              fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
              ok = 0;
              goto Err;
              }
              }
            */
            err = clEnqueueWriteBuffer(hardware.mQueue, enclooppara.u, CL_FALSE, 0, uv_size, enc->pic_->u, 0, NULL, NULL);
            if(CL_SUCCESS != err) {
                fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
                ok = 0;
                goto Err;
            }
            /*
              if (expand_uvheight > uv_height) {
              err = clEnqueueWriteBuffer(hardware.mQueue, enclooppara.u, CL_FALSE, uv_size, expand_uv_size, expand_u, 0, NULL, NULL);
              if(CL_SUCCESS != err) {
              fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
              ok = 0;
              goto Err;
              }
              }
            */
            err = clEnqueueWriteBuffer(hardware.mQueue, enclooppara.v, CL_FALSE, 0, uv_size, enc->pic_->v, 0, NULL, NULL);
            if(CL_SUCCESS != err) {
                fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
                ok = 0;
                goto Err;
            }
            /*
              if (expand_uvheight > uv_height) {
              err = clEnqueueWriteBuffer(hardware.mQueue, enclooppara.v, CL_FALSE, uv_size, expand_uv_size, expand_v, 0, NULL, NULL);
              if(CL_SUCCESS != err) {
              fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
              ok = 0;
              goto Err;
              }
              }
            */
            /* err = clEnqueueWriteBuffer(hardware.mQueue, enclooppara.output_tokens, CL_FALSE, 0, output_tokens_size, output_tokens.tokens_, 0, NULL, NULL); */
            /* if(CL_SUCCESS != err) { */
            /*    fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
            /*    ok = 0; */
            /*    goto Err; */
            /* } */


            // *********************************** run kernel ********************************

            err = clEnqueueNDRangeKernel(hardware.mQueue, encloop.mKernel, 1, 0,
                                         globalSize, localSize, 0, NULL, NULL);
            if (err != CL_SUCCESS) {
                fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
                ok = 0;
                goto Err;
            }

            output_size = MAX_NUM_MB_W * MAX_NUM_MB_H * 1024 * sizeof(uint16_t);

            // *************************************************************************


            // read buffer from device

            fprintf(stderr, "start enctokenloop clFinish\n");
            err = clFinish(hardware.mQueue);
            if (err != CL_SUCCESS) {
                fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
                ok = 0;
                goto Err;
            }
            fprintf(stderr, "stop enctokenloop clFinish\n");

            err = clEnqueueReadBuffer(hardware.mQueue, enclooppara.y, CL_FALSE, 0, y_size, enc->pic_->y, 0, NULL, NULL);
            if(CL_SUCCESS != err) {
                fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
                ok = 0;
                goto Err;
            }

            err = clEnqueueReadBuffer(hardware.mQueue, enclooppara.u, CL_FALSE, 0, uv_size, enc->pic_->u, 0, NULL, NULL);
            if(CL_SUCCESS != err) {
                fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
                ok = 0;
                goto Err;
            }

            err = clEnqueueReadBuffer(hardware.mQueue, enclooppara.v, CL_FALSE, 0, uv_size, enc->pic_->v, 0, NULL, NULL);
            if(CL_SUCCESS != err) {
                fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
                ok = 0;
                goto Err;
            }

            /* err = clEnqueueReadBuffer(hardware.mQueue, enclooppara.output, CL_FALSE, 0, output_size, output_tokens.tokens_, 0, NULL, NULL); */
            /* if(CL_SUCCESS != err) { */
            /*    fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
            /*    ok = 0; */
            /*    goto Err; */
            /* } */

            /* output_tokens.error_ = output_data.error_; */
            /* enc->tokens_.left_ = output_data.left_; */
            /* enc->tokens_.page_size_ = output_data.page_size_; */
            /* enc->tokens_.error_ = output_data.error_; */
            /* ReadTokenFromKernel(&enc->tokens_, &output_tokens); */

#else
            int index = 0;

#if TOKEN_RECONSTRUCT
            VP8TBufferKernel tokens_;
            VP8TBufferKernelInit(&tokens_, enc->tokens_.page_size_);
#endif

            printf("TOKEN_RECONSTRUCT :%d\n", TOKEN_RECONSTRUCT);

            do {
                index++;
                VP8ModeScore info;
                VP8IteratorImport(&it, NULL);
                if (--cnt < 0) {
                    // printf("index:%d cnt:%d\n", index, cnt);
                    // debug_log_stats_p(proba);
                    FinalizeTokenProbas(proba);
                    VP8CalculateLevelCosts(proba);  // refresh cost tables for rd-opt
                    cnt = max_count;
                }

                VP8Decimate(&it, &info, rd_opt);
#if TOKEN_RECONSTRUCT
                ok = RecordTokens(&it, &info, &tokens_);
#else
                ok = RecordTokens(&it, &info, &enc->tokens_);
#endif
                // if (!ok) {
                //   WebPEncodingSetError(enc->pic_, VP8_ENC_ERROR_OUT_OF_MEMORY);
                //   break;
                // }
                // size_p0 += info.H;
                // distortion += info.D;
                // if (is_last_pass) {
                //   StoreSideInfo(&it);
                //   VP8StoreFilterStats(&it);
                //   VP8IteratorExport(&it);
                //   ok = VP8IteratorProgress(&it, 20);
                // }
                VP8IteratorSaveBoundary(&it);
            } while (/*0*/ok && VP8IteratorNext(&it));// TODO

            debug_tokens(&enc->tokens_, &it);
#endif

#if TOKEN_RECONSTRUCT
            ReadTokenFromKernel(&enc->tokens_, &tokens_);
#endif
            if (!ok) break;
            debug_probas(&enc->proba_);

#if 1
            size_p0 += enc->segment_hdr_.size_;
            // printf("stats.do_size_search:%d\n", stats.do_size_search);
            if (stats.do_size_search) {
                uint64_t size = FinalizeTokenProbas(&enc->proba_);
                size += VP8EstimateTokenSize(&enc->tokens_,
                                             (const uint8_t*)proba->coeffs_);
                size = (size + size_p0 + 1024) >> 11;  // -> size in bytes
                size += HEADER_SIZE_ESTIMATE;
                stats.value = (double)size;
            } else {  // compute and store PSNR
                stats.value = GetPSNR(distortion, pixel_count);
            }

#if (DEBUG_SEARCH > 0)
            printf("#%2d metric:%.1lf -> %.1lf   last_q=%.2lf q=%.2lf dq=%.2lf\n",
                   num_pass_left, stats.last_value, stats.value,
                   stats.last_q, stats.q, stats.dq);
#endif
            // printf("size_p0:%d PARTITION0_SIZE_LIMIT:%d\n", size_p0, PARTITION0_SIZE_LIMIT);
            if (size_p0 > PARTITION0_SIZE_LIMIT) {
                ++num_pass_left;
                enc->max_i4_header_bits_ >>= 1;  // strengthen header bit limitation...
                continue;                        // ...and start over
            }
            if (is_last_pass) {
                break;   // done
            }
            if (do_search) {
                ComputeNextQ(&stats);  // Adjust q
            }
#endif
        }
#if 1
        if (ok) {
            if (!stats.do_size_search) {
                FinalizeTokenProbas(&enc->proba_);
            }
            ok = VP8EmitTokens(&enc->tokens_, enc->parts_ + 0,
                               (const uint8_t*)proba->coeffs_, 1);
        }
        ok = ok && WebPReportProgress(enc->pic_, enc->percent_ + 20, &enc->percent_);
        StopProfiling(&stop_watch, &timeEncTokenLoop, &countEncTokenLoop);
#endif
        return PostLoopFinalize(&it, ok);
        // return 1;

#ifdef USE_C_KERNEL

    Err:
        releaseKernel(encloop);
        clReleaseMemObject(enclooppara.input);
        clReleaseMemObject(enclooppara.y);
        clReleaseMemObject(enclooppara.u);
        clReleaseMemObject(enclooppara.v);
        /* clReleaseMemObject(enclooppara.mb_info); */
        /* clReleaseMemObject(enclooppara.preds); */
        /* clReleaseMemObject(enclooppara.nz); */
        /* clReleaseMemObject(enclooppara.y_top); */
        /* clReleaseMemObject(enclooppara.uv_top); */
        /* clReleaseMemObject(enclooppara.quant_matrix); */
        /* clReleaseMemObject(enclooppara.coeffs); */
        /* clReleaseMemObject(enclooppara.stats); */
        /* clReleaseMemObject(enclooppara.level_cost); */
        /* clReleaseMemObject(enclooppara.bw_buf); */
        /* clReleaseMemObject(enclooppara.sse); */
        /* clReleaseMemObject(enclooppara.block_count); */
        /* clReleaseMemObject(enclooppara.extra_info); */
        /* clReleaseMemObject(enclooppara.max_edge); */
        /* clReleaseMemObject(enclooppara.bit_count); */
        /* clReleaseMemObject(enclooppara.sse_count); */
        /* clReleaseMemObject(enclooppara.output_data); */
        /* clReleaseMemObject(enclooppara.output_tokens); */
        clReleaseMemObject(enclooppara.output);

        return ok;
#endif

    }
    //-------------------------------------------------------------------------------------//


    static int RecordTokens_nrd2(
        VP8Encoder* const enc,
        ap_NoneZero* ap_nz,
        int x_, int y_,int type_,
        hls::stream< ap_int<WD_LEVEL*16> >* str_level_dc,
        hls::stream< ap_int<WD_LEVEL*16> >* str_level_y,
        hls::stream< ap_int<WD_LEVEL*16> >* str_level_uv,
        VP8TBuffer* const tokens) {
        int x, y, ch;
        VP8Residual res;
        //VP8Encoder* const enc = it->enc_;

        //VP8IteratorNzToBytes(it);
        ap_uint<9> ap_top_nz = ap_nz->load_top9(x_, y_);
        ap_uint<9> ap_left_nz = ap_nz->load_left9(x_);
        int top_nz_[9] ;//=ap_top_nz[i];
        int left_nz_[9];//= ap_left_nz[i];
        for(int i=0;i<9;i++){
            top_nz_[i] =ap_top_nz[i];
            left_nz_[i]= ap_left_nz[i];
        }
        ap_int<WD_LEVEL*16> tmp16 = str_level_dc->read();
        if (type_ == 1) {   // i16x16
            const int ctx = top_nz_[8] + left_nz_[8];
            VP8InitResidual(0, 1, enc, &res);
            short int y_dc_levels[16];
            CPY16(y_dc_levels, tmp16, WD_LEVEL);
            VP8SetResidualCoeffs(y_dc_levels, &res);
            //    VP8SetResidualCoeffs(rd->y_dc_levels, &res);
            top_nz_[8] = left_nz_[8] =
                VP8RecordCoeffTokens(ctx, 1,
                                     res.first, res.last, res.coeffs, tokens);
            VP8RecordCoeffs(ctx, &res);
            VP8InitResidual(1, 0, enc, &res);
        } else {
            VP8InitResidual(0, 3, enc, &res);
        }

        // luma-AC
        for (y = 0; y < 4; ++y) {
            for (x = 0; x < 4; ++x) {
                const int ctx = top_nz_[x] + left_nz_[y];
                short int y_ac_levels[16];
                ap_int<WD_LEVEL*16> tmp = str_level_y->read();
                CPY16(y_ac_levels,tmp,WD_LEVEL);
                //VP8SetResidualCoeffs(rd->y_ac_levels[x + y * 4], &res);
                VP8SetResidualCoeffs(y_ac_levels, &res);
                top_nz_[x] = left_nz_[y] =
                    VP8RecordCoeffTokens(ctx, res.coeff_type,
                                         res.first, res.last, res.coeffs, tokens);
                VP8RecordCoeffs(ctx, &res);
            }
        }

        // U/V
        VP8InitResidual(0, 2, enc, &res);
        for (ch = 0; ch <= 2; ch += 2) {
            for (y = 0; y < 2; ++y) {
                for (x = 0; x < 2; ++x) {
                    const int ctx = top_nz_[4 + ch + x] + left_nz_[4 + ch + y];
                    short int uv_levels[16];
                    ap_int<WD_LEVEL*16> tmp = str_level_uv->read();
                    CPY16(uv_levels,tmp,WD_LEVEL);
                    //VP8SetResidualCoeffs(rd->uv_levels[ch * 2 + x + y * 2], &res);
                    VP8SetResidualCoeffs(uv_levels, &res);
                    top_nz_[4 + ch + x] = left_nz_[4 + ch + y] =
                        VP8RecordCoeffTokens(ctx, 2, res.first, res.last, res.coeffs, tokens);
                    VP8RecordCoeffs(ctx, &res);
                }
            }
        }
        //VP8IteratorBytesToNz(it);
        uint32_t nz=0;
        nz |= (top_nz_[0] << 12) | (top_nz_[1] << 13);
        nz |= (top_nz_[2] << 14) | (top_nz_[3] << 15);
        nz |= (top_nz_[4] << 18) | (top_nz_[5] << 19);
        nz |= (top_nz_[6] << 22) | (top_nz_[7] << 23);
        nz |= (top_nz_[8] << 24);  // we propagate the _top_ bit, esp. for intra4
        // left
        nz |= (left_nz_[0] << 3) | (left_nz_[1] << 7);
        nz |= (left_nz_[2] << 11);
        nz |= (left_nz_[4] << 17) | (left_nz_[6] << 21);

        // VP8IteratorNzToBytes(it);
        ap_nz->left_nz[8] = left_nz_[8];
        ap_nz->nz_current = nz;//*it->nz_;
        ap_nz->store_nz(x_);
        //  ap_uint<25> mask=0x1eef888 & ap_nz->nz_current;
        return !tokens->error_;
    }


    static int RecordTokens_nrd2(
        VP8Encoder* const enc,
        ap_NoneZero* ap_nz,
        int x_, int y_,int type_,
        short int* y_dc_levels,
        short int* y_ac_levels,
        short int* uv_levels,
        VP8TBuffer* const tokens) {
        int x, y, ch;
        VP8Residual res;
        //VP8Encoder* const enc = it->enc_;

        //VP8IteratorNzToBytes(it);
        ap_uint<9> ap_top_nz = ap_nz->load_top9(x_, y_);
        ap_uint<9> ap_left_nz = ap_nz->load_left9(x_);
        int top_nz_[9] ;//=ap_top_nz[i];
        int left_nz_[9];//= ap_left_nz[i];
        for(int i=0;i<9;i++){
            top_nz_[i] =ap_top_nz[i];
            left_nz_[i]= ap_left_nz[i];
        }
        //ap_int<WD_LEVEL*16> tmp16 = str_level_dc->read();
        if (type_ == 1) {   // i16x16
            const int ctx = top_nz_[8] + left_nz_[8];
            {//VP8InitResidual_smp(0, 1, enc, &res);
                //VP8SetResidualCoeffs(y_dc_levels, &res);

                int first=0;
                int coeff_type=1;// 0: i16-AC,  1: i16-DC,  2:chroma-AC,  3:i4-AC
                res.coeff_type = coeff_type;
                res.prob  = enc->proba_.coeffs_[coeff_type];
                res.stats = enc->proba_.stats_[coeff_type];
                res.first = first;
                res.coeffs = y_dc_levels;
                int n;
                res.last = -1;
                assert(res.first == 0 || res.coeffs[0] == 0);
                for (n = 15; n >= 0; --n) {
                    if (res.coeffs[n]) {
                        res.last = n;
                        break;
                    }
                }

            }

            top_nz_[8] = left_nz_[8] =
                VP8RecordCoeffTokens(ctx, 1,
                                     res.first, res.last, res.coeffs, tokens);
            VP8RecordCoeffs(ctx, &res);
            // VP8InitResidual_smp(1, 0, enc, &res);
            {
                int first=1;
                int coeff_type=0;// 0: i16-AC,  1: i16-DC,  2:chroma-AC,  3:i4-AC
                res.coeff_type = coeff_type;
                res.prob  = enc->proba_.coeffs_[coeff_type];
                res.stats = enc->proba_.stats_[coeff_type];
                res.first = first;
            }
        } else {
            //VP8InitResidual_smp(0, 3, enc, &res);
            {
                int first=0;
                int coeff_type=3;// 0: i16-AC,  1: i16-DC,  2:chroma-AC,  3:i4-AC
                res.coeff_type = coeff_type;
                res.prob  = enc->proba_.coeffs_[coeff_type];
                res.stats = enc->proba_.stats_[coeff_type];
                res.first = first;
            }
        }

        // luma-AC
        for (y = 0; y < 4; ++y) {
            for (x = 0; x < 4; ++x) {
                const int ctx = top_nz_[x] + left_nz_[y];
                //short int y_ac_levels[16];
                //  ap_int<WD_LEVEL*16> tmp = str_level_y->read();
                //  CPY16(y_ac_levels,tmp,WD_LEVEL);
                //VP8SetResidualCoeffs(rd->y_ac_levels[x + y * 4], &res);
                VP8SetResidualCoeffs(y_ac_levels, &res);y_ac_levels+=16;
                top_nz_[x] = left_nz_[y] =
                    VP8RecordCoeffTokens(ctx, res.coeff_type,
                                         res.first, res.last, res.coeffs, tokens);
                VP8RecordCoeffs(ctx, &res);
            }
        }

        // U/V
        // VP8InitResidual_smp(0, 2, enc, &res);
        {
            int first=0;
            int coeff_type=2;// 0: i16-AC,  1: i16-DC,  2:chroma-AC,  3:i4-AC
            res.coeff_type = coeff_type;
            res.prob  = enc->proba_.coeffs_[coeff_type];
            res.stats = enc->proba_.stats_[coeff_type];
            res.first = first;
        }
        for (ch = 0; ch <= 2; ch += 2) {
            for (y = 0; y < 2; ++y) {
                for (x = 0; x < 2; ++x) {
                    const int ctx = top_nz_[4 + ch + x] + left_nz_[4 + ch + y];
                    //  short int uv_levels[16];
                    //  ap_int<WD_LEVEL*16> tmp = str_level_uv->read();
                    //  CPY16(uv_levels,tmp,WD_LEVEL);
                    //VP8SetResidualCoeffs(rd->uv_levels[ch * 2 + x + y * 2], &res);
                    VP8SetResidualCoeffs(uv_levels, &res);uv_levels+=16;
                    top_nz_[4 + ch + x] = left_nz_[4 + ch + y] =
                        VP8RecordCoeffTokens(ctx, 2, res.first, res.last, res.coeffs, tokens);
                    VP8RecordCoeffs(ctx, &res);
                }
            }
        }
        //VP8IteratorBytesToNz(it);
        uint32_t nz=0;
        nz |= (top_nz_[0] << 12) | (top_nz_[1] << 13);
        nz |= (top_nz_[2] << 14) | (top_nz_[3] << 15);
        nz |= (top_nz_[4] << 18) | (top_nz_[5] << 19);
        nz |= (top_nz_[6] << 22) | (top_nz_[7] << 23);
        nz |= (top_nz_[8] << 24);  // we propagate the _top_ bit, esp. for intra4
        // left
        nz |= (left_nz_[0] << 3) | (left_nz_[1] << 7);
        nz |= (left_nz_[2] << 11);
        nz |= (left_nz_[4] << 17) | (left_nz_[6] << 21);

        // VP8IteratorNzToBytes(it);
        ap_nz->left_nz[8] = left_nz_[8];
        ap_nz->nz_current = nz;//*it->nz_;
        ap_nz->store_nz(x_);
        //  ap_uint<25> mask=0x1eef888 & ap_nz->nz_current;
        return !tokens->error_;
    }


    void Set_AllPicInfo( AllPicInfo* des, VP8Encoder* const enc)
    {
        const WebPPicture* const pic = enc->pic_;

        des->mb_w = enc->mb_w_;//

        des->id_pic;//0
        des->cnt_line_mb;
        des->y_stride       = pic->y_stride;
        des->uv_stride      = pic->uv_stride;
        des->width          = pic->width;
        des->height         = pic->height;
        des->mb_w           = enc->mb_w_;//
        des->mb_h           = enc->mb_h_;//5
        VP8SegmentInfo* dqm = &enc->dqm_[0];//
        des->seg_lambda_p16 = dqm->lambda_i16_;
        des->seg_lambda_p44 = dqm->lambda_i4_;
        des->seg_tlambda    = dqm->tlambda_;
        des->seg_lambda_uv  = dqm->lambda_uv_;
        des->seg_tlambda_m  = dqm->lambda_mode_;//10

        des->seg_y1_q_0      = dqm->y1_.q_[0];     // quantizer steps
        des->seg_y1_q_n      = dqm->y1_.q_[1];
        des->seg_y1_iq_0     = dqm->y1_.iq_[0];    // reciprocals fixed point.
        des->seg_y1_iq_n     = dqm->y1_.iq_[1];
        des->seg_y1_bias_0   = dqm->y1_.bias_[0];  // rounding bias
        des->seg_y1_bias_n   = dqm->y1_.bias_[1];//16

        des->seg_y2_q_0      = dqm->y2_.q_[0];     // quantizer steps
        des->seg_y2_q_n      = dqm->y2_.q_[1];
        des->seg_y2_iq_0     = dqm->y2_.iq_[0];    // reciprocals fixed point.
        des->seg_y2_iq_n     = dqm->y2_.iq_[1];
        des->seg_y2_bias_0   = dqm->y2_.bias_[0];  // rounding bias
        des->seg_y2_bias_n   = dqm->y2_.bias_[1];//22

        des->seg_uv_q_0      = dqm->uv_.q_[0];     // quantizer steps
        des->seg_uv_q_n      = dqm->uv_.q_[1];
        des->seg_uv_iq_0     = dqm->uv_.iq_[0];    // reciprocals fixed point.
        des->seg_uv_iq_n     = dqm->uv_.iq_[1];
        des->seg_uv_bias_0   = dqm->uv_.bias_[0];  // rounding bias
        des->seg_uv_bias_n   = dqm->uv_.bias_[1];//28
        for(int i=0; i<16 ;i++)
        {
            des->seg_y1_sharpen[i] = dqm->y1_.sharpen_[i];
            des->seg_uv_sharpen[i] = dqm->uv_.sharpen_[i];
        }
    }


    /*   int VP8EncTokenLoop_ryanw(VP8Encoder* const enc) { */


    /*    const WebPPicture* const pic = enc->pic_; */
    /*    VP8EncIterator it; */
    /*    PassStats stats; */
    /*    int ok; */
    /*    InitPassStats(enc, &stats); */
    /*    ok = PreLoopInitialize(enc); */
    /*    if (!ok) return 0; */
    /*    VP8IteratorInit(enc, &it); */

    /*    //////////////////////////////////////////////// */
    /*    float q = Clamp(stats.q, 0.f, 100.f); */
    /*    VP8SetSegmentParams(enc, q);      // setup segment quantizations and filters */
    /*    SetSegmentProbas(enc);            // compute segment probabilities */
    /*    ResetSSE(enc); */


    /*    /////VP8DefaultProbas(enc); */
    /*    VP8EncProba* const proba = &enc->proba_; */
    /*    proba->use_skip_proba_ = 0; */
    /*    memset(proba->segments_, 255u, sizeof(proba->segments_));//oly {0xff, 0xff, 0xff} */
    /*    memcpy(proba->coeffs_, VP8CoeffsProba0, sizeof(VP8CoeffsProba0)); */
    /*    proba->dirty_ = 1; */
    /*    /////SetLoopParams(enc, stats.q); */

    /*    //////ResetStats(enc); */
    /*    //VP8CalculateLevelCosts(proba); */
    /*    proba->nb_skip_ = 0; */

    /*    ///// ResetTokenStats(enc); */
    /*    memset(proba->stats_, 0, sizeof(proba->stats_)); */
    /*    ///////////////////////////////////////////////// */
    /*    VP8TBufferClear(&enc->tokens_); */

    /*    //FinalizeTokenProbas(&enc->proba_);//This is about AC */

    /*    it.do_trellis_ = 0;//(rd_opt RD_OPT_TRELLIS_ALL); */
    /*    /\*************************************************************\/ */
    /*    /\* Preparing data for FPGA                                   *\/ */
    /*    /\*************************************************************\/ */

    /*    AllPicInfo picinfo;//Picture information */

    /*    Set_AllPicInfo( &picinfo, enc);//Set picture information */
    /*    int size_info = sizeof(AllPicInfo); */
    /*    //int* p_info = malloc( size_info*4);//244 bytes now */
    /*    //interface of kernel top */
    /*    /\*--Following should be the interface of kernel -----------------------------------------------------------------*\/ */
    /*    int p_info[128]; */
    /*    uint8_t* ysrc; */
    /*    uint8_t* usrc; */
    /*    uint8_t* vsrc; */
    /*    int16_t* pout_level; */
    /*    uint8_t* pout_out; */
    /*    uint8_t* pout_pred; */
    /*    uint8_t* pout_ret; */
    /*    //   int16_t pout_mb[512];//for level, pred and ret, */
    /*    //   uint8_t pout_mb_out[384]; */
    /*    /\*-------------------------------------------------*\/ */

    /*    int num_mb = picinfo.mb_w * picinfo.mb_h; */
    /*    ysrc       = malloc( num_mb * 16 * 16 * sizeof(uint8_t)); */
    /*    usrc       = malloc( num_mb *  4 * 16 * sizeof(uint8_t)); */
    /*    vsrc       = malloc( num_mb *  4 * 16 * sizeof(uint8_t)); */
    /*    pout_level = malloc( num_mb *512 *      sizeof(int16_t)); */
    /*    //for pout_level, we plan to put all data of one MB into 1K Byte space and send it to DDR. */
    /*    //Thus no need to prepare a buffer for coefficients and other data which is 2 times bigger than input buffer */
    /*    pout_out   = malloc( num_mb * 24 * 16 * sizeof(uint8_t)); */
    /*    //   pout_pred  = malloc( num_mb * sizeof(uint64_t)); */
    /*    //   pout_ret   = malloc( num_mb * sizeof(uint8_t)); */

    /*    //for testing copy picture data from host to FPGA, should be replaced by formal code */
    /*    memcpy( (void*)ysrc,   (void*)(pic->y), picinfo.y_stride  *   picinfo.height); */
    /*    memcpy( (void*)usrc,   (void*)(pic->u), picinfo.uv_stride * ((picinfo.height+1)>>1)); */
    /*    memcpy( (void*)vsrc,   (void*)(pic->v), picinfo.uv_stride * ((picinfo.height+1)>>1)); */
    /*    memcpy( (void*)p_info, (void*)(&picinfo), size_info); */

    /*    FILE* fp_ysrc=fopen("fp_ysrc.dat", "wb"); */
    /*    FILE* fp_usrc=fopen("fp_usrc.dat", "wb"); */
    /*    FILE* fp_vsrc=fopen("fp_vsrc.dat", "wb"); */
    /*    FILE* fp_p_info=fopen("fp_p_info.dat", "wb"); */
    /*    fwrite( (void*)ysrc,        1, picinfo.y_stride  *   picinfo.height,            fp_ysrc); */
    /*    fwrite( (void*)usrc,    1, picinfo.uv_stride * ((picinfo.height+1)>>1),     fp_usrc); */
    /*    fwrite( (void*)vsrc,        1, picinfo.uv_stride * ((picinfo.height+1)>>1),     fp_vsrc); */
    /*    fwrite( (void*)p_info,      1, size_info,                                       fp_p_info); */
    /*    fclose(fp_ysrc); */
    /*    fclose(fp_usrc); */
    /*    fclose(fp_vsrc); */
    /*    fclose(fp_p_info); */

    /*    hls::stream< ap_uint<WD_PIX*16> >  str_out; */

    /*    kernel_IntraPredLoop2( */
    /*                          p_info, */
    /*                          ysrc, */
    /*                          usrc, */
    /*                          vsrc, */
    /* #ifdef _KEEP_PSNR_ */
    /*                          &str_out, */
    /* #endif */
    /*                          pout_level//(int32_t*)pout_level */
    /*                          ); */

    /*    FILE* fp_level=fopen("fp_level.dat", "wa"); */
    /*    fwrite( (void*)pout_level, 1, num_mb *512 * sizeof(int16_t), fp_level); */
    /*    fclose(fp_level); */

    /*    FinalizeTokenProbas(&enc->proba_); */
    /*    int16_t* pt=pout_level; */
    /*    ap_NoneZero ap_nz; */
    /*    do { */
    /*      ap_uint<LG2_MAX_NUM_MB_W> x_mb = it.x_; */
    /*      ap_uint<LG2_MAX_NUM_MB_W> y_mb = it.y_; */
    /* #ifdef _KEEP_PSNR_ */
    /*      VP8IteratorImport( &it, NULL); */
    /* #endif */
    /*      ap_uint<6> ret     = (ap_uint<6>)pt[416]; */
    /*      it.mb_->uv_mode_   = ret(3,0);//it_m.ap_uv_mode_c; */
    /*      it.mb_->type_      = ret(4,4); */
    /*      it.mb_->skip_      = ret(5,5);//(it_r.ap_nz == 0); */
    /*      for(int y=0; y<4 ; y++){ */
    /*        for(int x=0; x<4 ; x++){ */
    /*          it.preds_[x + it.enc_->preds_w_*y ] = pt[400+y*4+x];//SB_GET(mode_b,y,x,WD_MODE); */
    /*        } */
    /*      } */
    /*      //      FinalizeTokenProbas(&enc->proba_); */

    /*      ok = RecordTokens_nrd2(enc, &ap_nz, x_mb, y_mb, it.mb_->type_, pt, pt+16, pt+16*17, &enc->tokens_); */
    /*      pt+=512; */

    /* #ifdef _KEEP_PSNR_ */
    /*      int VP8ScanUV[4 + 4] = { */
    /*        0 + 0 * BPS,   4 + 0 * BPS, 0 + 4 * BPS,  4 + 4 * BPS,    // U */
    /*        8 + 0 * BPS,  12 + 0 * BPS, 8 + 4 * BPS, 12 + 4 * BPS     // V */
    /*      }; */
    /*      for(int n=0;n<16;n++){ */
    /*        ap_uint<WD_PIX*16> tmp = str_out.read(); */
    /*        set_vect_to(tmp,it.yuv_out_ + VP8Scan[n],32); */
    /*      } */
    /*      for(int n = 0; n < 8; n += 1){ */
    /*        ap_uint<WD_PIX*16> tmp = str_out.read(); */
    /*        set_vect_to(tmp,it.yuv_out_ + U_OFF_ENC+ VP8ScanUV[n],32); */
    /*      } */
    /*      StoreSideInfo(&it);//just for PSRN calculation, can be passed */
    /* #endif */

    /*    } while (ok && VP8IteratorNext(&it)); */

    /*    FinalizeTokenProbas(&enc->proba_);//This is about AC */
    /*    ok = VP8EmitTokens(&enc->tokens_, enc->parts_+0,(const uint8_t*)enc->proba_.coeffs_, 1); */
    /*    return PostLoopFinalize(&it, ok);//This functions */
    /*   } */

#else

    int VP8EncTokenLoop(VP8Encoder* const enc) {
        (void)enc;
        return 0;   // we shouldn't be here.
    }

#endif    // DISABLE_TOKEN_BUFFER


    int VP8EncTokenLoopAsyncHost2Device(VP8Encoder* const enc, const int buf, 
                                        AllPicInfo & picinfo,                 
                                        uint8_t* & pout_prob,
                                        uint8_t* & pout_bw,
                                        uint8_t* & pout_ret,
                                        uint8_t* & pout_pred,
                                        cl_uint num_wait_event, cl_event* wait_event, std::array<cl_event,4> & event){

        StopProfilingWatch watch, watch_total;
        double watch_time;
        int watch_count;

        StartProfiling(&watch_total);

        VP8DefaultProbas(enc);
        PassStats stats;
        int ok;
        int err;
        InitPassStats(enc, &stats);
        ok = PreLoopInitialize(enc);
        if (!ok) return 0;
        SetLoopParams(enc, stats.q);
        ResetTokenStats(enc);
        VP8TBufferClear(&enc->tokens_);

        const int xsize = enc->pic_->width;
        const int ysize = enc->pic_->height;

        const int mb_w = (xsize + 15) >> 4; // nb of blocks in x
        const int mb_h = (ysize + 15) >> 4; // nb of blocks in y
        const int num_mb = mb_w * mb_h;

        const int y_width = xsize;
        const int y_height = ysize;

        const int uv_width = (xsize + 1) >> 1;
        const int uv_height = (ysize + 1) >> 1;

        uint64_t y_size = 0;//
        uint64_t uv_size = 0;//

        // bits size
        y_size = y_width * y_height * sizeof(uint8_t);//
        uv_size = uv_width * uv_height * sizeof(uint8_t);//
        uint64_t output_size = MAX_NUM_MB_W * MAX_NUM_MB_H * 512 * sizeof(uint16_t); //

        int output_tokens_size = sizeof(uint16_t) * PAGE_COUNT * TOKENS_COUNT_PER_PAGE;

        // AllPicInfo picinfo; //Picture information
        ap_NoneZero ap_nz;

        Set_AllPicInfo(&picinfo, enc); //Set picture information

        output_size = MAX_NUM_MB_W * MAX_NUM_MB_H * 512 * sizeof(uint16_t);

        StartProfiling(&watch); 

        /* encloopparaAsync[buf].input = clCreateBuffer(hardware.mContext, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR, */
        /*                                           sizeof(AllPicInfo), &picinfo, &err); */
        /* if(CL_SUCCESS != err) { */
        /*   fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
        /* } */
        /* printf("INFO: Buffer .input created \n"); */

        /* encloopparaAsync[buf].y = clCreateBuffer(hardware.mContext, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR, */
        /*                                       y_size, (enc->pic_->y), &err); */
        /* if(CL_SUCCESS != err) { */
        /*   fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
        /* } */
  
        /* printf("INFO: Buffer .y created \n"); */

        /* encloopparaAsync[buf].u = clCreateBuffer(hardware.mContext, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR, */
        /*                                       uv_size, enc->pic_->u, &err); */
        /* if(CL_SUCCESS != err) { */
        /*   fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
        /* } */
  
        /* printf("INFO: Buffer .u created \n"); */
  
        /* encloopparaAsync[buf].v = clCreateBuffer(hardware.mContext, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR, */
        /*                                       uv_size, enc->pic_->v, &err); */
        /* if(CL_SUCCESS != err) { */
        /*   fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
        /* } */
  
        /* printf("INFO: Buffer .v created \n"); */

        /* encloopparaAsync[buf].output = clCreateBuffer(hardware.mContext, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR, */
        /*                                            output_size, pout_level, &err); */
        /* if(CL_SUCCESS != err) { */
        /*   fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
        /* } */
  
        /* printf("INFO: Buffer .output created \n"); */

        /* encloopparaAsync[buf].output_prob = clCreateBuffer(hardware.mContext, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR, */
        /*                                                 SIZE8_MEM_PROB, pout_prob, &err); */
        /* if(CL_SUCCESS != err) { */
        /*   fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
        /* } */
  
        /* printf("INFO: Buffer .output_prob created \n"); */

        /* encloopparaAsync[buf].output_bw = clCreateBuffer(hardware.mContext, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR, */
        /*                                               SIZE8_MEM_BW, pout_bw, &err); */
        /* if(CL_SUCCESS != err) { */
        /*   fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
        /* } */

        /* printf("INFO: Buffer .output_bw created \n"); */

        /* encloopparaAsync[buf].output_ret = clCreateBuffer(hardware.mContext, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR, */
        /*                                                SIZE8_MEM_RET, pout_ret, &err); */
        /* if(CL_SUCCESS != err) { */
        /*   fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
        /* } */

        /* printf("INFO: Buffer .output_ret created \n"); */

        /* encloopparaAsync[buf].output_pred = clCreateBuffer(hardware.mContext, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR, */
        /*                                                 SIZE8_MEM_PRED, pout_pred, &err); */
        /* if(CL_SUCCESS != err) { */
        /*   fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
        /* } */

        /* printf("INFO: Buffer .output_pred created \n"); */


        /* err = clEnqueueMigrateMemObjects(hardware.mQueue, 1, &(encloopparaAsync[buf].input), 0, */
        /*                               num_wait_event, wait_event, &event[0]); */

        err = clEnqueueWriteBuffer(hardware.mQueue, encloopparaAsync[buf].input, CL_FALSE, 0, sizeof(AllPicInfo), &picinfo,
                                   num_wait_event, wait_event, &event[0]);
        if(CL_SUCCESS != err) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
        }
        printf("INFO: COPY .input to Buffer.\n");

        /* err = clEnqueueMigrateMemObjects(hardware.mQueue, 1, &(encloopparaAsync[buf].y), 0, */
        /*                               num_wait_event, wait_event, &event[1]); */

        err = clEnqueueWriteBuffer(hardware.mQueue, encloopparaAsync[buf].y, CL_FALSE, 0, y_size, enc->pic_->y,
                                   num_wait_event, wait_event, &event[1]);
        if(CL_SUCCESS != err) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
        }

        printf("INFO: COPY .y to Buffer.\n");

        /* err = clEnqueueMigrateMemObjects(hardware.mQueue, 1, &(encloopparaAsync[buf].u), 0, */
        /*                               num_wait_event, wait_event, &event[2]); */

        err = clEnqueueWriteBuffer(hardware.mQueue, encloopparaAsync[buf].u, CL_FALSE, 0, uv_size, enc->pic_->u,
                                   num_wait_event, wait_event, &event[2]);
        if(CL_SUCCESS != err) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
        }

        printf("INFO: COPY .u to Buffer.\n");

        /* err = clEnqueueMigrateMemObjects(hardware.mQueue, 1, &(encloopparaAsync[buf].v), 0, */
        /*                               num_wait_event, wait_event, &event[3]); */
    
        err = clEnqueueWriteBuffer(hardware.mQueue, encloopparaAsync[buf].v, CL_FALSE, 0, uv_size, enc->pic_->v,
                                   num_wait_event, wait_event, &event[3]);
        if(CL_SUCCESS != err) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
        }
        printf("INFO: COPY .v to Buffer.\n");

        /* err = clFlush(hardware.mQueue); */
        /* if (err != CL_SUCCESS) { */
        /*   fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
        /*   ok = 0; */
        /* } */

        watch_time = 0.0;
        StopProfiling(&watch_total, &watch_time, &watch_count);
        printf("INFO: Host2Device finished. Computation time is %f (ms) \n", watch_time);

        return 1;
    };


    /* int VP8EncTokenLoopAsyncDevice2Host(VP8Encoder* const & enc, const int buf, int16_t* & pout_level, */
    /*                                      cl_uint num_wait_event, cl_event* wait_event, std::array<cl_event,2> & event){  */
    /*    StopProfilingWatch watch; */
    /*    double watch_time; */
    /*    int watch_count; */

    /*    int ok; */
    /*    cl_int err; */
    /*    ap_NoneZero ap_nz; */

    /*    const int xsize = enc->pic_->width; */
    /*    const int ysize = enc->pic_->height; */
    /*    const int y_width = xsize; */
    /*    const int y_height = ysize; */
    /*    const int uv_width = (xsize + 1) >> 1; */
    /*    const int uv_height = (ysize + 1) >> 1; */

    /*    // bits size */
    /*    const uint64_t y_size = y_width * y_height * sizeof(uint8_t);  */
    /*    const uint64_t uv_size = uv_width * uv_height * sizeof(uint8_t); */
    /*    const uint64_t output_size = MAX_NUM_MB_W * MAX_NUM_MB_H * 512 * sizeof(uint16_t); */

    /*    StartProfiling(&watch);  */

    /*    err = clEnqueueReadBuffer(hardware.mQueue, encloopparaAsync[buf].y, CL_FALSE, 0, y_size, enc->pic_->y, */
    /*                              num_wait_event, wait_event, &event[0]); */
    /*    if(CL_SUCCESS != err) { */
    /*      fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
    /*      ok = 0; */
    /*      //      goto Err; */
    /*    } */

    /*    watch_time = 0.0; */
    /*    StopProfiling(&watch, &watch_time, &watch_count); */
    /*    printf("INFO: COPY .y to Host.      Computation time is %f (ms) \n", watch_time); */

    /*    StartProfiling(&watch); */

    /*    err = clEnqueueReadBuffer(hardware.mQueue, encloopparaAsync[buf].output, CL_FALSE, 0, output_size, pout_level, */
    /*                              num_wait_event, wait_event, &event[1]); */
    /*    if(CL_SUCCESS != err) { */
    /*      fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
    /*      ok = 0; */
    /*      //      goto Err; */
    /*    } */

    /*    watch_time = 0.0; */
    /*    StopProfiling(&watch, &watch_time, &watch_count); */
    /*    printf("INFO: COPY .output to Host. Computation time is %f (ms) \n", watch_time); */

    /*    /\* err = clFinish(hardware.mQueue); *\/ */
    /*    /\* if (err != CL_SUCCESS) { *\/ */
    /*    /\*   fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); *\/ */
    /*    /\*   ok = 0; *\/ */
    /*    /\* } *\/ */
    /* }; */


    /* int VP8EncTokenLoopAsyncACKernel(VP8Encoder* const & enc, const int buf, int16_t* pout_level, VP8EncIterator & it){ */

    /*   VP8IteratorInit(enc, &it); */
    /*    it.do_trellis_ = 0;//(rd_opt RD_OPT_TRELLIS_ALL); */

    /*    int16_t* pt = pout_level; */

    /*    int ok; */
    /*    ap_NoneZero ap_nz; */

    /*    do { */
    /*      int x_mb = it.x_; */
    /*      int y_mb = it.y_; */

    /*      ap_uint<6> ret     = (ap_uint<6>)pt[416]; */
    /*      it.mb_->uv_mode_   = ret(3,0);//it_m.ap_uv_mode_c; */
    /*      it.mb_->type_      = ret(4,4); */
    /*      it.mb_->skip_      = ret(5,5);//(it_r.ap_nz == 0); */
    /*      for(int y=0; y<4 ; y++){ */
    /*        for(int x=0; x<4 ; x++){ */
    /*          it.preds_[x + it.enc_->preds_w_*y ] = pt[400+y*4+x];//SB_GET(mode_b,y,x,WD_MODE); */
    /*        } */
    /*      } */
    /*      FinalizeTokenProbas(&enc->proba_); */
    /*      ok = RecordTokens_nrd2(enc, &ap_nz, x_mb, y_mb, it.mb_->type_, pt, pt+16, pt+16*17, &enc->tokens_); */
    /*      pt += 512; */

    /*    } while (ok && VP8IteratorNext(&it)); */

    /*    // FinalizeTokenProbas(&enc->proba_);//This is about AC */

    /*    ok = VP8EmitTokens(&enc->tokens_, enc->parts_+0, (const uint8_t*)enc->proba_.coeffs_, 1); */
    /*    // PostLoopFinalize(&it, ok); // This functions */

    /*    return ok; */
    /* }; */


    /*   int VP8EncTokenLoopAsyncPredKernel(VP8Encoder* const & enc, const int buf,  */
    /*                                     cl_uint num_wait_event, cl_event* wait_event, std::array<cl_event,1> & event) { */

    /*    int ok; */
    /*    cl_int err; */

    /*    StopProfilingWatch watch; */
    /*    double watch_time; */
    /*    int watch_count; */

    /*    const int xsize = enc->pic_->width; */
    /*    const int ysize = enc->pic_->height; */
    /*    const int y_width = xsize; */
    /*    const int y_height = ysize; */
    /*    const int uv_width = (xsize + 1) >> 1; */
    /*    const int uv_height = (ysize + 1) >> 1; */

    /*    // bits size */
    /*    const uint64_t y_size = y_width * y_height * sizeof(uint8_t);// */
    /*    const uint64_t uv_size = uv_width * uv_height * sizeof(uint8_t);// */
    /*    const uint64_t output_size = MAX_NUM_MB_W * MAX_NUM_MB_H * 512 * sizeof(uint16_t); // */

    /*    StartProfiling(&watch);  */

    /*    // Set args */
    /*    int arg = 0; */
    /*    int status ; */
    /*    err = clSetKernelArg(encloop.mKernel, arg++, sizeof(cl_mem), &(encloopparaAsync[buf].input)); */
    /*    if (err != CL_SUCCESS) { */
    /*      fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
    /*      status = -1; */
    /*      //    goto Err; */
    /*    } */

    /*    err = clSetKernelArg(encloop.mKernel, arg++, sizeof(cl_mem), &(encloopparaAsync[buf].y)); */
    /*    if (err != CL_SUCCESS) { */
    /*      fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
    /*      status = -1; */
    /*      //    goto Err; */
    /*    } */
    
    /*    err = clSetKernelArg(encloop.mKernel, arg++, sizeof(cl_mem), &(encloopparaAsync[buf].u)); */
    /*    if (err != CL_SUCCESS) { */
    /*      fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
    /*      status = -1; */
    /*      //    goto Err; */
    /*    } */
    
    /*    err = clSetKernelArg(encloop.mKernel, arg++, sizeof(cl_mem), &(encloopparaAsync[buf].v)); */
    /*    if (err != CL_SUCCESS) { */
    /*      fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
    /*      status = -1; */
    /*      //    goto Err; */
    /*    } */
    
    /*    err = clSetKernelArg(encloop.mKernel, arg++, sizeof(cl_mem), &(encloopparaAsync[buf].output)); */
    /*    if (err != CL_SUCCESS) { */
    /*      fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
    /*      status = -1; */
    /*      //    goto Err; */
    /*    } */
    
    /*    // launch kernel */
    /*    size_t globalSize[] = {1, 1, 1}; */
    /*    size_t localSize[] = {1, 1, 1}; */

    /*    err = clEnqueueNDRangeKernel(hardware.mQueue, encloop.mKernel, 1, 0, */
    /*                                 globalSize, localSize, num_wait_event, wait_event, &event[0]); */
    /*    if (err != CL_SUCCESS) { */
    /*      fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
    /*      ok = 0; */
    /*      //      goto Err; */
    /*    } */

    /*    /\* err = clEnqueueNDRangeKernel(hardware.mQueue, encloop.mKernel, 1, 0, *\/ */
    /*    /\*                              globalSize, localSize, 0, NULL, NULL); *\/ */
    
    /*    /\* err = clFlush(hardware.mQueue); *\/ */
    /*    /\* if (err != CL_SUCCESS) { *\/ */
    /*    /\*   fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); *\/ */
    /*    /\*   ok = 0; *\/ */
    /*    /\* } *\/ */

    /*    watch_time = 0.0; */
    /*    StopProfiling(&watch, &watch_time, &watch_count); */
    /*    printf("\nINFO: PredKernel Finished. Computation time is %f (ms) \n\n", watch_time); */

    /* /\* #ifdef USE_C_KERNEL *\/ */
    /* /\*   Err: *\/ */
    /* /\*    /\\* releaseKernel(encloop); *\\/ *\/ */
    /* /\*    /\\* clReleaseMemObject(enclooppara.input); *\\/ *\/ */
    /* /\*    /\\* clReleaseMemObject(enclooppara.y); *\\/ *\/ */
    /* /\*    /\\* clReleaseMemObject(enclooppara.u); *\\/ *\/ */
    /* /\*    /\\* clReleaseMemObject(enclooppara.v); *\\/ *\/ */
    /* /\*    /\\* clReleaseMemObject(enclooppara.output); *\\/ *\/ */
    /* /\*    return ok; *\/ */
    /* /\* #endif *\/ */

    /*    ok = 1; */
    /*    return ok; */
    /*   } */

    int VP8EncTokenLoop_ryanw_k(VP8Encoder* const enc) {

        StopProfilingWatch watch;
        double watch_time;
        int watch_count;

        VP8DefaultProbas(enc);
        VP8EncIterator it;
        PassStats stats;
        int ok;
        InitPassStats(enc, &stats);
        ok = PreLoopInitialize(enc);
        if (!ok) return 0;
        VP8IteratorInit(enc, &it);
        SetLoopParams(enc, stats.q);
        ResetTokenStats(enc);
        VP8TBufferClear(&enc->tokens_);

        StopProfilingWatch stop_watch;
        StartProfiling(&stop_watch);

        const int xsize = enc->pic_->width;
        const int ysize = enc->pic_->height;

        const int mb_w = (xsize + 15) >> 4; // nb of blocks in x
        const int mb_h = (ysize + 15) >> 4; // nb of blocks in y
        const int num_mb = mb_w * mb_h;

        const int y_width = xsize;
        const int y_height = ysize;

        const int uv_width = (xsize + 1) >> 1;
        const int uv_height = (ysize + 1) >> 1;

        uint64_t y_size = 0;//
        uint64_t uv_size = 0;//

        // bits size
        y_size = y_width * y_height * sizeof(uint8_t);//
        uv_size = uv_width * uv_height * sizeof(uint8_t);//
        uint64_t output_size = MAX_NUM_MB_W * MAX_NUM_MB_H * 512 * sizeof(uint16_t); //

        int output_tokens_size = sizeof(uint16_t) * PAGE_COUNT * TOKENS_COUNT_PER_PAGE;

        it.do_trellis_ = 0;//(rd_opt RD_OPT_TRELLIS_ALL);
        AllPicInfo picinfo;//Picture information
        ap_NoneZero ap_nz;

        Set_AllPicInfo( &picinfo, enc);//Set picture information
        //int size_info = sizeof(AllPicInfo);
        uint8_t* ysrc;
        uint8_t* usrc;
        uint8_t* vsrc;
        int16_t* pout_level;
        uint8_t* pout_out;
        uint8_t* pout_pred;
        uint8_t* pout_ret;
        pout_level = malloc( MAX_NUM_MB_W * MAX_NUM_MB_H  * 512 * sizeof(int16_t));
        int16_t* pt= pout_level;

        size_t globalSize[] = {1, 1, 1};
        size_t localSize[] = {1, 1, 1};

        output_size = MAX_NUM_MB_W * MAX_NUM_MB_H * 512 * sizeof(uint16_t); //

        StartProfiling(&watch);

        cl_int err;
        err = clEnqueueWriteBuffer(hardware.mQueue, enclooppara.input, CL_FALSE, 0, sizeof(AllPicInfo), &picinfo, 0, NULL, NULL);
        if(CL_SUCCESS != err) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
            goto Err;
        }

        watch_time = 0.0;
        StopProfiling(&watch, &watch_time, &watch_count);
        printf("INFO: COPY .input to Buffer. Computation time is %f (ms) \n", watch_time);

        StartProfiling(&watch);

        err = clEnqueueWriteBuffer(hardware.mQueue, enclooppara.y, CL_FALSE, 0, y_size, enc->pic_->y, 0, NULL, NULL);
        if(CL_SUCCESS != err) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
            goto Err;
        }

        watch_time = 0.0;
        StopProfiling(&watch, &watch_time, &watch_count);
        printf("INFO: COPY .y to Buffer. Computation time is %f (ms) \n", watch_time);

        StartProfiling(&watch);

        err = clEnqueueWriteBuffer(hardware.mQueue, enclooppara.u, CL_FALSE, 0, uv_size, enc->pic_->u, 0, NULL, NULL);
        if(CL_SUCCESS != err) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
            goto Err;
        }

        watch_time = 0.0;
        StopProfiling(&watch, &watch_time, &watch_count);
        printf("INFO: COPY .u to Buffer. Computation time is %f (ms) \n", watch_time);

        StartProfiling(&watch);

        err = clEnqueueWriteBuffer(hardware.mQueue, enclooppara.v, CL_FALSE, 0, uv_size, enc->pic_->v, 0, NULL, NULL);
        if(CL_SUCCESS != err) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
            goto Err;
        }

        watch_time = 0.0;
        StopProfiling(&watch, &watch_time, &watch_count);
        printf("INFO: COPY .v to Buffer. Computation time is %f (ms) \n", watch_time);

        StartProfiling(&watch);

        // for(int rr=0;rr<1000;rr++){

        // launch kernel
        err = clEnqueueNDRangeKernel(hardware.mQueue, encloop.mKernel, 1, 0,
                                     globalSize, localSize, 0, NULL, NULL);
        if (err != CL_SUCCESS) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
            goto Err;
        }

        err = clFinish(hardware.mQueue);
        if (err != CL_SUCCESS) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
            goto Err;
        }
        // }

        watch_time = 0.0;
        StopProfiling(&watch, &watch_time, &watch_count);
        printf("\nINFO: Kernel Finished. Computation time is %f (ms) \n\n", watch_time);

        StartProfiling(&watch);

        err = clEnqueueReadBuffer(hardware.mQueue, enclooppara.y, CL_FALSE, 0, y_size, enc->pic_->y, 0, NULL, NULL);
        if(CL_SUCCESS != err) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
            goto Err;
        }

        watch_time = 0.0;
        StopProfiling(&watch, &watch_time, &watch_count);
        printf("INFO: COPY .y to Host. Computation time is %f (ms) \n", watch_time);

        StartProfiling(&watch);

        err = clEnqueueReadBuffer(hardware.mQueue, enclooppara.output, CL_FALSE, 0, output_size, pout_level, 0, NULL, NULL);
        if(CL_SUCCESS != err) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
            goto Err;
        }

        watch_time = 0.0;
        StopProfiling(&watch, &watch_time, &watch_count);
        printf("INFO: COPY .output to Host. Computation time is %f (ms) \n", watch_time);

        do {
            int x_mb = it.x_;
            int y_mb = it.y_;

            ap_uint<6> ret     = (ap_uint<6>)pt[416];
            it.mb_->uv_mode_   = ret(3,0);//it_m.ap_uv_mode_c;
            it.mb_->type_      = ret(4,4);
            it.mb_->skip_      = ret(5,5);//(it_r.ap_nz == 0);
            for(int y=0; y<4 ; y++){
                for(int x=0; x<4 ; x++){
                    it.preds_[x + it.enc_->preds_w_*y ] = pt[400+y*4+x];//SB_GET(mode_b,y,x,WD_MODE);
                }
            }
            FinalizeTokenProbas(&enc->proba_);
            ok = RecordTokens_nrd2(enc, &ap_nz, x_mb, y_mb, it.mb_->type_, pt, pt+16, pt+16*17, &enc->tokens_);
            pt+=512;

        } while (ok && VP8IteratorNext(&it));

        //FinalizeTokenProbas(&enc->proba_);//This is about AC
        ok = VP8EmitTokens(&enc->tokens_, enc->parts_+0,(const uint8_t*)enc->proba_.coeffs_, 1);

        PostLoopFinalize(&it, ok);//This functions

        /*
          #if TOKEN_RECONSTRUCT
          ReadTokenFromKernel(&enc->tokens_, &tokens_);
          #endif
        */


#ifdef USE_C_KERNEL

    Err:
        releaseKernel(encloop);
        clReleaseMemObject(enclooppara.input);
        clReleaseMemObject(enclooppara.y);
        clReleaseMemObject(enclooppara.u);
        clReleaseMemObject(enclooppara.v);
        clReleaseMemObject(enclooppara.output);

        return ok;
#endif

    }

    int VP8EncTokenLoop_ryanw_k2(VP8Encoder* const enc) {

        printf(" In VP8EncTokenLoop_ryanw_k in frame.c \n");

        // Roughly refresh the proba eight times per pass
        /*  int max_count = (enc->mb_w_ * enc->mb_h_) >> 3;
            int num_pass_left = enc->config_->pass;
            const int do_search = 0;//enc->do_search_;

            VP8EncIterator it;
            VP8EncProba* const proba = &enc->proba_;
            const VP8RDLevel rd_opt = enc->rd_opt_level_;
            const uint64_t pixel_count = enc->mb_w_ * enc->mb_h_ * 384;

            PassStats stats;
            int ok;
            StopProfilingWatch stop_watch;
            StartProfiling(&stop_watch);

            InitPassStats(enc, &stats);
            ok = PreLoopInitialize(enc);
            if (!ok) return 0;

            if (max_count < MIN_COUNT) max_count = MIN_COUNT;

            assert(enc->num_parts_ == 1);
            assert(enc->use_tokens_);
            assert(proba->use_skip_proba_ == 0);
            assert(rd_opt >= RD_OPT_BASIC);   // otherwise, token-buffer won't be useful
            assert(num_pass_left > 0);

            while (ok && num_pass_left-- > 0) {

            const int is_last_pass = (fabs(stats.dq) <= DQ_LIMIT) ||
            (num_pass_left == 0) ||
            (enc->max_i4_header_bits_ == 0);
            uint64_t size_p0 = 0;
            uint64_t distortion = 0;
            int cnt = max_count;

            VP8IteratorInit(enc, &it);
            SetLoopParams(enc, stats.q);

            if (is_last_pass) {
            ResetTokenStats(enc);
            VP8InitFilter(&it);  // don't collect stats until last pass (too costly)
            }

            VP8TBufferClear(&enc->tokens_);*/


        int i, j, index;

        cl_int err;
        EncloopInputData input_data;
        // EncloopSegmentData segment_data;
        // EncLoopOutputData output_data;
        // VP8TBufferKernel output_tokens;

        // VP8EncMatrix matrix_y1[NUM_MB_SEGMENTS];
        // VP8EncMatrix matrix_y2[NUM_MB_SEGMENTS];
        // VP8EncMatrix matrix_uv[NUM_MB_SEGMENTS];

        const int xsize = enc->pic_->width;
        const int ysize = enc->pic_->height;

        const int mb_w = (xsize + 15) >> 4; // nb of blocks in x
        const int mb_h = (ysize + 15) >> 4; // nb of blocks in y
        const int num_mb = mb_w * mb_h;

        const int preds_w = 4 * mb_w + 1; // prediction size in x
        const int preds_h = 4 * mb_h + 1; // prediction size in y

        const int y_width = xsize;
        const int y_height = ysize;

        const int uv_width = (xsize + 1) >> 1;
        const int uv_height = (ysize + 1) >> 1;

        const int y_stride = y_width;
        const int uv_stride = uv_width;

        const int expand_yheight = RoundUp(ysize, 16);
        const int expand_uvheight = RoundUp(uv_height, 8);

        uint64_t y_size = 0;
        uint64_t uv_size = 0;

        int mb_size = 0;
        int preds_size = 0;
        int nz_size = 0;
        int top_data_size = 0;
        int lf_stats_size = 0;
        int quant_matrix_size = 0;
        int coeffs_size = 0;
        int stats_size = 0;
        int level_cost_size = 0;
        int bw_buf_size = 0;
        int sse_size = 0;
        int block_count_size = 0;
        int extra_info_size = 0;
        int max_edge_size = 0;
        int bit_count_size = 0;
        int expand_y_size = 0;
        int expand_uv_size = 0;
        int input_size = 0;

        // bits size
        y_size = y_width * y_height * sizeof(uint8_t);
        uv_size = uv_width * uv_height * sizeof(uint8_t);
        //mb_size = mb_w * mb_h * sizeof(uint8_t);
        // preds_size = preds_w * preds_h * sizeof(uint8_t) + preds_w + 1;
        //nz_size = (mb_w + 1 + 1) * sizeof(uint32_t) /*+ WEBP_ALIGN_CST*/;
        /*    top_data_size = mb_w * 16 * sizeof(uint8_t);
              lf_stats_size = NUM_MB_SEGMENTS * MAX_LF_LEVELS * sizeof(double);
              quant_matrix_size = sizeof(VP8EncMatrix);
              coeffs_size = NUM_TYPES * NUM_BANDS * NUM_CTX * NUM_PROBAS * sizeof(uint8_t);
              stats_size = NUM_TYPES * NUM_BANDS * NUM_CTX * NUM_PROBAS * sizeof(uint32_t);
              level_cost_size = NUM_TYPES * NUM_BANDS * NUM_CTX * (MAX_VARIABLE_LEVEL + 1) * sizeof(uint16_t);
              bw_buf_size = 408000 * sizeof(uint8_t);
              sse_size = 4 * sizeof(uint64_t);
              block_count_size = 3 * sizeof(int);
              extra_info_size = mb_w * mb_h * sizeof(uint8_t);
              max_edge_size = NUM_MB_SEGMENTS * sizeof(int);
              bit_count_size = 4 * 3 * sizeof(uint64_t);
              input_size = sizeof(EncloopInputData);*/
        int output_size = MAX_NUM_MB_W * MAX_NUM_MB_H * 1024 * sizeof(uint16_t); //

        int output_tokens_size = sizeof(uint16_t) * PAGE_COUNT * TOKENS_COUNT_PER_PAGE;

        /*  input_data.width = xsize;
            input_data.height = ysize;
            input_data.filter_sharpness = enc->config_->filter_sharpness;
            input_data.show_compressed = enc->config_->show_compressed;
            input_data.extra_info_type = enc->pic_->extra_info_type;
            input_data.stats_add = enc->pic_->stats;
            input_data.simple = enc->filter_hdr_.simple_;
            input_data.num_parts = enc->num_parts_;
            input_data.max_i4_header_bits = enc->max_i4_header_bits_;

            if (enc->lf_stats_ == NULL) {
            input_data.lf_stats_status = 0;
            } else {
            input_data.lf_stats_status = 1;
            }

            input_data.use_skip_proba = !enc->proba_.use_skip_proba_;
            input_data.method = enc->method_;
            input_data.rd_opt = (int)enc->rd_opt_level_;

            for (i = 0; i < NUM_MB_SEGMENTS; i++) {

            segment_data.quant[i] = enc->dqm_[i].quant_;
            segment_data.fstrength[i] = enc->dqm_[i].fstrength_;
            segment_data.max_edge[i] = enc->dqm_[i].max_edge_;
            segment_data.min_disto[i] = enc->dqm_[i].min_disto_;
            segment_data.lambda_i16[i] = enc->dqm_[i].lambda_i16_;
            segment_data.lambda_i4[i] = enc->dqm_[i].lambda_i4_;
            segment_data.lambda_uv[i] = enc->dqm_[i].lambda_uv_;
            segment_data.lambda_mode[i] = enc->dqm_[i].lambda_mode_;
            segment_data.tlambda[i] = enc->dqm_[i].tlambda_;
            segment_data.lambda_trellis_i16[i] = enc->dqm_[i].lambda_trellis_i16_;
            segment_data.lambda_trellis_i4[i] = enc->dqm_[i].lambda_trellis_i4_;
            segment_data.lambda_trellis_uv[i] = enc->dqm_[i].lambda_trellis_uv_;
            }

            expand_y_size = (expand_yheight - ysize) * xsize;
            uint8_t expand_y[expand_y_size];
            if (expand_yheight > ysize) {
            for (i = 0; i < expand_yheight - ysize; i++) {
            memcpy(expand_y + i * xsize, enc->pic_->y + xsize * (ysize - 1), xsize);
            }
            }

            // copy expanded block
            expand_uv_size = (expand_uvheight - uv_height) * uv_width;
            uint8_t expand_u[expand_uv_size];
            uint8_t expand_v[expand_uv_size];
            if (expand_uvheight > uv_height) {
            for (i = 0; i < expand_uvheight - uv_height; i++) {
            memcpy(expand_u + i * uv_width, enc->pic_->u + uv_width * (uv_height - 1), uv_width);
            memcpy(expand_v + i * uv_width, enc->pic_->v + uv_width * (uv_height - 1), uv_width);
            }
            }

            uint8_t mb_info[5 * mb_w * mb_h];
            for (index = 0; index < mb_size; index++) {
            mb_info[5 * index + 0] = enc->mb_info_[index].type_;
            mb_info[5 * index + 1] = enc->mb_info_[index].uv_mode_;
            mb_info[5 * index + 2] = enc->mb_info_[index].skip_;
            mb_info[5 * index + 3] = enc->mb_info_[index].segment_;
            mb_info[5 * index + 4] = enc->mb_info_[index].alpha_;
            }

            for (i = 0; i < NUM_MB_SEGMENTS; i++) {
            VP8Matrix* matrix = &(enc->dqm_[i].y1_);
            for (j = 0; j < 16; j++) {
            matrix_y1[i].q_[j] = matrix->q_[j];
            matrix_y1[i].iq_[j] = matrix->iq_[j];
            matrix_y1[i].bias_[j] = matrix->bias_[j];
            matrix_y1[i].zthresh_[j] = matrix->zthresh_[j];
            matrix_y1[i].sharpen_[j] = matrix->sharpen_[j];
            }
            }

            for (i = 0; i < NUM_MB_SEGMENTS; i++) {
            VP8Matrix* matrix = &(enc->dqm_[i].y2_);
            for (j = 0; j < 16; j++) {
            matrix_y2[i].q_[j] = matrix->q_[j];
            matrix_y2[i].iq_[j] = matrix->iq_[j];
            matrix_y2[i].bias_[j] = matrix->bias_[j];
            matrix_y2[i].zthresh_[j] = matrix->zthresh_[j];
            matrix_y2[i].sharpen_[j] = matrix->sharpen_[j];
            }
            }

            for (i = 0; i < NUM_MB_SEGMENTS; i++) {
            VP8Matrix* matrix = &(enc->dqm_[i].uv_);
            for (j = 0; j < 16; j++) {
            matrix_uv[i].q_[j] = matrix->q_[j];
            matrix_uv[i].iq_[j] = matrix->iq_[j];
            matrix_uv[i].bias_[j] = matrix->bias_[j];
            matrix_uv[i].zthresh_[j] = matrix->zthresh_[j];
            matrix_uv[i].sharpen_[j] = matrix->sharpen_[j];
            }
            }

            output_data.range = enc->parts_[0].range_;
            output_data.value = enc->parts_[0].value_;
            output_data.run = enc->parts_[0].run_;
            output_data.nb_bits = enc->parts_[0].nb_bits_;
            output_data.pos = enc->parts_[0].pos_;
            output_data.max_pos = enc->parts_[0].max_pos_;
            output_data.error = enc->parts_[0].error_;

            uint8_t y_top[mb_w * 16];
            uint8_t uv_top[mb_w * 16];

            memset(y_top, 127, top_data_size);
            memset(uv_top, 127, top_data_size);

            int max_edge_data[NUM_MB_SEGMENTS];
            for (i = 0; i < NUM_MB_SEGMENTS; i++) {
            max_edge_data[i] = enc->dqm_[i].max_edge_;
            }
            uint64_t bit_count[4][3];*/

        size_t globalSize[] = {1, 1, 1};
        size_t localSize[] = {1, 1, 1};

        VP8EncIterator it;
        PassStats stats;
        int ok;
        InitPassStats(enc, &stats);
        ok = PreLoopInitialize(enc);
        if (!ok) return 0;
        VP8IteratorInit(enc, &it);
        SetLoopParams(enc, stats.q);
        ResetTokenStats(enc);
        VP8TBufferClear(&enc->tokens_);

        it.do_trellis_ = 0;//(rd_opt RD_OPT_TRELLIS_ALL);
        AllPicInfo picinfo;//Picture information
        ap_NoneZero ap_nz;

        Set_AllPicInfo( &picinfo, enc);//Set picture information
        int size_info = sizeof(AllPicInfo);
        /*--Following should be the interface of kernel -----------------------------------------------------------------*/
        //int p_info[64];
        uint8_t* ysrc;
        uint8_t* usrc;
        uint8_t* vsrc;
        int16_t* pout_level;
        uint8_t* pout_out;
        uint8_t* pout_pred;
        uint8_t* pout_ret;
        pout_level = malloc( num_mb * 512 * sizeof(int16_t));
        int16_t* pt=pout_level;
        if(pout_level==NULL){
            fprintf(stderr, "pout_level==NULL\n");
            goto Err;
        }
        // copy buffer

        // err = clEnqueueWriteBuffer(hardware.mQueue, enclooppara.input, CL_FALSE, 0, sizeof(EncloopInputData), &input_data, 0, NULL, NULL);
        err = clEnqueueWriteBuffer(hardware.mQueue, enclooppara.input, CL_FALSE, 0, sizeof(AllPicInfo), &picinfo, 0, NULL, NULL);
        if(CL_SUCCESS != err) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
            goto Err;
        }

        err = clEnqueueWriteBuffer(hardware.mQueue, enclooppara.y, CL_FALSE, 0, y_size, enc->pic_->y, 0, NULL, NULL);
        if(CL_SUCCESS != err) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
            goto Err;
        }
        /*
          if (expand_yheight > y_height) {
          err = clEnqueueWriteBuffer(hardware.mQueue, enclooppara.y, CL_FALSE, y_size, expand_y_size, expand_y, 0, NULL, NULL);
          if(CL_SUCCESS != err) {
          fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
          ok = 0;
          goto Err;
          }
          }*/

        err = clEnqueueWriteBuffer(hardware.mQueue, enclooppara.u, CL_FALSE, 0, uv_size, enc->pic_->u, 0, NULL, NULL);
        if(CL_SUCCESS != err) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
            goto Err;
        }
        /*
          if (expand_uvheight > uv_height) {
          err = clEnqueueWriteBuffer(hardware.mQueue, enclooppara.u, CL_FALSE, uv_size, expand_uv_size, expand_u, 0, NULL, NULL);
          if(CL_SUCCESS != err) {
          fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
          ok = 0;
          goto Err;
          }
          }*/

        err = clEnqueueWriteBuffer(hardware.mQueue, enclooppara.v, CL_FALSE, 0, uv_size, enc->pic_->v, 0, NULL, NULL);
        if(CL_SUCCESS != err) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
            goto Err;
        }
        /*
          if (expand_uvheight > uv_height) {
          err = clEnqueueWriteBuffer(hardware.mQueue, enclooppara.v, CL_FALSE, uv_size, expand_uv_size, expand_v, 0, NULL, NULL);
          if(CL_SUCCESS != err) {
          fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
          ok = 0;
          goto Err;
          }
          }
        */
        /* err = clEnqueueWriteBuffer(hardware.mQueue, enclooppara.output_tokens, CL_FALSE, 0, output_tokens_size, output_tokens.tokens_, 0, NULL, NULL); */
        /* if(CL_SUCCESS != err) { */
        /*  fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
        /*  ok = 0; */
        /*  goto Err; */
        /* } */
        // *********************************** run kernel ********************************

        err = clEnqueueNDRangeKernel(hardware.mQueue, encloop.mKernel, 1, 0,
                                     globalSize, localSize, 0, NULL, NULL);
        if (err != CL_SUCCESS) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
            goto Err;
        }

        output_size = MAX_NUM_MB_W * MAX_NUM_MB_H * 1024 * sizeof(uint16_t); //

        // *************************************************************************


        // read buffer from device

        fprintf(stderr, "start enctokenloop clFinish\n");
        err = clFinish(hardware.mQueue);
        if (err != CL_SUCCESS) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
            goto Err;
        }
        fprintf(stderr, "stop enctokenloop clFinish\n");
        /*
          err = clEnqueueReadBuffer(hardware.mQueue, enclooppara.y, CL_FALSE, 0, y_size, enc->pic_->y, 0, NULL, NULL);
          if(CL_SUCCESS != err) {
          fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
          ok = 0;
          goto Err;
          }

          err = clEnqueueReadBuffer(hardware.mQueue, enclooppara.u, CL_FALSE, 0, uv_size, enc->pic_->u, 0, NULL, NULL);
          if(CL_SUCCESS != err) {
          fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
          ok = 0;
          goto Err;
          }

          err = clEnqueueReadBuffer(hardware.mQueue, enclooppara.v, CL_FALSE, 0, uv_size, enc->pic_->v, 0, NULL, NULL);
          if(CL_SUCCESS != err) {
          fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
          ok = 0;
          goto Err;
          }
        */
        // err = clEnqueueReadBuffer(hardware.mQueue, enclooppara.output, CL_FALSE, 0, output_size, output_tokens.tokens_, 0, NULL, NULL);
        err = clEnqueueReadBuffer(hardware.mQueue, enclooppara.output, CL_FALSE, 0, output_size, pout_level, 0, NULL, NULL);
        if(CL_SUCCESS != err) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
            goto Err;
        }

        /* output_tokens.error_ = output_data.error_; */
        /* enc->tokens_.left_ = output_data.left_; */
        /* enc->tokens_.page_size_ = output_data.page_size_; */
        /* enc->tokens_.error_ = output_data.error_; */
        /* ReadTokenFromKernel(&enc->tokens_, &output_tokens); */
        do {
            ap_uint<LG2_MAX_NUM_MB_W> x_mb = it.x_;
            ap_uint<LG2_MAX_NUM_MB_W> y_mb = it.y_;

            ap_uint<6> ret     = (ap_uint<6>)pt[416];
            it.mb_->uv_mode_   = ret(3,0);//it_m.ap_uv_mode_c;
            it.mb_->type_      = ret(4,4);
            it.mb_->skip_      = ret(5,5);//(it_r.ap_nz == 0);
            for(int y=0; y<4 ; y++){
                for(int x=0; x<4 ; x++){
                    it.preds_[x + it.enc_->preds_w_*y ] = pt[400+y*4+x];//SB_GET(mode_b,y,x,WD_MODE);
                }
            }

            ok = RecordTokens_nrd2(enc, &ap_nz, x_mb, y_mb, it.mb_->type_, pt, pt+16, pt+16*17, &enc->tokens_);
            pt+=512;

        } while (ok && VP8IteratorNext(&it));

        FinalizeTokenProbas(&enc->proba_);//This is about AC
        ok = VP8EmitTokens(&enc->tokens_, enc->parts_+0,(const uint8_t*)enc->proba_.coeffs_, 1);
        PostLoopFinalize(&it, ok);//This functions

    Err:
        releaseKernel(encloop);
        clReleaseMemObject(enclooppara.input);
        clReleaseMemObject(enclooppara.y);
        clReleaseMemObject(enclooppara.u);
        clReleaseMemObject(enclooppara.v);
        /* clReleaseMemObject(enclooppara.mb_info); */
        /* clReleaseMemObject(enclooppara.preds); */
        /* clReleaseMemObject(enclooppara.nz); */
        /* clReleaseMemObject(enclooppara.y_top); */
        /* clReleaseMemObject(enclooppara.uv_top); */
        /* clReleaseMemObject(enclooppara.quant_matrix); */
        /* clReleaseMemObject(enclooppara.coeffs); */
        /* clReleaseMemObject(enclooppara.stats); */
        /* clReleaseMemObject(enclooppara.level_cost); */
        /* clReleaseMemObject(enclooppara.bw_buf); */
        /* clReleaseMemObject(enclooppara.sse); */
        /* clReleaseMemObject(enclooppara.block_count); */
        /* clReleaseMemObject(enclooppara.extra_info); */
        /* clReleaseMemObject(enclooppara.max_edge); */
        /* clReleaseMemObject(enclooppara.bit_count); */
        /* clReleaseMemObject(enclooppara.sse_count); */
        /* clReleaseMemObject(enclooppara.output_data); */
        /* clReleaseMemObject(enclooppara.output_tokens); */
        clReleaseMemObject(enclooppara.output);

        return ok;


    }

    //------------------------------------------------------------------------------
    int VP8EncTokenLoopAsyncDevice2Host(VP8Encoder* const & enc, const int buf,
                                        // int16_t* & pout_level,
                                        uint8_t* & pout_prob,
                                        uint8_t* & pout_bw,
                                        uint8_t* & pout_ret,
                                        uint8_t* & pout_pred,
                                        cl_uint num_wait_event, cl_event* wait_event, std::array<cl_event,4> & event){

        StopProfilingWatch watch;
        double watch_time;
        int watch_count;

        int ok;
        cl_int err;
        ap_NoneZero ap_nz;

        const int xsize = enc->pic_->width;
        const int ysize = enc->pic_->height;
        const int y_width = xsize;
        const int y_height = ysize;
        const int uv_width = (xsize + 1) >> 1;
        const int uv_height = (ysize + 1) >> 1;

        // bits size
        const uint64_t y_size = y_width * y_height * sizeof(uint8_t);
        const uint64_t uv_size = uv_width * uv_height * sizeof(uint8_t);
        const uint64_t output_size = MAX_NUM_MB_W * MAX_NUM_MB_H * 512 * sizeof(uint16_t);
        const uint64_t output_size_prob = SIZE8_MEM_PROB * sizeof(uint8_t);
        const uint64_t output_size_bw = SIZE8_MEM_BW;
        const uint64_t output_size_ret = SIZE8_MEM_RET;
        const uint64_t output_size_pred = SIZE8_MEM_PRED;

        //=============================================================//
        StartProfiling(&watch);

        /* err = clEnqueueMigrateMemObjects(hardware.mQueue, 1, &(encloopparaAsync[buf].y), CL_MIGRATE_MEM_OBJECT_HOST, */
        /*                               num_wait_event, wait_event, &event[0]); */

        /* err = clEnqueueReadBuffer(hardware.mQueue, encloopparaAsync[buf].y, CL_FALSE, 0, y_size, enc->pic_->y, */
        /*                        num_wait_event, wait_event, &event[0]); */
        /* if(CL_SUCCESS != err) { */
        /*   fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
        /*   ok = 0; */
        /*   //   goto Err; */
        /* } */
        /* printf("INFO: COPY .y to Host.\n"); */

        //=============================================================//

        /* err = clEnqueueMigrateMemObjects(hardware.mQueue, 1, &(encloopparaAsync[buf].output), CL_MIGRATE_MEM_OBJECT_HOST, */
        /*                               num_wait_event, wait_event, &event[1]); */

        /* err = clEnqueueReadBuffer(hardware.mQueue, encloopparaAsync[buf].output, CL_FALSE, 0, output_size, pout_level, */
        /*                        num_wait_event, wait_event, &event[1]); */

        /* if(CL_SUCCESS != err) { */
        /*   fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
        /*   ok = 0; */
        /*   //   goto Err; */
        /* } */
        /* printf("INFO: COPY .output to Host.\n"); */

        //=============================================================//

        /* err = clEnqueueMigrateMemObjects(hardware.mQueue, 1, &(encloopparaAsync[buf].output_prob), CL_MIGRATE_MEM_OBJECT_HOST, */
        /*                               num_wait_event, wait_event, &event[1]); */

        err = clEnqueueReadBuffer(hardware.mQueue, encloopparaAsync[buf].output_prob, CL_FALSE, 0, output_size_prob, pout_prob,
                                  num_wait_event, wait_event, &event[0]);
        if(CL_SUCCESS != err) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
            //      goto Err;
        }
        printf("INFO: COPY .output_prob to Host.\n");

        //bw=============================================================//

        /* err = clEnqueueMigrateMemObjects(hardware.mQueue, 1, &(encloopparaAsync[buf].output_bw), CL_MIGRATE_MEM_OBJECT_HOST, */
        /*                               num_wait_event, wait_event, &event[2]); */

        err = clEnqueueReadBuffer(hardware.mQueue, encloopparaAsync[buf].output_bw, CL_FALSE, 0, output_size_bw, pout_bw,
                                  num_wait_event, wait_event, &event[1]);
        if(CL_SUCCESS != err) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
            //      goto Err;
        }
        printf("INFO: COPY .output_bw to Host.\n");

        //ret==========================================================//

        /* err = clEnqueueMigrateMemObjects(hardware.mQueue, 1, &(encloopparaAsync[buf].output_ret), CL_MIGRATE_MEM_OBJECT_HOST, */
        /*                               num_wait_event, wait_event, &event[3]); */

        err = clEnqueueReadBuffer(hardware.mQueue, encloopparaAsync[buf].output_ret, CL_FALSE, 0, output_size_ret, pout_ret,
                                  num_wait_event, wait_event, &event[2]);
        if(CL_SUCCESS != err) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
            //      goto Err;
        }
        printf("INFO: COPY .output_ret to Host.\n");

        //pred=========================================================//

        /* err = clEnqueueMigrateMemObjects(hardware.mQueue, 1, &(encloopparaAsync[buf].output_pred), CL_MIGRATE_MEM_OBJECT_HOST, */
        /*                               num_wait_event, wait_event, &event[4]); */

        err = clEnqueueReadBuffer(hardware.mQueue, encloopparaAsync[buf].output_pred, CL_FALSE, 0, output_size_pred, pout_pred,
                                  num_wait_event, wait_event, &event[3]);
        if(CL_SUCCESS != err) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
            //      goto Err;
        }
        printf("INFO: COPY .output_pred to Host.\n");
        //=============================================================//

        watch_time = 0.0;
        StopProfiling(&watch, &watch_time, &watch_count);
        printf("INFO: Device2Host finished. Computation time is %f (ms) \n", watch_time);


        /* // Flush */
        /* err = clFlush(hardware.mQueue); */
        /* if (err != CL_SUCCESS) { */
        /*   fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
        /*   ok = 0; */
        /* } */


        /*int t, b, c, p, id=0;
          for (t = 0; t < 4; ++t)
          #pragma HLS UNROLL
          for (b = 0; b < 8; ++b)
          for (c = 0; c < 3; ++c){
          for (p = 0; p < 11; ++p) {
          printf("%d: %d ; ", id++,pout_prob[t*8*3*11+b*3*11+c*11+p]);
          }
          printf("t=%d, b=%d, c=%d\n",t,b,c);
          }*/

        /* err = clFinish(hardware.mQueue); */
        /* if (err != CL_SUCCESS) { */
        /*   fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
        /*   ok = 0; */
        /* } */
    };

    int VP8EncTokenLoopAsyncPredKernel(VP8Encoder* const & enc, const int buf,
                                       cl_uint num_wait_event, cl_event* wait_event, std::array<cl_event,1> & event) {

        int ok;
        cl_int err;

        StopProfilingWatch watch;
        double watch_time;
        int watch_count;

        const int xsize = enc->pic_->width;
        const int ysize = enc->pic_->height;
        const int y_width = xsize;
        const int y_height = ysize;
        const int uv_width = (xsize + 1) >> 1;
        const int uv_height = (ysize + 1) >> 1;

        // bits size
        const uint64_t y_size = y_width * y_height * sizeof(uint8_t);//
        const uint64_t uv_size = uv_width * uv_height * sizeof(uint8_t);//
        const uint64_t output_size = MAX_NUM_MB_W * MAX_NUM_MB_H * 512 * sizeof(uint16_t); //

        StartProfiling(&watch);

        // Set args
        int arg = 0;
        int status ;
        err = clSetKernelArg(encloop.mKernelPred[buf], arg++, sizeof(cl_mem), &(encloopparaAsync[buf].input));
        if (err != CL_SUCCESS) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            status = -1;
            //    goto Err;
        }

        err = clSetKernelArg(encloop.mKernelPred[buf], arg++, sizeof(cl_mem), &(encloopparaAsync[buf].y));
        if (err != CL_SUCCESS) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            status = -1;
            //    goto Err;
        }

        err = clSetKernelArg(encloop.mKernelPred[buf], arg++, sizeof(cl_mem), &(encloopparaAsync[buf].u));
        if (err != CL_SUCCESS) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            status = -1;
            //    goto Err;
        }

        err = clSetKernelArg(encloop.mKernelPred[buf], arg++, sizeof(cl_mem), &(encloopparaAsync[buf].v));
        if (err != CL_SUCCESS) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            status = -1;
            //    goto Err;
        }

        err = clSetKernelArg(encloop.mKernelPred[buf], arg++, sizeof(cl_mem), &(encloopparaAsync[buf].output));
        if (err != CL_SUCCESS) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            status = -1;
            //    goto Err;
        }
        err = clSetKernelArg(encloop.mKernelPred[buf], arg++, sizeof(cl_mem), &(encloopparaAsync[buf].output_prob));
        if (err != CL_SUCCESS) {
            fprintf(stderr, "%s %d %s :output_prob \n", __func__, __LINE__, oclErrorCode(err));
            status = -1;
            //    goto Err;
        }
        // launch kernel
        size_t globalSize[] = {1, 1, 1};
        size_t localSize[] = {1, 1, 1};


        StartProfiling(&watch);

        err = clEnqueueNDRangeKernel(hardware.mQueue, encloop.mKernelPred[buf], 1, 0,
                                     globalSize, localSize, num_wait_event, wait_event, &event[0]);
        if (err != CL_SUCCESS) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
        }

        /* err = clFlush(hardware.mQueue); */
        /* if (err != CL_SUCCESS) { */
        /*   fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
        /*   ok = 0; */
        /* } */

        /* err = clFlush(hardware.mQueue); */
        /* if (err != CL_SUCCESS) { */
        /*   fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
        /*   ok = 0; */
        /* } */

        watch_time = 0.0;
        StopProfiling(&watch, &watch_time, &watch_count);
        printf("\nINFO: PredKernel Finished. Computation time is %f (ms) \n", watch_time);

        /* #ifdef USE_C_KERNEL */
        /*   Err: */
        /*      /\* releaseKernel(encloop); *\/ */
        /*      /\* clReleaseMemObject(enclooppara.input); *\/ */
        /*      /\* clReleaseMemObject(enclooppara.y); *\/ */
        /*      /\* clReleaseMemObject(enclooppara.u); *\/ */
        /*      /\* clReleaseMemObject(enclooppara.v); *\/ */
        /*      /\* clReleaseMemObject(enclooppara.output); *\/ */
        /*      return ok; */
        /* #endif */

        ok = 1;
        return ok;
    }


    int VP8EncTokenLoopAsyncACKernel(VP8Encoder* enc, const int buf,
                                     cl_uint num_wait_event, cl_event* wait_event, std::array<cl_event,1> & event) {

        int ok;
        cl_int err;
    
        StopProfilingWatch watch;
        double watch_time;
        int watch_count;

        StartProfiling(&watch);

        int arg=0;
        int status;

        //1)
        err = clSetKernelArg(encloop.mKernelAC[buf], arg++, sizeof(cl_mem), &(encloopparaAsync[buf].output));
        if (err != CL_SUCCESS) {
            fprintf(stderr, "%s %d %s :output (level) \n", __func__, __LINE__, oclErrorCode(err));
            status = -1;
        }
        //2)
        err = clSetKernelArg(encloop.mKernelAC[buf], arg++, sizeof(cl_mem), &(encloopparaAsync[buf].output_prob));
        if (err != CL_SUCCESS) {
            fprintf(stderr, "%s %d %s :output_prob \n", __func__, __LINE__, oclErrorCode(err));
            status = -1;
        }
        //3)
        err = clSetKernelArg(encloop.mKernelAC[buf], arg++, sizeof(cl_mem), &(encloopparaAsync[buf].output_bw));
        if (err != CL_SUCCESS) {
            fprintf(stderr, "%s %d %s :output_bw \n", __func__, __LINE__, oclErrorCode(err));
            status = -1;
        }
        //4)
        err = clSetKernelArg(encloop.mKernelAC[buf], arg++, sizeof(cl_mem), &(encloopparaAsync[buf].output_ret));
        if (err != CL_SUCCESS) {
            fprintf(stderr, "%s %d %s :output_ret \n", __func__, __LINE__, oclErrorCode(err));
            status = -1;
        }
        //5)
        err = clSetKernelArg(encloop.mKernelAC[buf], arg++, sizeof(cl_mem), &(encloopparaAsync[buf].output_pred));
        if (err != CL_SUCCESS) {
            fprintf(stderr, "%s %d %s :output_pred \n", __func__, __LINE__, oclErrorCode(err));
            status = -1;
        }


        // launch kernel
        size_t globalSize[] = {1, 1, 1};
        size_t localSize[] = {1, 1, 1};

        StartProfiling(&watch);

        err = clEnqueueNDRangeKernel(hardware.mQueue, encloop.mKernelAC[buf], 1, 0,
                                     globalSize, localSize, num_wait_event, wait_event, &event[0]);
        if (err != CL_SUCCESS) {
            fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err));
            ok = 0;
        }

        /* // Flush */
        /* err = clFlush(hardware.mQueue); */
        /* if (err != CL_SUCCESS) { */
        /*   fprintf(stderr, "%s %d %s\n", __func__, __LINE__, oclErrorCode(err)); */
        /*   ok = 0; */
        /* } */

        watch_time = 0.0;
        StopProfiling(&watch, &watch_time, &watch_count);
        printf("INFO: ACKernel Finished. Computation time is %f (ms) \n\n", watch_time);

        ok = 1;
        return ok;
    }

    void VP8BitWriterCpy_hls(VP8BitWriter* const bw_des,uint8_t* const pout_bw) {
        uint32_t* p_bw = (uint32_t*)pout_bw + SIZE32_MEM_BW - SIZE32_AC_STATE;
        bw_des->range_   = p_bw[0];// - 1;
        bw_des->value_   = p_bw[1];
        bw_des->nb_bits_ = p_bw[2];
        bw_des->pos_     = p_bw[3];
        bw_des->run_     = p_bw[4];
        bw_des->max_pos_ = p_bw[5];
        bw_des->error_   = p_bw[6];
        bw_des->buf_     = (uint8_t*)(pout_bw);
    }
    int VP8EncTokenLoopAsyncACKernel_k2(
        VP8Encoder* enc,
        const int buf,
        int16_t* pout_level,
        uint8_t* pout_prob,
        uint8_t* pout_bw,// = malloc(SIZE32_MEM_BW*4);/
        uint8_t* pout_ret,// = malloc(SIZE32_MEM_RET*4);
        uint8_t* pout_pred,// = malloc(SIZE32_MEM_PRED*4);
        VP8EncIterator &it
        )
    {

        VP8IteratorInit(enc, &it);
        it.do_trellis_ = 0;//(rd_opt RD_OPT_TRELLIS_ALL);
        /*
          int16_t* pt = pout_level;

          int ok;
          ap_NoneZero ap_nz;
          do {
          int x_mb = it.x_;
          int y_mb = it.y_;

          ap_uint<6> ret     = (ap_uint<6>)pt[416];
          it.mb_->uv_mode_   = ret(3,0);//it_m.ap_uv_mode_c;
          it.mb_->type_      = ret(4,4);
          it.mb_->skip_      = ret(5,5);//(it_r.ap_nz == 0);
          for(int y=0; y<4 ; y++){
          for(int x=0; x<4 ; x++){
          it.preds_[x + it.enc_->preds_w_*y ] = pt[400+y*4+x];//SB_GET(mode_b,y,x,WD_MODE);
          }
          }
          FinalizeTokenProbas(&enc->proba_);
          ok = RecordTokens_nrd2(enc, &ap_nz, x_mb, y_mb, it.mb_->type_, pt, pt+16, pt+16*17, &enc->tokens_);
          pt += 512;

          } while (ok && VP8IteratorNext(&it));
        */
        // FinalizeTokenProbas(&enc->proba_);//This is about AC

        //  ok = VP8EmitTokens(&enc->tokens_, enc->parts_+0, (const uint8_t*)enc->proba_.coeffs_, 1);
        //ok = VP8EmitTokens(&enc->tokens_, enc->parts_+0, pout_prob, 1);
        // PostLoopFinalize(&it, ok); // This functions

        /*  FILE* fp_prob=fopen("fp_prob_0.dat", "wa");
            fwrite( (const uint8_t*)enc->proba_.coeffs_, 1, 33*32 , fp_prob);
            fclose(fp_prob);

            FILE* fp_prob0=fopen("fp_prob_1.dat", "wa");
            fwrite( (void*)pout_prob, 1, 33*32 , fp_prob0);
            fclose(fp_prob0);*/
        memcpy((uint8_t*)enc->proba_.coeffs_, (uint8_t*)pout_prob, 32*33);
        enc->proba_.dirty_ = pout_prob[SIZE8_MEM_PROB-1];
        kernel_2_ArithmeticCoding(
            (uint32_t*)pout_level,
            (uint8_t*)enc->proba_.coeffs_,//pout_prob,
            (uint32_t*)pout_bw,
            (uint32_t*)pout_ret,
            (uint32_t*)pout_pred );
        VP8BitWriterCpy_hls(enc->parts_+0, pout_bw);

        uint8_t*   p_ret = pout_ret;//_host;
        uint8_t*   p_pred = pout_pred;//_host;
        VP8MBInfo* p_mb_info = enc->mb_info_;   // contextual macroblock infos (mb_w_ + 1)
        uint8_t*   p_preds = enc->preds_;     // predictions modes: (4*mb_w+1) * (4*mb_h+1)
        int16_t* pt=pout_level;

        for(int y_mb = 0; y_mb < enc->mb_h_;y_mb++){
            p_mb_info = enc->mb_info_ + y_mb * enc->mb_w_;
            p_preds = enc->preds_ + y_mb * 4 * enc->preds_w_;
            for(int x_mb=0; x_mb< enc->mb_w_;x_mb++){
                uint8_t ret     = *p_ret++;//(ap_uint<6>)pt[416];
                p_mb_info->uv_mode_   = ret&15;//(3,0);//it_m.ap_uv_mode_c;
                p_mb_info->type_      = (ret&16)>>4;//(4,4);
                p_mb_info->skip_      = (ret&32)>>5;//(5,5);//(it_r.ap_nz == 0);
                for(int y=0; y<4 ; y++){
                    for(int x=0; x<4 ; x+=2){
                        uint8_t tmp = *p_pred++;
                        if((tmp&15)!=pt[400+y*4+x])
                            printf("hahahah\n");
                        p_preds[x + enc->preds_w_*y ] = tmp&15;//pt[400+y*4+x];//tmp&15;//pt[400+y*4+x];//
                        p_preds[x+1 + enc->preds_w_*y ] = (tmp>>4)&15;//pt[400+y*4+x+1];//;(tmp>>4)&15;
                    }
                }
                pt+=512;
                p_preds += 4;
                p_mb_info +=1;
            }
        }
        return 1;
    };

    int VP8EncTokenLoopAsyncAfterAC(
        VP8Encoder* enc,
        uint8_t* pout_prob,
        uint8_t* pout_bw,// = malloc(SIZE32_MEM_BW*4);/
        uint8_t* pout_ret,// = malloc(SIZE32_MEM_RET*4);
        uint8_t* pout_pred,// = malloc(SIZE32_MEM_PRED*4);
        VP8EncIterator &it){

        VP8IteratorInit(enc, &it);
        it.do_trellis_ = 0;//(rd_opt RD_OPT_TRELLIS_ALL);

        memcpy((uint8_t*)enc->proba_.coeffs_, (uint8_t*)pout_prob, 32*33);
        enc->proba_.dirty_ = pout_prob[SIZE8_MEM_PROB-1];

        VP8BitWriterCpy_hls(enc->parts_+0, pout_bw);

        uint8_t*   p_ret = pout_ret;//_host;
        uint8_t*   p_pred = pout_pred;//_host;
        VP8MBInfo* p_mb_info = enc->mb_info_;   // contextual macroblock infos (mb_w_ + 1)
        uint8_t*   p_preds = enc->preds_;     // predictions modes: (4*mb_w+1) * (4*mb_h+1)

        for(int y_mb = 0; y_mb < enc->mb_h_;y_mb++){
            p_mb_info = enc->mb_info_ + y_mb * enc->mb_w_;
            p_preds = enc->preds_ + y_mb * 4 * enc->preds_w_;
            for(int x_mb=0; x_mb< enc->mb_w_;x_mb++){
                uint8_t ret     = *p_ret++;//(ap_uint<6>)pt[416];
                p_mb_info->uv_mode_   = ret&15;//(3,0);//it_m.ap_uv_mode_c;
                p_mb_info->type_      = (ret&16)>>4;//(4,4);
                p_mb_info->skip_      = (ret&32)>>5;//(5,5);//(it_r.ap_nz == 0);
                for(int y=0; y<4 ; y++){
                    for(int x=0; x<4 ; x+=2){
                        uint8_t tmp = *p_pred++;
                        p_preds[x + enc->preds_w_*y ] = tmp&15;//pt[400+y*4+x];//tmp&15;//pt[400+y*4+x];//
                        p_preds[x+1 + enc->preds_w_*y ] = (tmp>>4)&15;//pt[400+y*4+x+1];//;(tmp>>4)&15;
                    }
                }
                p_preds += 4;
                p_mb_info +=1;
            }
        }
        return 1;
    };

