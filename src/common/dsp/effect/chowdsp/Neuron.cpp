/*
** Surge Synthesizer is Free and Open Source Software
**
** Surge is made available under the Gnu General Public License, v3.0
** https://www.gnu.org/licenses/gpl-3.0.en.html
**
** Copyright 2004-2020 by various individuals as described by the Git transaction log
**
** All source at: https://github.com/surge-synthesizer/surge.git
**
** Surge was a commercial product from 2004-2018, with Copyright and ownership
** in that period held by Claes Johanson at Vember Audio. Claes made Surge
** open source in September 2018.
*/

#include "Neuron.h"

namespace chowdsp
{

Neuron::Neuron(SurgeStorage* storage, FxStorage* fxdata, pdata* pd)
    : Effect(storage, fxdata, pd)
{
    dc_blocker.setBlockSize(BLOCK_SIZE);
    makeup.set_blocksize(BLOCK_SIZE);
    outgain.set_blocksize(BLOCK_SIZE);
}

Neuron::~Neuron()
{}

void Neuron::init()
{
    Wf.reset(numSteps);
    Wh.reset(numSteps);
    Uf.reset(numSteps);
    Uh.reset(numSteps);
    bf.reset(numSteps);

    delay1Smooth.reset(numSteps);
    delay2Smooth.reset(numSteps);

    os.reset();

    delay1.prepare(dsamplerate * os.getOSRatio(), BLOCK_SIZE, 2);
    delay2.prepare(dsamplerate * os.getOSRatio(), BLOCK_SIZE, 2);
    delay1.setDelay(0.0f);
    delay2.setDelay(0.0f);

    y1[0] = 0.0f;
    y1[1] = 0.0f;

    dc_blocker.suspend();
    dc_blocker.coeff_HP(35.0f / samplerate, 0.707);
    dc_blocker.coeff_instantize();

    width.instantize();

    makeup.set_target(1.0f);
    outgain.set_target(0.0f);
}

void Neuron::process(float* dataL, float* dataR)
{
    set_params();

    os.upsample(dataL, dataR);
    process_internal(os.leftUp, os.rightUp, os.getUpBlockSize());
    os.downsample(dataL, dataR);

    dc_blocker.process_block(dataL, dataR);
    makeup.multiply_2_blocks(dataL, dataR, BLOCK_SIZE_QUAD);

    // scale width
    float M alignas(16)[BLOCK_SIZE], S alignas(16)[BLOCK_SIZE];

    encodeMS(dataL, dataR, M, S, BLOCK_SIZE_QUAD);
    width.multiply_block(S, BLOCK_SIZE_QUAD);
    decodeMS(M, S, dataL, dataR, BLOCK_SIZE_QUAD);

    outgain.multiply_2_blocks(dataL, dataR, BLOCK_SIZE_QUAD);
}

void Neuron::process_internal(float* dataL, float* dataR, const int numSamples)
{
    for(int k = 0; k < numSamples; k++)
    {
        dataL[k] = processSample(dataL[k], y1[0]);
        dataR[k] = processSample(dataR[k], y1[1]);

        delay1.setDelay(delay1Smooth.getNextValue());
        delay2.setDelay(delay2Smooth.getNextValue());
        delay1.pushSample(0, dataL[k]);
        delay2.pushSample(1, dataR[k]);

        y1[0] = delay1.popSample(0);
        y1[1] = delay2.popSample(1);
    }
}

void Neuron::set_params()
{
   auto bf_clamped = limit_range(*f[neuron_bias_bf], 0.0f, 1.0f);

    Wf.setTargetValue(*f[neuron_squash_wf] * 20.0f);
   Wh.setTargetValue(db_to_linear(*f[neuron_drive_wh]));
   Uf.setTargetValue(*f[neuron_stab_uf] * 5.0f);
   Uh.setTargetValue(*f[neuron_asym_uh] * 0.9f);
   bf.setTargetValue(bf_clamped * 6.0f - 1.0f);

   // tune delay length
   auto freqHz1 = (2 * 3.14159265358979323846) * 440 *
                  storage->note_to_pitch_ignoring_tuning(*f[neuron_comb_freq]);
   auto freqHz2 =
       (2 * 3.14159265358979323846) * 440 *
       storage->note_to_pitch_ignoring_tuning(*f[neuron_comb_freq] + *f[neuron_comb_sep]);
   auto delayTimeSec1 = 1.0f / (float) freqHz1;
   auto delayTimeSec2 = 1.0f / (float) freqHz2;

   delay1Smooth.setTargetValue(delayTimeSec1 * 0.5f * samplerate * os.getOSRatio());
   delay2Smooth.setTargetValue(delayTimeSec2 * 0.5f * samplerate * os.getOSRatio());

   // calc makeup gain
   auto drive_makeup = [](float wh) -> float
   {
      return std::exp(-0.11898f * wh) + 1.0f;
   };

   auto bias_makeup = [](float bf) -> float
   {
      return 6.0f * std::pow(bf, 7.5f) + 0.9f;
   };

   const auto makeupGain = drive_makeup(*f[neuron_drive_wh]) * bias_makeup(bf_clamped);

   makeup.set_target_smoothed(makeupGain);

   width.set_target_smoothed(db_to_linear(*f[neuron_width]));
   outgain.set_target_smoothed(db_to_linear(*f[neuron_gain]));
}


void Neuron::suspend()
{
   init();
}

const char* Neuron::group_label(int id)
{
   switch (id)
   {
   case 0:
      return "Distortion";
   case 1:
      return "Comb";
   case 2:
      return "Output";
   }

   return 0;
}

int Neuron::group_label_ypos(int id)
{
   switch (id)
   {
   case 0:
      return 1;
   case 1:
      return 13;
   case 2:
      return 19;
   }
   return 0;
}

void Neuron::init_ctrltypes()
{
   Effect::init_ctrltypes();

   fxdata->p[neuron_drive_wh].set_name("Drive");
   fxdata->p[neuron_drive_wh].set_type(ct_decibel_narrow);
   fxdata->p[neuron_drive_wh].posy_offset = 1;

   fxdata->p[neuron_squash_wf].set_name("Squash");
   fxdata->p[neuron_squash_wf].set_type(ct_percent);
   fxdata->p[neuron_squash_wf].posy_offset = 1;
   fxdata->p[neuron_squash_wf].val_default.f = 0.5f;

   fxdata->p[neuron_stab_uf].set_name("Stab");
   fxdata->p[neuron_stab_uf].set_type(ct_percent);
   fxdata->p[neuron_stab_uf].posy_offset = 1;
   fxdata->p[neuron_stab_uf].val_default.f = 0.5f;

   fxdata->p[neuron_asym_uh].set_name("Asymmetry");
   fxdata->p[neuron_asym_uh].set_type(ct_percent);
   fxdata->p[neuron_asym_uh].posy_offset = 1;
   fxdata->p[neuron_asym_uh].val_default.f = 1.0f;

   fxdata->p[neuron_bias_bf].set_name("Bias");
   fxdata->p[neuron_bias_bf].set_type(ct_percent);
   fxdata->p[neuron_bias_bf].posy_offset = 1;

   fxdata->p[neuron_comb_freq].set_name("Frequency");
   fxdata->p[neuron_comb_freq].set_type(ct_freq_audible);
   fxdata->p[neuron_comb_freq].posy_offset = 3;
   fxdata->p[neuron_comb_freq].val_default.f = 70.0f;

   fxdata->p[neuron_comb_sep].set_name("Separation");
   fxdata->p[neuron_comb_sep].set_type(ct_freq_mod);
   fxdata->p[neuron_comb_sep].posy_offset = 3;

   fxdata->p[neuron_width].set_name("Width");
   fxdata->p[neuron_width].set_type(ct_decibel_narrow);
   fxdata->p[neuron_width].posy_offset = 5;

   fxdata->p[neuron_gain].set_name("Gain");
   fxdata->p[neuron_gain].set_type(ct_decibel_narrow);
   fxdata->p[neuron_gain].posy_offset = 5;
}

void Neuron::init_default_values()
{
   fxdata->p[neuron_drive_wh].val.f = 0.0f;
   fxdata->p[neuron_squash_wf].val.f = 0.5f;
   fxdata->p[neuron_stab_uf].val.f = 0.5f;
   fxdata->p[neuron_asym_uh].val.f = 1.0f;
   fxdata->p[neuron_bias_bf].val.f = 0.0f;
   fxdata->p[neuron_comb_freq].val.f = 70.0f;
   fxdata->p[neuron_comb_sep].val.f = 0.5f;
   fxdata->p[neuron_width].val.f = 0.0f;
   fxdata->p[neuron_gain].val.f = 0.0f;
}

} // namespace chowdsp
