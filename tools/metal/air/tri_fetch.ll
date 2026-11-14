; SPDX-License-Identifier: MIT
; Copyright (c) 2026 the HakiOS authors (Scion).
;
; A fragment stage of the Metal-on-Vulkan triangle (tools/metal/metal_tri.m --fetch) that reads its colour attachment (framebuffer
; fetch), hand-written as AIR (see tri_vertex.ll for why). It is this shader:
;
;     fragment float4 tri_fetch(VOut in [[stage_in]], float4 fb [[color(0)]]) { return in.color * 0.5 + fb * 0.5; }
;
; The attachment's colour arrives as an argument whose role is air.render_target with the colour index (0), as Apple's compiler declares
; a [[color(0)]] input. On a device without rasterization-order attachment access and without the pixel interlock, metal2vulkan reads
; it as an input attachment (tools/metal/m2v/0005-unordered-framebuffer-fetch.patch).

source_filename = "tri_fetch"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.0.0"

define <{ <4 x float> }> @tri_fetch(<4 x float> %pos, <4 x float> %color, <4 x float> %fb) local_unnamed_addr {
  %c = fmul <4 x float> %color, <float 5.000000e-01, float 5.000000e-01, float 5.000000e-01, float 5.000000e-01>
  %f = fmul <4 x float> %fb, <float 5.000000e-01, float 5.000000e-01, float 5.000000e-01, float 5.000000e-01>
  %s = fadd <4 x float> %c, %f
  %o = insertvalue <{ <4 x float> }> undef, <4 x float> %s, 0
  ret <{ <4 x float> }> %o
}

!air.fragment = !{!0}
!air.version = !{!7}
!air.language_version = !{!8}

!0 = !{<{ <4 x float> }> (<4 x float>, <4 x float>, <4 x float>)* @tri_fetch, !1, !3}
!1 = !{!2}
!2 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!3 = !{!4, !5, !6}
!4 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_unused", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!5 = !{i32 1, !"air.fragment_input", !"generated(5colorDv4_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!6 = !{i32 2, !"air.render_target", i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"fb"}
!7 = !{i32 2, i32 5, i32 0}
!8 = !{!"Metal", i32 3, i32 0, i32 0}
