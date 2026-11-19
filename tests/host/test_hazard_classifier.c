/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for the integer-only CPU classifier,
 * firmware/Appli/Core/Src/app_hazard_classifier.c (17 -> 32 -> 16 -> 1,
 * INT8). The firmware source is #included unmodified so the tests can
 * reach its static helpers and read the pre-sigmoid logit directly.
 *
 * The held-out regression replays data/held_out_test_vectors_20260918.csv
 * (3,721 rows, already normalised to the firmware's milli-unit inputs)
 * through the real weights. It locks the confusion matrix this code
 * produces today; it does NOT re-derive the TFLite cross-check described
 * in app_hazard_classifier.h, which needs the TFLite interpreter.
 */
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"

#include "app_hazard_classifier.c"	/* firmware source, as-is */

static const char *g_csv_path = "../../data/held_out_test_vectors_20260918.csv";

/* Same forward pass as hazard_classify(), returning the logit byte instead
 * of the thresholded decision. Built only from the firmware's own static
 * helpers and constants. */
static int logit_of(const int32_t nf[HAZ_FEAT_COUNT])
{
	int8_t xq[HAZ_FEAT_COUNT], act1[32], act2[16], logit_q[1];
	int i;

	for (i = 0; i < HAZ_FEAT_COUNT; i++) {
		int64_t num = (int64_t)nf[i] * HAZ_IN_NUM;
		int32_t q;

		if (num >= 0)
			q = (int32_t)((num + HAZ_IN_DEN / 2) / HAZ_IN_DEN);
		else
			q = -(int32_t)((-num + HAZ_IN_DEN / 2) / HAZ_IN_DEN);
		xq[i] = clamp_i8(q + HAZ_IN_ZP);
	}
	fc_layer(xq, HAZ_IN_ZP, HAZ_W1, HAZ_B1, 32, 17, HAZ_FM1, HAZ_ACT1_ZP, act1);
	fc_layer(act1, HAZ_ACT1_ZP, HAZ_W2, HAZ_B2, 16, 32, HAZ_FM2, HAZ_ACT2_ZP, act2);
	fc_layer(act2, HAZ_ACT2_ZP, HAZ_W3, HAZ_B3, 1, 16, HAZ_FM3, HAZ_LOGIT_ZP, logit_q);
	return logit_q[0];
}

static void test_weight_table_shapes(void)
{
	CHECK_EQ(HAZ_FEAT_COUNT, 17);
	CHECK_EQ(sizeof(HAZ_W1), 32u * 17u);
	CHECK_EQ(sizeof(HAZ_W2), 16u * 32u);
	CHECK_EQ(sizeof(HAZ_W3), 1u * 16u);
	CHECK_EQ(sizeof(HAZ_B1) / sizeof(HAZ_B1[0]), 32u);
	CHECK_EQ(sizeof(HAZ_B2) / sizeof(HAZ_B2[0]), 16u);
	CHECK_EQ(sizeof(HAZ_B3) / sizeof(HAZ_B3[0]), 1u);
}

static void test_clamp_and_requantize(void)
{
	CHECK_EQ(clamp_i8(127), 127);
	CHECK_EQ(clamp_i8(128), 127);
	CHECK_EQ(clamp_i8(-128), -128);
	CHECK_EQ(clamp_i8(-129), -128);
	CHECK_EQ(clamp_i8(INT32_MAX), 127);
	CHECK_EQ(clamp_i8(INT32_MIN), -128);

	/* fm = 2^39 is a multiplier of exactly 0.5; rounding is to nearest,
	 * ties away from zero, symmetric in sign. */
	CHECK_EQ(requantize(4, (int64_t)1 << 39), 2);
	CHECK_EQ(requantize(3, (int64_t)1 << 39), 2);
	CHECK_EQ(requantize(1, (int64_t)1 << 39), 1);
	CHECK_EQ(requantize(-1, (int64_t)1 << 39), -1);
	CHECK_EQ(requantize(-3, (int64_t)1 << 39), -2);
	CHECK_EQ(requantize(0, HAZ_FM1), 0);
}

static void test_decision_matches_logit_threshold(void)
{
	int32_t nf[HAZ_FEAT_COUNT];
	unsigned seed = 12345u;
	int i, k;

	for (k = 0; k < 2000; k++) {
		for (i = 0; i < HAZ_FEAT_COUNT; i++) {
			seed = seed * 1103515245u + 12345u;
			nf[i] = (int32_t)((seed >> 8) % 2001u) - 1000;
		}
		CHECK_EQ(hazard_classify(nf),
			 logit_of(nf) >= HAZ_LOGIT_THRESH_Q ? 1 : 0);
	}
}

static void test_out_of_range_inputs_saturate(void)
{
	int32_t lo[HAZ_FEAT_COUNT], hi[HAZ_FEAT_COUNT];
	int32_t lo_sat[HAZ_FEAT_COUNT], hi_sat[HAZ_FEAT_COUNT];
	int i;

	/* The caller clamps to [-1000, 1000], but the classifier must not
	 * misbehave if it is ever handed more: the int8 input quantiser
	 * saturates at 127 (reached by x >= 1004) and at -128 (x <= -996), so
	 * anything beyond those points must classify exactly like them. */
	for (i = 0; i < HAZ_FEAT_COUNT; i++) {
		lo[i] = INT32_MIN;
		hi[i] = INT32_MAX;
		lo_sat[i] = -996;
		hi_sat[i] = 1004;
	}
	CHECK_EQ(logit_of(lo), logit_of(lo_sat));
	CHECK_EQ(logit_of(hi), logit_of(hi_sat));
	CHECK(hazard_classify(lo) <= 1);
	CHECK(hazard_classify(hi) <= 1);
}

static void test_deterministic_and_stateless(void)
{
	int32_t a[HAZ_FEAT_COUNT] = { -800, -700, -600, -500, -400, -300, -200,
				      -100, 0, 100, 400, 20, -24, -1, 282, 500, 300 };
	int32_t b[HAZ_FEAT_COUNT] = { 0 };
	int la = logit_of(a), lb = logit_of(b);

	(void)hazard_classify(b);
	CHECK_EQ(logit_of(a), la);
	(void)hazard_classify(a);
	CHECK_EQ(logit_of(b), lb);
}

/* --- held-out regression ------------------------------------------------- */

typedef struct { int tp, fp, fn, tn; } confusion_t;

static int load_and_score(const char *path, int thresh, confusion_t *c, int *rows)
{
	char line[1024];
	FILE *f = fopen(path, "r");

	memset(c, 0, sizeof(*c));
	*rows = 0;
	if (f == NULL)
		return -1;
	if (fgets(line, sizeof(line), f) == NULL) {	/* header */
		fclose(f);
		return -1;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		int32_t nf[HAZ_FEAT_COUNT];
		char *p = line;
		int k, label, pred;

		/* skip source_file,surface,angle,speed */
		for (k = 0; k < 4 && p != NULL; k++) {
			p = strchr(p, ',');
			if (p != NULL)
				p++;
		}
		if (p == NULL)
			continue;
		for (k = 0; k < HAZ_FEAT_COUNT; k++) {
			nf[k] = (int32_t)strtol(p, &p, 10);
			if (*p == ',')
				p++;
		}
		label = (int)strtol(p, NULL, 10);
		pred = logit_of(nf) >= thresh;
		if (pred && label) c->tp++;
		else if (pred && !label) c->fp++;
		else if (!pred && label) c->fn++;
		else c->tn++;
		(*rows)++;
	}
	fclose(f);
	return 0;
}

static void test_held_out_regression(void)
{
	confusion_t c;
	int rows;

	if (load_and_score(g_csv_path, HAZ_LOGIT_THRESH_Q_V4_LOCKED, &c, &rows) != 0) {
		printf("  cannot open %s\n", g_csv_path);
		CHECK(!"held-out CSV missing (pass its path as argv[1])");
		return;
	}
	CHECK_EQ(rows, 3721);
	CHECK_EQ(c.tp + c.fn, 857);	/* positives in the held-out set */

	/*
	 * v4 weights at v4's own logit threshold (21, "p = 0.20"):
	 * recall 696/857 = 81.2 %, precision 696/1084 = 64.2 %. The README /
	 * model/README.md quote 81.3 % / 64.1 % for the same operating point
	 * taken on the float probability output; the one-or-two-row difference
	 * is the integer threshold, not a disagreement.
	 */
	CHECK_EQ(c.tp, 696);
	CHECK_EQ(c.fp, 388);
	CHECK_EQ(c.fn, 161);
	CHECK_EQ(c.tn, 2476);
	printf("  v4 weights @ logit>=%d: recall %.1f %%, precision %.1f %%\n",
	       HAZ_LOGIT_THRESH_Q_V4_LOCKED,
	       100.0 * c.tp / (c.tp + c.fn), 100.0 * c.tp / (c.tp + c.fp));

	/*
	 * Same weights at the threshold the CPU path actually compiles with
	 * (HAZ_LOGIT_THRESH_Q = 35, derived from the later v10 model -- the
	 * mismatch documented in model/README.md). Locked here so the cost of
	 * that mismatch on the CPU path stays visible: recall drops to 51.8 %.
	 * The shipped build uses the NPU path, whose threshold is separate.
	 */
	CHECK_EQ(load_and_score(g_csv_path, HAZ_LOGIT_THRESH_Q, &c, &rows), 0);
	CHECK_EQ(c.tp, 444);
	CHECK_EQ(c.fp, 132);
	CHECK_EQ(c.fn, 413);
	CHECK_EQ(c.tn, 2732);
	printf("  v4 weights @ logit>=%d (as compiled): recall %.1f %%, precision %.1f %%\n",
	       HAZ_LOGIT_THRESH_Q,
	       100.0 * c.tp / (c.tp + c.fn), 100.0 * c.tp / (c.tp + c.fp));
}

int main(int argc, char **argv)
{
	if (argc > 1)
		g_csv_path = argv[1];

	RUN(test_weight_table_shapes);
	RUN(test_clamp_and_requantize);
	RUN(test_decision_matches_logit_threshold);
	RUN(test_out_of_range_inputs_saturate);
	RUN(test_deterministic_and_stateless);
	RUN(test_held_out_regression);
	return MU_REPORT();
}
