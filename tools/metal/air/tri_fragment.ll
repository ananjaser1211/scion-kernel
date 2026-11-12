; SPDX-License-Identifier: MIT
; Copyright (c) 2026 the HakiOS authors (Scion).
;
; The fragment stage of the Metal-on-Vulkan triangle (tools/metal/metal_tri.m), hand-written as AIR (see
; tri_vertex.ll for why). It is this shader:
;
;     fragment float4 tri_fragment(VOut in [[stage_in]]) { return in.color; }
;
; The colour arrives as the vertex stage's user output "color" (the generated(...) name pairs the two),
; interpolated with perspective; the position is declared, as Apple's compiler declares it, and unused.

source_filename = "tri_fragment"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.0.0"

define <{ <4 x float> }> @tri_fragment(<4 x float> %pos, <4 x float> %color) local_unnamed_addr {
  %o = insertvalue <{ <4 x float> }> undef, <4 x float> %color, 0
  ret <{ <4 x float> }> %o
}

!air.fragment = !{!0}
!air.version = !{!6}
!air.language_version = !{!7}

!0 = !{<{ <4 x float> }> (<4 x float>, <4 x float>)* @tri_fragment, !1, !3}
!1 = !{!2}
!2 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!3 = !{!4, !5}
!4 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_unused", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!5 = !{i32 1, !"air.fragment_input", !"generated(5colorDv4_f)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!6 = !{i32 2, i32 5, i32 0}
!7 = !{!"Metal", i32 3, i32 0, i32 0}
