# ESP-DL int8 arithmetic, as `detect` reproduces it

What `modules/detect/pico.c` reproduces, read out of ESP-DL's source (esp-dl 9c5fc6f, MIT) and then confirmed byte for byte against ESP-DL running on the ESP32-P4 (design.md §62). Two parts: the pedestrian detector, and what the two-stage face detector adds. File paths are into the esp-dl repository. Scratch paths mentioned in part 2 were working files and are not kept.

## Part 1: pedestrian_detect pico_s8_v1

Model: dl/esp-dl/models/pedestrian_detect/models/p4/pedestrian_detect_pico_s8_v1.espdl (EDL2, 16-byte header, then flatbuffer; parser dl/espdl.py). Wiring/exponents: shifts.txt. Layers: dl/p4layers.txt.
114 nodes: Conv 84, GAP 5, Sigmoid(LUT) 11, Mul 8, Resize 2, Concat 4. 67.7M MACs. Weights 343 KB. Peak activations ~196 KB. ESP-DL: 46 ms model on P4.

- Rounding: P4 = round half to even everywhere: q=v>>s; r=v&((1<<s)-1); h=1<<(s-1); if(r>h||(r==h&&(q&1)))q++. Saturate -128..127.
- Conv: acc = bias(int32, exp in+w) + sum x*w; y=sat(rhe(acc>>mac_shift)); relu after. mac_shift = out-w-in, all 4..10. Zero padding. Out = (H+2p-k)/s+1.
- Weight layout (raw_data, NOT HWCN when cout>16): group=1: [cout/16][kh][kw][cin][16] then remainder [n_rem][kh][kw][cin]; elem (ky,kx,ci,co) at ((co/16)*kh*kw*cin + (ky*kw+kx)*cin + ci)*16 + co%16. Depthwise [kh,kw,C,1]: [C/16][kh][kw][16]; elem at ((c/16)*kh*kw + ky*kw+kx)*16 + c%16. Bias int32 LE natural order. (Agent's reading, statistically confirmed; verify.)
- GAP: float scale = 2^in_exp * (1.f/(H*W)) * 2^-out_exp (left to right, float32); y=sat(rne_float(sum*scale)).
- Sigmoid LUT: y = table[x+128], table int8 256 entries, exp = out exp (-7).
- Mul: broadcasting [H,W,C]x[1,1,C] or [H,W,1]x[H,W,1]; y=sat(rhe(a*b >> (out-e0-e1))), shifts 4..7.
- Resize nearest x2: out[y][x]=in[y>>1][x>>1], same exp.
- Concat axis 3: memcpy, node input order, exps equal.
- Preprocess: stretch (no letterbox) nearest to 224x224: sx=min((int)(i*((float)src_w/224)),src_w-1); RGB order; q=sat(rne(v*0.5f)). RGB565 LE: R8=(p&0xF800)>>8, G8=(p&0x7E0)>>3, B8=(p&0x1F)<<3 (zero fill).
- Post: score_thr 0.7, nms 0.5, top_k 10. Stages: (score0,bbox0,stride 8,off 4, exps -8/-3), (score1,bbox1,16,8,-7/-3), (score2,bbox2,32,16,-7/-4). Scan y,x. Keep if q > quant(0.49) (125 for exp -8, 63 for -7); score = sqrtf(q*2^exp). Bbox DFL: 32ch = L[0..7] T[8..15] R[16..23] B[24..31]; softmax of q*2^exp per 8, dfl=sum i*p_i. cx=x*stride+off; x1=(int)((cx-dfl(L)*stride)*src_w/224) etc. Sorted desc by score (stable). Greedy NMS IoU>0.5 with int areas (x2-x1+1)*(y2-y1+1); stop at 10 kept; clip to image after.
- Unverified: srs.s.xacc (cout=1 convs) rounding assumed half-even.

## Part 2: human_face_detect msr_s8_v1 + mnp_s8_v1

Resolved since this was written: `esp.vprelu.s8` rounds **half to even**, settled on the board (the example image gives 0.7773, as below).


Paths are relative to `dl/esp-dl/` (repo) or to `dl/esp-dl/esp-dl/` (library: dl/, vision/). Models: `models/human_face_detect/models/p4/human_face_detect_{msr,mnp}_s8_v1.espdl`. The s3 files contain the same weights; my simulation gives identical results with either.

Everything in pico-spec.md still holds: half-to-even rounding, conv requantisation, bias = int32 at exponent in+w, zero padding, sat, relu-after, NMS, the resize index formula, and RGB565 extraction. This document adds only what differs.

**Reference implementation.** `scratchpad/facesim/sim.py` is a numpy int8 simulation of this whole spec. Run it as `python3 -I sim.py img.jpg`. It runs both models end to end. Use it to make golden vectors.
- On ESP-DL's own example image (`examples/human_face_detect/main/human_face.jpg`, 320x240) it gives: MSR candidate (0.719, [88,82,197,204]); final face score 0.7773, box [101,64,196,191], keypoints [118,114, 121,160, 132,145, 158,113, 152,160].
- On `me2.jpg` the five keypoints land exactly on the eyes, nose tip and mouth corners.
- The example README (`examples/human_face_detect/README.md:17-18`) gives box [100,64,193,191], score 0.936, keypoints within 1-2 px of these. That output comes from an unspecified older build; see the PRelu caveat in §4.

## Network shapes and exponents
- MSR: input [1,120,160,3] (H=120, W=160), exponent 1.
  - Outputs: score0 [13,18,2] e-4; box0 [13,18,8] e-8; landmark0 [13,18,20] e-7; score1 [7,9,2] e-4; box1 [7,9,8] e-8; landmark1 [7,9,20] e-7.
  - The graph branches: node 8's output feeds nodes 9, 10 and 11. Use `ed.py <model> wire` and `ed.py <model> shapes` (scratchpad/dl/ed.py) to get the wiring.
  - Ops: 39 Conv and 2 Concat. Every depthwise has no bias and uses Relu. Every group=1 conv has a bias and is Linear, except node 0 (3x3, Relu).
  - **landmark0 and landmark1 (nodes 32 and 40) are never read by the postprocessor.** You can skip them.
- MNP: input [1,48,48,3], exponent 1. Outputs: score [1,1,2] e-4; box [1,1,4] e-8; landmark [1,1,10] e-7. Ops: 15 Conv and 8 PRelu. Activations are all Linear; the nonlinearity is PRelu.
- Every mac_shift is positive. Per node (c=group conv, dw=depthwise, the digit is k, b=bias, R=relu, sN=shift):
  - MSR: 0:c3bR s9, 1:dw3R s7, 2:c1b s5, 3:dw3R s7, 4:c1b s6, 5:dw3R s5, 6:c1b s6, 7:dw3R s7, 8:c1b s6, 9:dw3R s7, 10:dw3R s7, 11:dw3R s6, 12:c1b s9, 13:c1b s7, 14:c1b s7, 15:dw3R s7, 16:dw3R s7, 17:dw3R s7, 18:c1b s8, 19:c1b s6, 20:c1b s6, 21:dw3R s7, 22:dw3R s7, 23:dw3R s6, 24:c1b s8, 25:c1b s6, 26:c1b s7, 28:dw3R s8, 29:dw3R s6, 30:c1b s6, 31:c1b s4, 32:c1b s5, 33:c1b s6, 34:c1b s6, 35:dw3R s8, 36:c1b s6, 38:c1b s8, 39:c1b s6, 40:c1b s7.
  - MNP: 0:c3b s8, 1:PRelu, 2:c1 s6, 3:PRelu, 4:dw3 s5, 5:PRelu, 6:c1b s6, 7:dw3 s6, 8:PRelu, 9:c1b s7, 10:c1 s7, 11:PRelu, 12:dw3 s6, 13:PRelu, 14:c1b s8, 15:c1 s8, 16:PRelu, 17:dw3 s6, 18:PRelu, 19:c1b s8, 20-22:c1b s8/s7/s8.
  - In every conv that has a bias, bias exponent = in + w; the simulation asserts this.

## 1. Weight layout when the channel count is not a multiple of 16
raw_data holds exactly prod(dims) elements, padded only at the end to a 16-byte multiple. There is no per-channel padding: dw C=24 is 216 data bytes (224 stored); [1,1,24,24] is 576; [3,3,3,32] is 864. Let nd = cout/16 (C/16 for depthwise) and rem = cout%16.

**group=1, dims [kh,kw,cin,cout]**
```
co <  16*nd : idx = ((co/16)*kh*kw*cin + (ky*kw+kx)*cin + ci)*16 + co%16
co >= 16*nd : idx = 16*nd*kh*kw*cin + ((co-16*nd)*kh*kw + ky*kw+kx)*cin + ci
```
- cin that is not a multiple of 16 (3, 24, 96) changes nothing. Each tap stores cin*16 bytes for the 16-lane groups and cin bytes per remainder channel, with no padding.
- Source:
  - `dl/base/dl_base_conv_args.hpp:443-460`: n_div_x = cout/u, n_remainder = cout%u, c_remainder = cin%u; filter_element_unaligned = filter + n_div_x*kh*kw*filter_c*16 with filter_c = cin (:343).
  - Remainder kernel, `isa/esp32p4/dl_esp32p4_s8_unaligned_conv2d.S:301-358` (macro `11c1`): it reads cin contiguous bytes per output channel, so the net pointer step is cin.
  - Lane-group kernel `11c16` (:12-88): it walks [cin][16] and handles the c%16 tail in place.
- Remainder channels are computed one at a time (`:457-476`): `esp.zero.xacc`; load the int32 bias sign-extended into xacc (`dl_esp32p4_s8.S:15-23`); mac; `esp.srs.s.xacc out, mac_shift` (:231); clamp to [-128,127] or [0,127] for relu.
  - This is the same scalar path as pico's cout=1 heads, which matched byte for byte, so treat its half-to-even rounding as verified.
  - It matters much more here: score (cout 2), box (8 or 4) and the MNP landmark conv (10) run entirely on this path, and cout 24 or 20 conv layers have 8 or 4 remainder channels.
- Checks:
  - Statistical, MNP node 0 (cout=32), mean R/G/B correlation per filter: packed 0.40 against 0.06 for HWCN; packed is also spatially smoother.
  - Functional: the MNP landmark conv (cout=10, all remainder) gives correct eye, nose and mouth points.
  - Swapping the remainder to [kh][kw][cin][rem] removes every MSR detection.

**Depthwise, dims [kh,kw,C,1]**
```
c <  16*nd : idx = ((c/16)*kh*kw + ky*kw+kx)*16 + c%16
c >= 16*nd : idx = 16*nd*kh*kw + (ky*kw+kx)*rem + (c-16*nd)      // remainder block is [kh][kw][rem]
```
- Source:
  - filter_c = 1 for depthwise (`dl_base_conv_args.hpp:350`), so the unaligned pointer = filter + nd*kh*kw*16.
  - Per-tap steps: `dl_base_dwconv_loop.cpp:83-86` (unaligned_filter_c_n_offset = c_remainder; aligned 16).
  - Remainder kernel: `dl_esp32p4_s8_unaligned_depthwise_conv2d.S:996-1013` (unaligned_filter_ptr at args+168). It uses the same vector QACC, then `esp.srcmb.s8.qacc` (half-to-even) and saturation, so the arithmetic is identical to the aligned lanes.
- Statistics on the 8 remainder channels of all 9 C=24 depthwise layers: [kh][kw][rem] is clearly the smoothest. Spatial roughness 0.51-1.02, against 1.13-1.86 for HWC, [rem][kh][kw] and a 16-padded block.

## 2. Depthwise (and 1x1) with no bias
- Yes, exactly: acc = Σx·w (no bias), then y = sat(rhe(acc >> mac_shift)), then relu if the node says Relu. The kernel starts from `esp.zero.qacc` instead of loading a bias.
  - Kernel variants without bias: `dl_esp32p4_s8_unaligned_depthwise_conv2d.S:1142` (`_hwc1`) and :1252 (`_hwc1_relu`).
  - Relu is `esp.vrelu.s8 q, 0, 0`, i.e. max(0, ·), applied after saturation (`dl_esp32p4_s8.S:75-78`).
- The conv module passes bias = nullptr when the node has only 2 inputs: `dl_module_conv.hpp:123-125`, `dl_base_conv_args.hpp:408`.
- MNP also has group=1 1x1 convs with no bias (nodes 2, 10, 15). They follow the same rule.

## 3. Asymmetric pads
- The ONNX attribute order is [top(H begin), left(W begin), bottom(H end), right(W end)].
- ESP-DL reorders it to [top, bottom, left, right] (`dl/module/include/dl_module_conv.hpp:186-187`: `pads = {pads[0], pads[2], pads[1], pads[3]}`). Those become padding_h_head/tail and padding_w_head/tail (`dl_base_conv_args.hpp:360-363`).
- Output size: `Ho = (H + top + bottom - (kh-1) - 1)/sh + 1` and `Wo = (W + left + right - (kw-1) - 1)/sw + 1` (`dl_module_conv.hpp:95,100`). Input row for output y, tap ky = y*s - top + ky; zero outside.
- MSR node 7, [0,0,1,1]: 26x36 → 13x18. No top/left pad; one zero row at the bottom and one zero column at the right. The last output row reads input rows 24, 25 and the pad row.
- MSR node 17, [1,0,1,1]: 13x18 → 7x9. One zero row at top and bottom, none at left, one at the right.
- Both match the shapes stored in the model.

## 4. PRelu (MNP only, 8 nodes)
- Alpha is a separate initializer of shape [C,1,1], int8, per channel in natural channel order, with a single exponent ea.
  - Values range from -120 to 113, and **some alphas are negative**.
  - Exponents: node 1: -6, 3: -5, 5: -6, 8: -6, 11: -6, 13: -6, 16: -6, 18: -5.
- The node has no `lut` attribute, so it is a real PRelu module, not a LUT (`dl_module_prelu.hpp:136-142`).
- Arguments, `dl/base/dl_base.hpp:1902-1928`:
  - `activation_shift = out_e - in_e - ea`
  - `output_shift = out_e - in_e`; if that is negative, `output_scale = 1<<(in_e-out_e)` and the shift becomes 0.
- P4 kernel `isa/esp32p4/dl_esp32p4_prelu.S:8-56` (all C here are multiples of 16, so the aligned path is used); round mode is set to half-even just before (`dl_base_prelu.cpp:94`):
  - x<0:  `esp.vprelu.s8` → (x*alpha) >> activation_shift, saturated to int8, masked by x<0.
  - x>0:  `vmulas` x*output_scale into QACC, then `srcmb` with shift 0 → sat(x << (in_e-out_e)), masked by x>0.
  - x==0: 0.
- Per node (neg shift / pos left shift, in→out exponent):
  - 1: 6/0 (2→2)
  - 3: 4/1 (2→1)
  - 5: 4/2 (-1→-3)
  - 8: 5/1 (-5→-6)
  - 11: 6/0 (-5→-5)
  - 13: 5/1 (-6→-7)
  - 16: 5/1 (-4→-5)
  - 18: 2/3 (-5→-8)
  - Saturation matters on both branches.
- **NOT DETERMINED: the rounding of the right shift inside `esp.vprelu.s8`.**
  - The repo only documents the S3 `EE.VPRELU` as `(x*a)>>sar` (`tools/agents/skills/esp32s3-pie-simd/references/pie_instruction_set.md:987-1000`).
  - The C fallback uses a floor shift (`DL_RIGHT_SHIFT`, `dl_base_prelu.cpp:15`).
  - My recommendation is **round half to even**: `y = sat(rhe(x*alpha, shift))`. It equals ESP-PPQ's float reference requantised with P4's rounding, the kernel sets half-even immediately before, and it gives the result closest to the README box.
  - It is decision-critical: on the example image, half-to-even gives a face probability of 0.777 and floor gives 0.974.
  - Verify by dumping one PRelu output on the device (e.g. tensor "34" or "37") for a fixed input.
  - Saturating (rather than wrapping) the negative branch is also assumed.

## 5. Concat
- All six inputs match their outputs: MSR node 27: 103/109/115 (e-5) → 116 (e-5); MSR node 37: 152/158/164 (e-7) → 165 (e-7). Nothing is requantised.
- It is a plain channel concat in node input order: 48+24+24 = 96 channels.

## 6. Preprocessing
- `ImagePreprocessor(model, {0,0,0}, {1,1,1}, true)`: the 4th argument is `rgb_swap` and means **the model takes BGR** (`vision/image/dl_image_preprocessor.hpp:18`; the dst pix type becomes BGR888_QINT8 at `.cpp:80-85`).
- Source RGB888 bytes [R,G,B] → model channels [B,G,R] (`dl_image_color_rgb8882rgb888.hpp:59-62`). **Pico used RGB order; this model uses BGR.**
- RGB565 sources use the same extraction as pico; then ch0 = B, ch1 = G, ch2 = R (`dl_image_color_rgb5652rgb888.hpp:28-31` for LE).
- Quantisation is a LUT: q = sat(rne(v * 2^-1)) (`dl_image_norm_quant.hpp:22-27`), with input exponent 1 in both models. **255 → 128 → saturates to 127.** The mean and std are the same for all channels, so the LUTs are identical.
- MSR resize is the pico stretch-nearest formula, with dst 160x120: inv_sx = (float)src_w/160, inv_sy = (float)src_h/120; sx = min((int)(i*inv_sx), src_w-1) (`dl_image_process.cpp:832-843`). No letterbox.

## 7. MSR postprocessing (`vision/detect/dl_detect_msr_postprocessor.cpp`)
- Stages: stage 0 = (score0, box0), stride 8, offset 9, anchors (h,w) = (16,16) then (32,32). Stage 1 = (score1, box1), stride 16, offset 9, anchors (64,64) then (128,128). See :75-82.
- A = 2 anchors and C = 1 class. Score channel a is the logit for anchor a. Box channels 4a..4a+3 are [x1off, y1off, x2off, y2off] (:19-22, :62-63).
- Scan order is y, x, a. Stage 0 is fully inserted before stage 1.
- Threshold:
  - `quantize(inverse_sigmoid(thr), 16)`, where `inverse_sigmoid(x) = -logf(1.0/x - 1)` (`dl_math.hpp:213`) and quantize is rne then clip (`dl_tensor_base.cpp:20-31`).
  - For the default thr 0.5 this is 0, so **keep a cell if q > 0**.
  - Defaults: score_thr 0.5, nms 0.5, top_k 10 (`models/human_face_detect/human_face_detect.hpp:11-12`, `.cpp:107-113`).
- score = (float)(1.0/(1.0 + expf(-(float)q*0.0625f))), a double divide with a float expf (`dl_math.hpp:200-203`).
- Box, all float32, with `(int)` truncation. Let es = 2^-8, isx = src_w/160.f and isy = src_h/120.f:
```
cx = x*stride + 9;  cy = y*stride + 9;  aw = ah = anchor size
x1 = (int)(((cx - (aw>>1)) + aw*(q0*es)) * isx)
y1 = (int)(((cy - (ah>>1)) + ah*(q1*es)) * isy)
x2 = (int)(((cx + aw - (aw>>1)) + aw*(q2*es)) * isx)
y2 = (int)(((cy + ah - (ah>>1)) + ah*(q3*es)) * isy)
```
- Insertion: stable descending (`upper_bound` with `greater_box`). Then the same NMS as pico (`dl_detect_postprocessor.cpp:73-109`; IoU in int areas with +1, iou > thr, top_k cutoff). `postprocess()` calls nms itself (:87).
- Then `get_result` clips each box to [0, W-1] and [0, H-1] (`dl_detect_base.cpp:69-70`, `dl_detect_define.hpp:16-22`). These clipped boxes, in that order, are the MNP candidates.
- MSR landmarks are not decoded at all.

## 8. Cascade and MNP (`models/human_face_detect/human_face_detect.cpp:156-190`, `vision/detect/dl_detect_mnp_postprocessor.cpp`)
For each MSR candidate, in list order:
```
cx = (x1+x2)>>1; cy = (y1+y2)>>1; side = max(x2-x1, y2-y1)   // no +1
x1 = cx-(side>>1); y1 = cy-(side>>1); x2 = x1+side; y2 = y1+side
clip x to [0,W-1], y to [0,H-1]                               // so it may no longer be square
```
**Crop**
- `preprocess(img, box)` treats the box as crop_area [x1,y1,x2,y2) with an **exclusive** right and bottom. Because x2 is at most W-1, the last image column and row can never be sampled.
- Stretch-nearest to 48x48, BGR, q as in §6 (`dl_image_process.cpp:817-853`):
```
isx = (float)(x2-x1)/48;  isy = (float)(y2-y1)/48
sx(i) = min((int)(i*isx) + x1, x2-1);  sy(j) = min((int)(j*isy) + y1, y2-1)
```
- When the crop is exactly 48x48, ESP-DL takes a copy path with scale 1, which gives the same result.
- ESP-DL asserts x1<x2 and y1<y2 (`:123-125`); a degenerate box aborts in ESP-DL. Skip such boxes in the port.
- Each candidate is one MNP run with a 1x1 spatial output (no anchor grid). Stage {1,1,0,0,{{48,48}}} means anchor_w = anchor_h = 48.

**Score** (`mnp_postprocessor.cpp:34-50`): s_i = q_i/16.f (float), m = max, sum = Σ expf(s_i - m) (float), p = (float)(1.0/sum).
- **A candidate is kept if p > 0.5, and p is the probability of whichever class wins, not specifically "face".**
- With 2 classes p ≥ 0.5 always, so ESP-DL keeps every candidate unless the two logits are exactly equal. category = 0.

**Box and landmarks** (float32, `(int)` truncation; es = 2^-8, el = 2^-7; tlx, tly = crop x1, y1; :53-83):
```
x1 = (int)(48*(b0*es)*isx + tlx)          y1 = (int)(48*(b1*es)*isy + tly)
x2 = (int)((48*(b2*es) + 48)*isx + tlx)   y2 = (int)((48*(b3*es) + 48)*isy + tly)
kp[2k]   = (int)(48*(l[2k]*el)*isx + tlx)
kp[2k+1] = (int)(48*(l[2k+1]*el)*isy + tly)        k = 0..4
```
- The order of evaluation is (int → float)·float·float + int. These results are inserted into a single list, stable and descending by p.
- After the loop over all candidates: NMS (thr 0.5, top_k 10, the same code as pico), then `get_result` clips the boxes and the keypoints (x to W-1, y to H-1).

**What gets reported per face**
- score: MNP p.
- box: [x1, y1, x2, y2].
- keypoint: 10 ints in the order left_eye, left_mouth, nose, right_eye, right_mouth (`examples/human_face_detect/main/app_main.cpp:36`). "Left" means image-left; the simulation confirms this.
- The MSR score is discarded.

## 9. Other differences from pico that affect bit-exactness
- **Channel order is BGR.**
- PRelu is a new op (§4), including the saturating left shift on the positive branch.
- The remainder-channel scalar path (srs.s.xacc) carries the whole head of every output.
- Asymmetric pads (§3).
- A Conv input of exactly 120x160 is split in two by height for dual-core (`dl_base_conv_args.hpp:470-490`). This does not change the arithmetic.
- The MNP p > 0.5 test is effectively always true (§8). Do not "fix" it by checking class 1, or your results will differ from ESP-DL's.
- The ordering ties in the MNP list depend on MSR order: candidates with equal p are kept in insertion order.
- **Could not determine:**
  - The vprelu shift rounding (§4).
  - Why the README example output differs. It most likely comes from an older ESP-DL build: no rounding variant I tried (rhe or floor on the negative branch, scale or copy on the positive branch, half-even or half-up in the convs, s3 or p4 file) reproduces it.

## Part 3: human_face_recognition (human_face_feat mfn_s8_v1)

As written before the port; the RV-9 signature matched sim.py byte for byte on a camera frame. Scratch paths are not kept.


esp-dl 9c5fc6f. Paths are relative to the checkout `dl/esp-dl/`. Library paths (`dl/…`, `vision/…`) are relative to `dl/esp-dl/esp-dl/`.
Everything in docs/esp-dl-arithmetic.md still applies: half-to-even rounding, conv requantisation, bias at exponent in+w, weight packing, zero padding, PRelu (rhe, saturating, left shift on the positive branch), Concat, the BGR swap, and the LUT quantiser.
Reference implementation: `scratchpad/recogsim/sim.py`, described in §7.

## 1. Graph
Model file: `models/human_face_recognition/models/p4/human_face_feat_mfn_s8_v1.espdl`. The s3 file holds different int8 weight values (e.g. `res_4_block0_conv_sep.weight`), so unlike the detector models it is not interchangeable with the p4 file.

**Shapes and exponents**
- Input: `input` [1,112,112,3], exponent **-6**.
- Output: `embedding` [1,1,1,512], int8, exponent **-5**.

**Size**
- 103 nodes: Conv 53, PRelu 35, **Add 12**, Concat 3.
- Weights: 1.22 MB (141 initializers).
- README: 0.46 GFLOPs, 81.5 ms on P4 (`models/human_face_recognition/README.md`).

**What is not there**
- No per-channel exponents: every tensor has a single exponent.
- No Flatten, Reshape, Gemm, BatchNorm, pooling, Sigmoid or Relu.
- Every Conv is `activation: Linear` with dilation 1.

**Convs**
- **Every conv has a bias, depthwise included.** This differs from MSR, where depthwise convs have none. Bias exponent = in + w; the simulation asserts this.
- Groups are only 1 or C (depthwise). Every channel count is a multiple of 16 except the 3-channel image input to node 0, so no remainder-lane paths are used apart from cin=3.
- Pads are all symmetric 1 for 3x3, and 0 for 1x1 and 7x7.
- Stride 2 occurs at nodes 0 (c3), 6, 35 and 79 (dw3).
- Spatial sizes: 112 → 56 → 28 → 14 → 7 → 1.

**Unusual structure**
- **Split "wide" convs.** Several 1x1 expansions are stored as two convs reading the same input, each followed by its own PRelu, then a Concat on axis 3:
  - 74/75 → 76/77 → Concat 78 (512 ch).
  - 96/97 → 98/99 → Concat 100.
  - The projection is split the same way: 81/82 (512→64 each) → Concat 83 (128 ch).
  - All concats have equal exponents (-7, -6, -7).
- **Head:**
  - Node 101 is a **7x7 depthwise conv, pad 0, on a 7x7 input** (a "GDConv"), giving 1x1x512 at e-7, shift 5. It uses the ordinary depthwise packing [C/16][7][7][16].
  - Node 102 is a **1x1 conv 512→512** ("fc1", BN folded), e-7 → e-5, shift 8, and produces `embedding`.
  - Nothing follows it in the graph.
- Residual blocks: the pattern is conv1 (expand) → PRelu → dw3 → PRelu → conv1 (project) → Add(skip).

**mac_shift per conv.** All are positive, from 5 to 10. Notation: c = group conv, dw = depthwise, the digit is k, /2 = stride 2, b = bias.
```
0:c3/2b s7, 2:dw3b s5, 4:c1b s6, 6:dw3/2b s5, 8:c1b s8, 9:c1b s7, 11:dw3b s6, 13:c1b s8, 15:c1b s7, 17:dw3b s6,
19:c1b s8, 21:c1b s8, 23:dw3b s6, 25:c1b s7, 27:c1b s8, 29:dw3b s6, 31:c1b s7, 33:c1b s8, 35:dw3/2b s7, 37:c1b s9,
38:c1b s7, 40:dw3b s7, 42:c1b s10, 44:c1b s7, 46:dw3b s6, 48:c1b s9, 50:c1b s7, 52:dw3b s5, 54:c1b s9, 56:c1b s7,
58:dw3b s6, 60:c1b s9, 62:c1b s7, 64:dw3b s6, 66:c1b s9, 68:c1b s7, 70:dw3b s6, 72:c1b s8, 74:c1b s7, 75:c1b s8,
79:dw3/2b s5, 81:c1b s9, 82:c1b s9, 84:c1b s8, 86:dw3b s5, 88:c1b s8, 90:c1b s8, 92:dw3b s6, 94:c1b s8, 96:c1b s8,
97:c1b s8, 101:dw7b s5, 102:c1b s8
```

**PRelu per node.** Format: neg shift / positive left shift (in → out exponent).
- The positive branch is never a right shift: out_e ≤ in_e everywhere.
- Every node's C is a multiple of 16, so the aligned `dl_esp32p4_s8_prelu_11c` path is taken (`dl/base/dl_base_prelu.cpp:64-70`).
```
1:6/0(-6→-6) 3:7/0(-5) 5:7/0(-5) 7:6/1(-5→-6) 10:7/1(-6→-7) 12:8/0(-6) 16:7/1(-6→-7) 18:7/0(-6) 22:7/1(-6→-7)
24:7/0(-6) 28:6/1(-6→-7) 30:7/0(-6) 34:7/1(-6→-7) 36:7/0(-6) 39:8/0(-7) 41:7/1(-6→-7) 45:8/0(-7) 47:7/1(-6→-7)
51:7/0(-7) 53:7/0(-7) 57:7/0(-7) 59:6/1(-6→-7) 63:7/0(-7) 65:6/1(-6→-7) 69:7/0(-7) 71:6/1(-6→-7) 76:8/0(-7)
77:7/0(-7) 80:7/0(-7) 85:7/0(-7) 87:7/0(-7) 91:7/0(-7) 93:6/1(-6→-7) 98:6/0(-7) 99:6/0(-7)
```

**Memory.** The largest activation is 56x56x128 = 401 KB (outputs of nodes 4 and 5, and the input of node 6). Residual Adds keep the skip tensor (≤ 28x28x64 = 50 KB) alive across a block.

## 2. Add
**Every Add has the same exponent on both inputs and on the output**, and the input shapes are identical (no broadcasting). These are confirmed from the model:
```
14: 154 + 161 -> 162   -5,-5 -> -5        49: 201 + 208 -> 209   -5,-5 -> -5
20: 162 + 169 -> 170   -5,-5 -> -5        55: 209 + 216 -> 217   -5,-5 -> -5
26: 170 + 177 -> 178   -5,-5 -> -5        61: 217 + 224 -> 225   -5,-5 -> -5
32: 178 + 185 -> 186   -5,-5 -> -5        67: 225 + 232 -> 233   -5,-5 -> -5
43: 193 + 200 -> 201   -5,-5 -> -5        73: 233 + 240 -> 241   -5,-5 -> -5
89: 254 + 261 -> 262   -6,-6 -> -6        95: 262 + 269 -> 270   -6,-6 -> -6
```
The arithmetic is **y = sat8(a + b)**: no shift, no rounding.

**Source**
- `dl/module/include/dl_module_add.hpp:66-79` → `base::elemwise_add` (`dl/base/dl_base_add.cpp:80-103`).
- With PIE_V2 and both d0 % 16 == 0, it selects `dl_esp32p4_s8_add2d_11c` (`dl/base/isa/esp32p4/dl_esp32p4_s8_add2d.S:21-84`). That kernel is only `esp.vadd.s8`, which is a signed saturating add (`tools/agents/skills/esp32p4-pie-simd/references/instructions.md:352`).
- The C fallback agrees: `tool::truncate(out, a+b)` (`dl_base_add.cpp:35-43`, `dl/tool/include/dl_tool.hpp:292-295`).

**When the exponents differ.** The int8 Add path ignores them entirely in this ESP-DL version.
- `get_elemwise_operation_args` does compute `mul_shift`, `input*_scale` and `output_rescale` (`dl/base/dl_base_elemwise.cpp:179-183`), but `elemwise_add` never selects a rescaling kernel.
- A `dl_esp32p4_s8_rescale_add2d_11c` exists (`dl_esp32p4_s8_add2d.S:114`), but nothing in `dl_base_add.cpp` calls it.
- So ESP-PPQ must emit Adds with equal exponents, as this model does.
- For the port: assert that the three exponents are equal and that the shapes match.

## 3. Preprocessing
**Call chain**
- `HumanFaceRecognizer::recognize`/`enroll` (`models/human_face_recognition/human_face_recognition.cpp:137-183`):
  - One face: use it.
  - Several faces: use the one with the largest `box_area() = (x2-x1)*(y2-y1)` (`vision/detect/dl_detect_define.hpp:32`; no +1). Ties go to the first in the list (`std::max_element` with `<`).
  - Only `keypoint` is used; the box is ignored.
- `FeatImpl::run` (`vision/recognition/dl_feat_base.cpp:41-57`) runs preprocess, then the model, then L2 postprocessing.
- Constructor: `FeatImagePreprocessor(m_model, {127.5,127.5,127.5}, {127.5,127.5,127.5}, true)` (`human_face_recognition.cpp:82-83`). rgb_swap=true, so **the model takes BGR**, as the detector does.

### 3a. Template and transform estimate
Template (`vision/recognition/dl_feat_image_preprocessor.cpp:5-6`), held as float32. The order is the same as the detector's keypoint order:
```
leye (38.2946,51.6963)  lmouth (41.5493,92.3655)  nose (56.0252,71.7366)  reye (73.5318,51.5014)  rmouth (70.7299,92.2041)
```

**Roles.** In `preprocess` (`:13-29`):
- source = template × (112/112 = 1.0f).
- dest = detected keypoints, converted int → float (`dl_math_matrix.hpp:105-115`).
- `M = get_similarity_transform(source, dest)`, a 2x3 matrix mapping **output (template) coordinates to image coordinates**.
- It is passed with `inv=true` (`vision/image/dl_image_preprocessor.cpp:137-140`), so ESP-DL uses M directly as the dst→src map and does **not** invert it (`vision/image/dl_image_process.cpp:220-236`).

**`get_similarity_transform`** (`dl/math/src/dl_math_matrix.cpp:197-377`) is Umeyama (similarity, reflection-checked) via a closed-form 2x2 SVD. Its precision is mixed:
- Means, A, AAT/ATA, U, V, S and scale are `double`. The P4 has no D extension, so these run in soft-float.
- The demeaned coordinates are stored as `float`.
- Each A term is `float*float/num`, computed in float and then added into the double.
- Every square root is `sqrt_newton(float)`: the argument is truncated to float and the result is float (`dl/math/include/dl_math.hpp:83-97`):
```c
float result = x; do { last = result; result = (last + x/last) * 0.5; } while (|result-last| > 1e-5f);
```
- The result matrix T is float:
  - The 2x2 block is rounded to float first.
  - The translation is computed as `dst_mean - scale*(T00*src_mean_x + T01*src_mean_y)` in double, using the unscaled float T.
  - Then `T[r][c] *= scale`, double, rounded to float.
- `sim.py:similarity_transform` reproduces this statement by statement.
- Against a double-precision Umeyama it agrees to about 1e-6 relative. me2: M = [[3.1933823, 0.0644878, 307.85034], [-0.0644868, 3.1933820, 97.206276]].
  - The two off-diagonals differ in the 5th digit because of the float `sqrt_newton`, so do not substitute a textbook implementation if you want exactness.

### 3b. Warp: nearest neighbour in Q10 fixed point
Map generation (`dl_image_process.cpp:854-877`, shift = 10, `dl_image_process.hpp:118`). i is an output column (x1, y1) or an output row (x2, y2), from 0 to 111:
```c
x1[i] = lrintf(M0*i*1024);        y1[i] = lrintf(M3*i*1024);           // per output column
x2[r] = lrintf((M1*r + M2)*1024); y2[r] = lrintf((M4*r + M5)*1024);    // per output row
```
Sampling (`:559-576`, `:578-610`):
```c
x = (x1[c] + x2[r] + 512) >> 10;   y = (y1[c] + y2[r] + 512) >> 10;   // arithmetic shift = floor
if ((unsigned)x < W && (unsigned)y < H) dst = cvt(src[y][x]) else dst = bg;
```
- Output pixel (c, r) samples source pixel (x, y). This is the integer-centre convention: no ±0.5.
- lrintf rounds half to even, in the default FE_TONEAREST mode.
- There is no crop area here; the whole image is the source.
- **Border:** out-of-image samples take the background pixel. The background is RGB (0,0,0) passed through the same quantiser (`gen_bg`, `:695-753`; `bg_t` defaults `ori{} = 0`, `dl_image_process.hpp:109-116`), so all three channels are **-64**.
- The pixel conversion is identical to the detector's (RGB888 or RGB565 → BGR, then the LUT), via `warp_nn_fn` (`vision/image/dl_image_pixel_kernel.cpp:122-131`).

### 3c. Normalisation and quantisation
Implemented as a LUT (`vision/image/dl_image_norm_quant.hpp:16-31`): `q = sat(rne(((v - 127.5f) * (1.f/127.5f)) * 64))`, with exponent -6 so inv_scale = 64.
- The range is [-64, 64], so nothing saturates. v=0 gives -64, v=127 gives -1, v=128 gives 0, v=255 gives 64.
- **There are no exact ties.** The float LUT equals the integer formula `q = (2*(128*v - 16320) + 255) / 510` (floor division) for all 256 values; the simulation checks this.
- The same LUT applies to all channels, because mean and std are equal.

### 3d. Where float makes exactness hard
- **Compile flags.** esp-dl is compiled with **`-ffast-math -O3`** (`esp-dl/CMakeLists.txt:83`), so GCC may contract or reassociate the float code. Two choices matter:
  - Whether `(M1*r + M2)` is a fused `fmadd.s`. Likely yes, since fast-math implies fp-contract=fast and the F extension has fmadd. `sim.py` defaults to fused (`fma=1`).
  - Whether the four `/num` in the A loop become `*(1.f/num)`, through the reciprocal-math recip pass. `sim.py` defaults to division (`recip=0`).
  - I could not determine either without the compiled binary.
- **Measured sensitivity:**
  - fma on or off changes 0 of 12544 pixels on me2 and 1 on musk2.
  - recip changes M by about 1e-6 relative: 0 pixels on me2, 3 on musk2.
  - One ULP in M02 changed 0 pixels.
  - **The int8 network amplifies a single changed pixel visibly.** musk2–musk1 similarity is 0.8113, against 0.8136 with fma=0 and 0.8125 with recip=1.
- **Recommendation:** port the transform in float32/double exactly as `sim.py` does, keep the warp map in Q10 integers, and settle fma/recip by dumping ESP-DL's `M` and the 112x112x3 model input on the device for one image.

## 4. Postprocessing (`vision/recognition/dl_feat_postprocessor.cpp:6-30`)
- **Dequantise.** `m_feat` is a float TensorBase with exponent 0. `assign` dequantises with `(float)q * 2^-5` (`dl/tensor/src/dl_tensor_base.cpp:39-43`, `:293-303`). This is exact.
- **Normalise.** `norm = sqrt_newton(Σ f_i²)`, then `f_i /= norm` in float, sequentially.
  - Σ f² is exact in float: every term is k·2⁻¹⁰ and the sum is below 2²⁴·2⁻¹⁰.
- No other transform: no PCA, no flip-averaging, no clipping.
- **For an integer port, keep the int8 vector plus its integer Σq²**; cosine similarity is then scale-free (§5).

## 5. Matching (`vision/recognition/dl_recognition_database.cpp`)
- **Similarity** is the plain dot product of the two L2-normalised float vectors, i.e. cosine similarity (`:234-241`).
  - Float accumulation in order i = 0..511; fast-math may fuse it into fmadd.
  - An integer equivalent: `Σ qa·qb / sqrt(Σqa² · Σqb²)`. It differs from ESP-DL only in float rounding, about 1e-7.
- **Query** (`:243-267`):
  - Keep entries with `sim > thr`, strictly greater.
  - Sort descending (`std::sort`, which is not stable), then truncate to top_k.
  - Defaults: `m_thr = 0.5`, `m_top_k = 1` (`human_face_recognition.cpp:123`).
  - Returns {id, similarity}. **The id returned is the 1-based position in the in-memory list, not the stored id** (`:252-258`, where `it->id` is commented out). After a deletion the two differ.
- **Enrolment** (`:120-168`) stores one float32[512] per enrol call. A person is just an id; enrolling the same person several times gives several independent ids, with no averaging or grouping.
  - New id = `num_feats_total + 1`.
- **File format** (all little-endian; `dl_recognition_define.hpp:6-10`, `database.cpp:23-42`, `:66-118`):
```
header: uint16 num_feats_total, uint16 num_feats_valid, uint16 feat_len (=512)      // 6 bytes
record: uint16 id, float32 feat[512]                                                 // 2050 bytes, appended
```
- **Deletion** (`:170-222`) rewrites the record's id as 0 (a tombstone) and rewrites `num_feats_valid` at offset 2. Loading skips records with id 0 and checks the valid count.
- **No face:** `recognize` returns an empty result and `enroll` returns ESP_FAIL.

## 6. Other things needed for bit-exactness
- **Image decode.** The example decodes JPEG with `dl::image::sw_decode_jpeg`, which is not PIL. Golden vectors must start from the same RGB pixels: use PPM or raw frames, not JPEG.
- **Keypoints.** The keypoints are the detector's ints after clipping (docs §8). Any detector difference moves M.
- **Rounding modes.**
  - lrintf assumes the FPU's dynamic rounding mode is RNE.
  - PIE rounding is set separately by `dl_esp32p4_cfg_round`. The conv and PRelu kernels set half-even themselves.
- **PRelu half-even on the negative branch** is settled for MNP (docs §4) and assumed to hold here; this model has 35 PRelus, so it matters a lot.
- **Add saturation** (§2) is assumed from the instruction description; it has not been seen on the device.
- **Pad symmetry.** Nothing new: all pads are symmetric, and the 7x7 dw has no pad.
- **Termination of `sqrt_newton`.** It loops while `|Δ| > 1e-5f`. For large arguments the float iteration could in principle oscillate by 1 ulp > 1e-5 and never stop. It terminated for every case tried; the simulation raises an exception if it ever would not.

## 7. Reference implementation: `scratchpad/recogsim/sim.py`
**Usage**
- `python3 -I sim.py img.ppm x0 y0 … x4 y4 [dump=out.npz]` runs the full pipeline: transform, warp, int8 MFN, L2. It prints M, checksums of the model input, and the 512 int8 embedding values.
- `python3 -I sim.py img.jpg detect` takes the keypoints from `facesim/sim.py`.
- `python3 -I sim.py examples` reproduces the ESP-DL example.
- Options: `fma=0|1`, `recip=0|1`.
- The simulation asserts: bias exponent, positive shifts, equal Add/Concat exponents, Linear activations.

**me2.ppm** (`me2_golden.npz` has the input, embedding, feat, M and kp; `me2_aligned.png` is the aligned face, scaled ×3):
```
kp [433,268, 449,397, 494,298, 548,259, 533,394]
model input: sum -1060226, sumsq 42107156, pixel(0,0) BGR [-10,-12,-24]
embedding (e-5): sum -334, sumsq 418438, first 16: -8 59 -72 35 3 6 48 25 -10 11 7 -24 -5 -15 54 -29
```
**ESP-DL example** (`examples/human_face_recognition`, fetched with sparse-checkout): enrol bill1, bill2, musk1; query musk2. The JPEGs are decoded with PIL and the keypoints come from the detector simulation:
```
bill1 kp [134,106,134,153,158,136,181,105,181,153]   bill2 [135,124,144,178,168,152,185,117,185,173]
musk1 kp [125,137,134,189,152,160,175,132,173,186]   musk2 [110,144,113,199,131,174,159,140,162,196]
musk2 vs id1 0.0031, id2 -0.0011, id3 0.8113  -> result id 3      bill1 vs bill2 0.6213
```
- The README (`examples/human_face_recognition/README.md`) says `id: 3, sim: 0.750027`.
  - The winning id matches. The value does not: it is 0.81 here.
  - Expected causes: a different JPEG decoder, keypoints that differ by a pixel or so, and an unspecified build (the detector README was also off).
  - This is not a check of bit-exactness.

## Could not determine
- Whether ESP-DL's compiled code fuses `(M1*r+M2)` (fma) and whether it turns `/num` into a reciprocal multiply (§3d). Both change M by about an ulp and occasionally a sampled pixel.
- The exact float behaviour of the double soft-float code under `-ffast-math` (reassociation). The simulation follows source order.
- Add saturation and the whole MFN int8 path have not been checked on the device. No ESP-DL golden embedding exists in the repo; the only reference is the README similarity, 0.750027, which does not match.
