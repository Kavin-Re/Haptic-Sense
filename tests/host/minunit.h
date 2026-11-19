/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tiny assert-based test harness for the host-side unit tests.
 * No dependencies beyond the C standard library.
 */
#ifndef MINUNIT_H
#define MINUNIT_H

#include <stdio.h>

static int mu_checks, mu_failures, mu_tests;

#define CHECK(cond) do {						\
	mu_checks++;							\
	if (!(cond)) {							\
		mu_failures++;						\
		printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);	\
	}								\
} while (0)

#define CHECK_EQ(actual, expected) do {					\
	long long mu_a = (long long)(actual);				\
	long long mu_e = (long long)(expected);				\
	mu_checks++;							\
	if (mu_a != mu_e) {						\
		mu_failures++;						\
		printf("  FAIL %s:%d: %s == %lld, expected %lld (%s)\n",	\
		       __FILE__, __LINE__, #actual, mu_a, mu_e, #expected);	\
	}								\
} while (0)

#define RUN(fn) do {							\
	int mu_before = mu_failures;					\
	mu_tests++;							\
	fn();								\
	printf("%s %s\n", mu_failures == mu_before ? "ok  " : "FAIL", #fn);	\
} while (0)

#define MU_REPORT()							\
	(printf("%d tests, %d checks, %d failures\n",			\
		mu_tests, mu_checks, mu_failures),			\
	 mu_failures ? 1 : 0)

#endif /* MINUNIT_H */
