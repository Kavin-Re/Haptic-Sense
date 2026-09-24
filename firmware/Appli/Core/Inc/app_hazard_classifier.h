/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * app_hazard_classifier.h — quantized MLP hazard classifier (Haptic-Sense,
 * Block 7/8). Replaces the Phase 4/CLAUDE.md-§6 stub rule
 * (d<800mm && v>20cm/s) in inference_task with the Edge-Impulse-trained
 * model, per app_tasks.c's own "Phase 6 replaces ONLY the classify step"
 * design comment.
 *
 * INTEGER ONLY, DELIBERATELY -- same reason as velocity_lsq.h: inference_task
 * runs at TK_PRI 2, one priority below hazard_task (TK_PRI 1, the task that
 * fires the haptic alert and is the one whose preemption latency the Block 9
 * timing campaign measures). Touching the FPU here would latch
 * CONTROL.FPCA for inference_task's context, and every later preemption of
 * it by hazard_task would then stack S16-S31 on top of the normal context
 * switch -- extra cycles on exactly the path that number is supposed to
 * bound. hazard_classify() therefore does the entire forward pass (three
 * FullyConnected layers, int8 in/out) in int32/int64 fixed-point, no float,
 * no libm, no sigmoid call -- the final decision is a plain integer compare
 * against a pre-derived logit threshold (see HAZ_LOGIT_THRESH_Q020 below),
 * since sigmoid is monotonic and the threshold only needs to be evaluated
 * in logit space, not probability space.
 *
 * SELF-CONTAINED ON PURPOSE (only <stdint.h>): no tkernel/HAL dependency,
 * so this module can be unit-tested and cross-validated on the host against
 * the real .tflite (as it was -- see the exact-match note below) without
 * pulling in any firmware scaffolding.
 *
 * VALIDATION: hazard_classify()'s fixed-point core was cross-checked against
 * ai_edge_litert.Interpreter (BUILTIN_REF resolver, experimental_preserve_
 * all_tensors) running the real tflite_learn_1115874_3.tflite on all 3721
 * rows of held_out_test_vectors_20260918.csv: the int8 pre-sigmoid logit
 * this code computes matched the interpreter's own internal logit tensor
 * EXACTLY (0 mismatches, max abs diff 0) for every row. The threshold
 * constants below were derived from that same validated run -- do not
 * hand-tune them without re-running the cross-check.
 */
#ifndef APP_HAZARD_CLASSIFIER_H
#define APP_HAZARD_CLASSIFIER_H

#include <stdint.h>

/* Feature count and order MUST match feature_buf / FEAT_COUNT in
 * app_tasks.c: [d0..d9 (mm), v (cm/s), a (cm/s^2), ax, ay, az (mg),
 * amb, spad (counts)] -- same order the training CSV used (app_tasks.c's
 * own [CSV] tm_printf line). If FEAT_COUNT / FEAT_IDX_* ever change,
 * this module and the trained model both go stale together. */
#define HAZ_FEAT_COUNT 17

/* Shared between the CPU (app_hazard_classifier.c) and NPU
 * (app_hazard_classifier_npu.c) paths -- both must quantize input and
 * threshold the output IDENTICALLY, or they'd silently diverge on the
 * same reading. Moved here (2026-09-18, NPU integration) from being a
 * private #define in app_hazard_classifier.c -- values unchanged, still
 * the ones cross-validated bit-exact in that file's VALIDATION note. */
#define HAZ_IN_ZP        (-1)
#define HAZ_IN_NUM       255
#define HAZ_IN_DEN       2000

#define HAZ_LOGIT_THRESH_Q015   18
#define HAZ_LOGIT_THRESH_Q020   21
#define HAZ_LOGIT_THRESH_Q025   24
#define HAZ_LOGIT_THRESH_Q_V4_LOCKED HAZ_LOGIT_THRESH_Q020  /* superseded 2026-09-19, see below */

/* SUPERSEDED 2026-09-19 -- same reason/history as HAZ_PROB_THRESH_Q below:
 * v10 (training_data_comprehensive_v5_clipped_20260919.csv) is a different
 * trained model than whatever HAZ_LOGIT_THRESH_Q020/015/025 above were
 * derived against -- a different model has a different logit mapping, this
 * constant does not carry over.
 *
 * ATTEMPT 1 for v10 (2026-09-19, PROVISIONAL, NOT YET LIVE-VALIDATED):
 * raw_thresh=35 against tensor 9 ("sequential/y_pred/MatMul;BiasAdd", scale
 * 0.10263491421937943, zero_point=43) of the v10 .tflite, cross-checked
 * against a direct sweep over held_out_test_vectors_WITH_ANGLE_20260918.csv's
 * real rows (see held_out_v10_logit_scored.csv): recall=0.827 precision=
 * 0.625, IDENTICAL operating point to HAZ_PROB_THRESH_Q=-51 below (both
 * correspond to probability threshold ~0.30 on the same v10 model --
 * verified: sigmoid(dequantized tensor-9 logit) matches the dequantized
 * tensor-10 probability to within 0.0036 across all 3721 rows).
 *
 * This firmware's own hazard_classify() computes its int8 pre-sigmoid logit
 * in this exact tensor-9 space (see this file's VALIDATION note above) --
 * so raw_thresh=35 is directly comparable to logit_q[0] at runtime, and to
 * app_tasks.c's own [NPUDIAG] "pre=" field in a live log.
 *
 * DO NOT LOCK THIS IN FROM THE CSV ALONE -- same live-validation
 * requirement as HAZ_LOGIT_THRESH_Q_V4_LOCKED's own history above. Validate
 * against the "pre=" field in a live capture (see analyze_live_capture.py)
 * before treating this as final. */
#define HAZ_LOGIT_THRESH_Q      35

/* HAZ_PROB_THRESH_Q -- NPU-path-ONLY threshold. NOT interchangeable with
 * HAZ_LOGIT_THRESH_Q above despite both being "the decision threshold":
 * hazard_classify_npu() (app_hazard_classifier_npu.c) compares the NPU's
 * raw output byte against this constant, and that byte lives in a
 * DIFFERENT quantization space than the CPU path's logit_q[0].
 *
 * The NPU necessarily runs the model's complete compiled graph, so its
 * output is tensor 10 ("Quantize_14_out_0", the real POST-SIGMOID
 * PROBABILITY output, scale=0.00390625, zero_point=-128) -- not tensor 9,
 * the PRE-SIGMOID LOGIT tensor the CPU path reads via TFLite's
 * experimental_preserve_all_tensors (specifically so the CPU path can skip
 * a runtime sigmoid call). Using HAZ_LOGIT_THRESH_Q020=21 against tensor
 * 10's raw byte (as the code briefly did, 2026-09-18) compares incompatible
 * units -- it "worked" well enough to not look obviously broken (sigmoid is
 * monotonic) while leaving real accuracy on the table and being wrong in
 * principle.
 *
 * ATTEMPT 1 (2026-09-18, superseded): T=-53, from sweeping held_out_test_
 * vectors_20260918.csv alone (max accuracy 86.56%). Recaptured live
 * (npu_diag_run6_20260918_1903.log, 97 frames, 81 real cpu=1 hazard
 * events): recall=0.000. The offline CSV's raw-byte range does not match
 * what real captures produce -- do not re-derive from the CSV alone.
 *
 * ATTEMPT 2 (2026-09-18, superseded): T=-76, from run6 alone (recall=0.926
 * precision=0.987 on that single capture) plus an independent offline
 * derivation agreeing. Recaptured on a SECOND live session
 * (npu_diag_run7_20260919_0047.log, 14 frames, 6 real cpu=1 events):
 * recall=0.333 -- 4/6 hazard frames had poste in -80..-84, just past -76.
 * One capture, however large, is still one sample of whatever motions
 * happened to get performed that session -- not proof a threshold
 * generalizes.
 *
 * CURRENT (2026-09-19, from FOUR live captures pooled: run6, run7_0047,
 * run7_0052, run7_0054 -- 137 frames total / 92 real cpu=1 hazard events
 * / 45 real cpu=0 no-hazard frames): the flat plateau holds up under more
 * data, T=-94 down through T=-84 all score identically, recall=1.000
 * fn=0 (every single hazard event across all four sessions caught) --
 * but precision is 0.902 (tp=92 fp=10 tn=35), not the 0.978 an earlier,
 * smaller two-capture pool suggested -- run7_0054 alone added 7 more
 * false positives clustered at -84..-71 that the first pool didn't have.
 * T=-87 is centered in the still-flat zone. Read this as: recall at this
 * threshold is now well-established (zero misses across four independent
 * sessions covering a real mix of motion); the false-alarm rate is real
 * and closer to ~1-in-10 than ~1-in-45 -- acceptable for a hazard warning
 * (false alarms are cheap, missed hazards are not) but don't cite the
 * higher precision figure again. Do NOT re-tune from a single capture --
 * this constant was wrong twice already for exactly that reason. Any
 * future change requires pooling multiple independent live captures, the
 * same standard this value was held to. */
#define HAZ_PROB_THRESH_Q_V4_LOCKED  (-87)  /* superseded 2026-09-19, see below */

/* SUPERSEDED 2026-09-19 -- retrained model (v10, trained on
 * training_data_comprehensive_v5_clipped_20260919.csv after the near-field
 * unclamped-noise bug fix; see SYNTH_DATA_METHODOLOGY.md addendum). -87
 * above was derived and live-validated against the OLDER (v4-family)
 * weights ONLY -- a different trained model has a different probability
 * mapping, so that constant does not carry over and must not be reused.
 *
 * ATTEMPT 1 for v10 (2026-09-19, PROVISIONAL, NOT YET LIVE-VALIDATED):
 * T=-51, derived from v10 run on held_out_test_vectors_WITH_ANGLE_20260918.csv
 * ALONE (3721 real rows, 100% held-out) -- overall recall=0.827
 * precision=0.625 at this point (probability threshold 0.30 dequantized:
 * raw_byte = round(0.30*256)-128 = -51). Per-angle at this point: left
 * rec=0.769, right rec=0.807, low rec=0.718, straight rec=0.884, high
 * rec=0.353 (still the weak angle). Near-field (0-100/100-200mm) recall on
 * this same offline set is ~0%, unchanged/slightly worse despite the
 * near-field training boost -- known, documented limitation, not fixed
 * by this retrain.
 *
 * DO NOT LOCK THIS IN FROM THE CSV ALONE -- this project already learned
 * that lesson the hard way (see ATTEMPT 1/2 history above HAZ_PROB_THRESH_
 * Q_V4_LOCKED: an earlier CSV-only-derived threshold for the old model
 * scored 86% offline and recall=0.000 on the first live capture). Live-
 * validate T=-51 against multiple independent live captures exactly as
 * done for the v4 constant above before treating it as final. */
#define HAZ_PROB_THRESH_Q       (-51)

/*
 * hazard_classify() input convention: NOT raw feature_buf values. Caller
 * must first apply the SAME norm1000()/NORM_* normalization app_tasks.c's
 * CSV_LOG_ENABLE logger already applies before printing each column (mm/
 * cm/s/mg/counts -> milli-units in [-1000,1000]) -- that normalized form is
 * what the model was trained on (the EI upload CSV came from this exact
 * logger). Do NOT pass feature_buf[] directly; see the call site in
 * inference_task_fct for the required per-feature norm1000() calls (they
 * mirror the CSV logger's own argument list line for line, so the two
 * paths cannot drift independently).
 *
 * Returns 1 if the model's hazard probability is >= the locked decision
 * threshold, 0 otherwise. Fixed cost (~1100 MACs, no branches on data
 * value, no allocation, no I/O) -- safe to call from inference_task.
 */
uint8_t hazard_classify(const int32_t norm_feat[HAZ_FEAT_COUNT]);

#endif /* APP_HAZARD_CLASSIFIER_H */
