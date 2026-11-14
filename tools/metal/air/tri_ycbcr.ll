; SPDX-License-Identifier: MIT
; Copyright (c) 2026 the HakiOS authors (Scion).
;
; A fragment stage of the Metal-on-Vulkan triangle (tools/metal/metal_tri.m --ycbcr) that reads a two-plane YCbCr texture at the
; fragment's own pixel, hand-written as AIR (see tri_vertex.ll for why). It is this shader:
;
;     fragment float4 tri_ycbcr(float4 pos [[position]], texture2d<float, access::read> t [[texture(0)]])
;     { return t.read(uint2(pos.xy)); }
;
; The read is the current form Apple's compiler emits (the texture, the read sampler, the coordinate, an offset of zero, the level and
; the access, read = 1), as metal2vulkan's own tests spell it; the texture is an argument of role air.texture at index 0.

source_filename = "tri_ycbcr"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.0.0"

%struct._texture_2d_t = type opaque
%struct._sampler_t = type opaque

define <{ <4 x float> }> @tri_ycbcr(<4 x float> %pos, %struct._texture_2d_t addrspace(1)* %t) local_unnamed_addr {
  %x = extractelement <4 x float> %pos, i64 0
  %y = extractelement <4 x float> %pos, i64 1
  %xi = fptoui float %x to i32
  %yi = fptoui float %y to i32
  %c0 = insertelement <2 x i32> undef, i32 %xi, i64 0
  %c = insertelement <2 x i32> %c0, i32 %yi, i64 1
  %s = tail call %struct._sampler_t addrspace(2)* @air.get_read_sampler()
  %r = call { <4 x float>, i8 } @air.read_texture_2d.v4f32(%struct._texture_2d_t addrspace(1)* %t, %struct._sampler_t addrspace(2)* %s, <2 x i32> %c, <2 x i32> zeroinitializer, i32 0, i32 1)
  %v = extractvalue { <4 x float>, i8 } %r, 0
  %o = insertvalue <{ <4 x float> }> undef, <4 x float> %v, 0
  ret <{ <4 x float> }> %o
}

declare %struct._sampler_t addrspace(2)* @air.get_read_sampler()
declare { <4 x float>, i8 } @air.read_texture_2d.v4f32(%struct._texture_2d_t addrspace(1)*, %struct._sampler_t addrspace(2)*, <2 x i32>, <2 x i32>, i32, i32)

!air.fragment = !{!0}
!air.version = !{!6}
!air.language_version = !{!7}

!0 = !{<{ <4 x float> }> (<4 x float>, %struct._texture_2d_t addrspace(1)*)* @tri_ycbcr, !1, !3}
!1 = !{!2}
!2 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!3 = !{!4, !5}
!4 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!5 = !{i32 1, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"texture2d<float, read>", !"air.arg_name", !"t"}
!6 = !{i32 2, i32 5, i32 0}
!7 = !{!"Metal", i32 3, i32 0, i32 0}
