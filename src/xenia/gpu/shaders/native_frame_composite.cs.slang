/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "../../ui/shaders/xesl.xesli"

// The native renderer accumulates premultiplied color over a transparent clear.
// Blend once, after title gamma/FXAA. The presenter output format is rgb10_a2.
XE_VK_PUSH_CONST
cbuffer NativeFrameCompositeConstants : register(b0, space0) {
  uint2 output_size;
  uint2 source_size;
  uint2 target_offset;
  uint2 target_size;
};

XE_VK_BINDING(0, 0)
Texture2D<float4> source : register(t0, space0);
XE_VK_BINDING(0, 1)
XE_VK_IMAGE_FORMAT("rgb10_a2")
RWTexture2D<unorm float4> destination : register(u0, space0);

[numthreads(16, 8, 1)] void main(
    uint3 dispatch_thread_id : SV_DispatchThreadID) {
  uint2 position = dispatch_thread_id.xy;
  if (any(position >= output_size) || any(position < target_offset) ||
      any(source_size == 0) || any(target_size == 0)) {
    return;
  }
  uint2 local_position = position - target_offset;
  if (any(local_position >= target_size)) {
    return;
  }

  // Explicit bilinear texel fetches avoid a sampler and preserve premultiplied
  // interpolation. Clamp edge samples rather than wrapping the guide image.
  float2 source_position = (float2(local_position) + 0.5) *
                               float2(source_size) / float2(target_size) -
                           0.5;
  int2 lower = int2(floor(source_position));
  int2 upper = lower + 1;
  float2 fraction = frac(source_position);
  int2 last = int2(source_size) - 1;
  lower = clamp(lower, int2(0, 0), last);
  upper = clamp(upper, int2(0, 0), last);
  float4 top = lerp(source.Load(int3(lower, 0)),
                    source.Load(int3(upper.x, lower.y, 0)), fraction.x);
  float4 bottom = lerp(source.Load(int3(lower.x, upper.y, 0)),
                       source.Load(int3(upper, 0)), fraction.x);
  float4 native_color = lerp(top, bottom, fraction.y);
  if (native_color.a == 0.0) {
    return;
  }
  destination[position] =
      native_color + destination[position] * (1.0 - native_color.a);
}
