/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia contributors. All rights reserved.                    *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "../../ui/shaders/xesl.xesli"

// The source view already applies the guest texture swizzle. Copy through the
// GPU into an immutable host image before publishing the native frame fence.
XE_VK_PUSH_CONST
cbuffer NativeFrameCopyConstants : register(b0, space0) {
  uint2 size;
  uint associate_overlay_rgb;
  uint reserved;
};

XE_VK_BINDING(0, 0)
Texture2D<float4> source : register(t0, space0);
XE_VK_BINDING(0, 1)
XE_VK_IMAGE_FORMAT("rgba8")
RWTexture2D<unorm float4> destination : register(u0, space0);

[numthreads(16, 8, 1)] void main(
    uint3 dispatch_thread_id : SV_DispatchThreadID) {
  uint2 position = dispatch_thread_id.xy;
  if (any(position >= size)) {
    return;
  }
  float4 color = source.Load(int3(position, 0));
  // Trial interpretation of this native overlay resolve as straight RGBA:
  // associate RGB before filtering into the premultiplied host plane. Raw guest
  // resolve/readback bytes and alpha are unchanged. Other output origins keep
  // their existing representation.
  if (associate_overlay_rgb != 0) {
    color.rgb *= color.a;
  }
  destination[position] = color;
}
