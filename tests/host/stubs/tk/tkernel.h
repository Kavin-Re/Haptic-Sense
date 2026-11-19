/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * HOST-TEST STUB of <tk/tkernel.h>. NOT the µT-Kernel header and never
 * compiled into firmware: it declares only the handful of types, constants
 * and service calls that the application sources under test reference, so
 * they can be compiled unmodified with a host gcc. The functions are
 * implemented by the test files themselves (see test_app_tasks.c).
 *
 * Widths follow the 32-bit target (W/UW are 32 bits there), not the host's
 * `long`, so integer overflow behaviour matches the firmware.
 */
#ifndef HOST_STUB_TKERNEL_H
#define HOST_STUB_TKERNEL_H

#include <stddef.h>
#include <stdint.h>

typedef int8_t   B;
typedef int16_t  H;
typedef int32_t  W;
typedef uint8_t  UB;
typedef uint16_t UH;
typedef uint32_t UW;
typedef int      INT;
typedef unsigned UINT;
typedef INT      ID;
typedef INT      ER;
typedef INT      BOOL;
typedef INT      PRI;
typedef INT      TMO;
typedef UW       RELTIM;
typedef UW       ATR;
typedef INT      SZ;
typedef void   (*FP)(void);

#define TRUE	1
#define FALSE	0

/* Error codes actually used by the code under test (µT-Kernel 3.0 values). */
#define E_OK	0
#define E_NOSPT	(-9)
#define E_PAR	(-17)
#define E_ID	(-18)
#define E_OBJ	(-41)
#define E_NOEXS	(-42)
#define E_QOVR	(-43)
#define E_TMOUT	(-50)
#define E_IO	(-57)

#define TMO_POL		0
#define TMO_FEVR	(-1)

#define TA_HLNG		0x00000001u
#define TA_USERBUF	0x00000020u
#define TA_RNG3		0x00000300u
#define TA_TFIFO	0x00000000u

typedef struct { W hi; UW lo; } SYSTIM;

typedef struct {
	void	*exinf;
	ATR	sematr;
	INT	isemcnt;
	INT	maxsem;
} T_CSEM;

typedef struct {
	void	*exinf;
	ID	wtsk;
	INT	semcnt;
} T_RSEM;

typedef struct {
	void	*exinf;
	ATR	tskatr;
	void	(*task)(INT stacd, void *exinf);
	PRI	itskpri;
	SZ	stksz;
	void	*bufptr;
} T_CTSK;

ID tk_cre_sem(const T_CSEM *pk_csem);
ER tk_wai_sem(ID semid, INT cnt, TMO tmout);
ER tk_sig_sem(ID semid, INT cnt);
ER tk_ref_sem(ID semid, T_RSEM *pk_rsem);
ID tk_cre_tsk(const T_CTSK *pk_ctsk);
ER tk_sta_tsk(ID tskid, INT stacd);
ER tk_slp_tsk(TMO tmout);
ER tk_dly_tsk(RELTIM dlytim);
ER tk_get_otm(SYSTIM *pk_tim);

#endif /* HOST_STUB_TKERNEL_H */
