; SPDX-License-Identifier: MIT
; Copyright (c) 2026 the HakiOS authors (Scion).
;
; The vertex stage of the Metal-on-Vulkan triangle (tools/metal/metal_tri.m), hand-written as AIR so that
; it can be made without Xcode's Metal compiler (on the Linux build host). It is this shader:
;
;     struct VOut { float4 position [[position]]; float4 color; };
;     vertex VOut tri_vertex(uint vid [[vertex_id]]) {
;         // 0: bottom left, red; 1: bottom right, green; 2: top, blue
;         float x = vid == 0 ? -0.8 : vid == 1 ? 0.8 : 0.0;
;         float y = vid == 2 ? 0.8 : -0.8;
;         return { float4(x, y, 0, 1), float4(vid == 0, vid == 1, vid == 2, 1) };
;     }
;
; No buffers and no constants in memory: the three corners come from the vertex id, so the draw needs
; nothing but the pipeline. The metadata keys are the AIR interface (docs/air.md section 3); nothing here
; comes out of Apple's libraries. Typed pointers (LLVM 14's llvm-as); metal2vulkan's reader takes both.
;
;   llvm-as -o tri_vertex.bc tools/metal/air/tri_vertex.ll   (tools/metal/mk-metal-tri.sh does it)

source_filename = "tri_vertex"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64-apple-ios16.0.0"

%VOut = type <{ <4 x float>, <4 x float> }>

define %VOut @tri_vertex(i32 %vid) local_unnamed_addr {
  %is0 = icmp eq i32 %vid, 0
  %is1 = icmp eq i32 %vid, 1
  %is2 = icmp eq i32 %vid, 2
  %x1 = select i1 %is1, float 0x3FE99999A0000000, float 0.000000e+00
  %x = select i1 %is0, float 0xBFE99999A0000000, float %x1
  %y = select i1 %is2, float 0x3FE99999A0000000, float 0xBFE99999A0000000
  %p0 = insertelement <4 x float> <float undef, float undef, float 0.000000e+00, float 1.000000e+00>, float %x, i32 0
  %p = insertelement <4 x float> %p0, float %y, i32 1
  %r = select i1 %is0, float 1.000000e+00, float 0.000000e+00
  %g = select i1 %is1, float 1.000000e+00, float 0.000000e+00
  %b = select i1 %is2, float 1.000000e+00, float 0.000000e+00
  %c0 = insertelement <4 x float> <float undef, float undef, float undef, float 1.000000e+00>, float %r, i32 0
  %c1 = insertelement <4 x float> %c0, float %g, i32 1
  %c = insertelement <4 x float> %c1, float %b, i32 2
  %o0 = insertvalue %VOut undef, <4 x float> %p, 0
  %o = insertvalue %VOut %o0, <4 x float> %c, 1
  ret %VOut %o
}

!air.vertex = !{!0}
!air.version = !{!6}
!air.language_version = !{!7}

!0 = !{%VOut (i32)* @tri_vertex, !1, !4}
!1 = !{!2, !3}
!2 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!3 = !{!"air.vertex_output", !"generated(5colorDv4_f)", !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!4 = !{!5}
!5 = !{i32 0, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"vid"}
!6 = !{i32 2, i32 5, i32 0}
!7 = !{!"Metal", i32 3, i32 0, i32 0}
