/* Copyright (c) 2022 Amazon
   Written by Jan Buethe */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <string.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "os_support.h"
#include "dred_decoder.h"
#include "dred_coding.h"
#include "celt/entdec.h"
#include "celt/laplace.h"

/* From http://graphics.stanford.edu/~seander/bithacks.html#FixedSignExtend */
static int sign_extend(int x, int b) {
  int m = 1U << (b - 1);
  return (x ^ m) - m;
}

static void dred_decode_latents(ec_dec *dec, float *x, const opus_uint16 *scale, const opus_uint16 *r, const opus_uint16 *p0, int dim) {
    int i;
    for (i=0;i<dim;i++) {
        int q;
        q = ec_laplace_decode_p0(dec, p0[i], r[i]);
        x[i] = q*256.f/(scale[i] == 0 ? 1 : scale[i]);
    }
}

int dred_ec_decode(OpusDRED *dec, const opus_uint8 *bytes, int num_bytes, int min_feature_frames)
{
  const opus_uint16 *p0              = DRED_rdovae_get_p0_pointer();
  const opus_uint16 *quant_scales    = DRED_rdovae_get_quant_scales_pointer();
  const opus_uint16 *r               = DRED_rdovae_get_r_pointer();
  ec_dec ec;
  int q_level;
  int i;
  int offset;
  int q0;
  int dQ;
  int state_qoffset;


  /* since features are decoded in quadruples, it makes no sense to go with an uneven number of redundancy frames */
  celt_assert(DRED_NUM_REDUNDANCY_FRAMES % 2 == 0);

  /* decode initial state and initialize RDOVAE decoder */
  ec_dec_init(&ec, (unsigned char*)bytes, num_bytes);
  dec->dred_offset = sign_extend(ec_dec_uint(&ec, 32), 5);
  q0 = ec_dec_uint(&ec, 16);
  dQ = ec_dec_uint(&ec, 8);
  /*printf("%d %d %d\n", dred_offset, q0, dQ);*/

  //dred_decode_state(&ec, dec->state);
  state_qoffset = q0*(DRED_LATENT_DIM+DRED_STATE_DIM) + DRED_LATENT_DIM;
  dred_decode_latents(
      &ec,
      dec->state,
      quant_scales + state_qoffset,
      r + state_qoffset,
      p0 + state_qoffset,
      DRED_STATE_DIM);

  /* decode newest to oldest and store oldest to newest */
  for (i = 0; i < IMIN(DRED_NUM_REDUNDANCY_FRAMES, (min_feature_frames+1)/2); i += 2)
  {
      /* FIXME: Figure out how to avoid missing a last frame that would take up < 8 bits. */
      if (8*num_bytes - ec_tell(&ec) <= 7)
         break;
      q_level = compute_quantizer(q0, dQ, i/2);
      offset = q_level * (DRED_LATENT_DIM+DRED_STATE_DIM);
      dred_decode_latents(
          &ec,
          &dec->latents[(i/2)*DRED_LATENT_DIM],
          quant_scales + offset,
          r + offset,
          p0 + offset,
          DRED_LATENT_DIM
          );

      offset = 2 * i * DRED_NUM_FEATURES;
  }
  dec->process_stage = 1;
  dec->nb_latents = i/2;
  return i/2;
}
