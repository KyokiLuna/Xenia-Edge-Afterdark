/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include "xesl.xesli"

push_const_begin_xe(b0, space0)
  block_offset_member_xe(16, c0.x, float2_xe, xe_native_output_offset)
  block_offset_member_xe(24, c0.z, float2_xe, xe_native_output_size_inv)
  block_offset_member_xe(32, c1.x, float, xe_native_opacity)
push_const_end_xe

entry_outputs_begin_xe
  entry_out_target_xe(float4_xe, xe_native_color, 0)
entry_outputs_end_stage_inputs_begin_xe
entry_stage_inputs_end_bindings_begin_pixel_xe
  push_const_binding_xe(buffer(0))
  entry_binding_next_xe
  texture_xe(texture_2d_xe, xe_native_source, 0, 0, t0, space0, texture(0))
  entry_binding_next_xe
  sampler_state_xe(xe_native_sampler, 0, 1, s0, space0, sampler(0))
entry_bindings_end_inputs_begin_xe
  entry_in_pixel_coord_xe
entry_inputs_end_code_begin_xe
{
  float2_xe coordinate =
      (in_pixel_coord_xe.xy -
       push_const_xe(xe_native_output_offset)) *
      push_const_xe(xe_native_output_size_inv);
  // The copied image already contains native premultiplied RGBA. Preserve
  // alpha; the fixed-function pipeline uses ONE / ONE_MINUS_SRC_ALPHA.
  // Apply only the producer's normalized opacity, once to all RGBA channels.
  // Raw D1OVL alpha registers are not interpreted by this shader.
  out_xe(xe_native_color) = sample_sep_lod_2d_xe(
      xe_native_source, xe_native_sampler, coordinate, 0.0f) *
      push_const_xe(xe_native_opacity);
}
entry_code_end_xe
