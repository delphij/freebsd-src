/*	$OpenBSD: arc4random.c,v 1.26 2013/10/21 20:33:23 deraadt Exp $	*/

/*
 * Copyright (c) 1996, David Mazieres <dm@uun.org>
 * Copyright (c) 2008, Damien Miller <djm@openbsd.org>
 * Copyright (c) 2013, Markus Friedl <markus@openbsd.org>
 * Copyright (c) 2014, Xin Li <delphij@FreeBSD.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * ChaCha based random number generator for OpenBSD.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "namespace.h"
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/queue.h>
#include <sys/sysctl.h>
#include <pthread.h>

#include "libc_private.h"
#include "un-namespace.h"

#define KEYSTREAM_ONLY
#include "chacha_private.h"

#ifdef __GNUC__
#define inline __inline
#else				/* !__GNUC__ */
#define inline
#endif				/* !__GNUC__ */

#define KEYSZ	32
#define IVSZ	8
#define BLOCKSZ	64
#define RSBUFSZ	(16*BLOCKSZ)

#define	RANDOMDEV	"/dev/random"

/* rsq_mtx protects all rs_* and rs0 */
static pthread_mutex_t	rsq_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t	rsq_cv = PTHREAD_COND_INITIALIZER;

static int rsq_ncpu = -1;
static int rsq_count = 0;	/* Total random_state_t of existence */
static TAILQ_HEAD(random_state_head, random_state) rsq_head;
typedef struct random_state {
	int		_rs_initialized;
	pid_t		_rs_stir_pid;
	chacha_ctx	_rs;		/* chacha context for random keystream */
	u_char		_rs_buf[RSBUFSZ];	/* keystream blocks */
	size_t		_rs_have;		/* valid bytes at end of rs_buf */
	size_t		_rs_count;		/* bytes till reseed */
	TAILQ_ENTRY(random_state) entries;
} random_state_t;
static random_state_t rs0;

#define rs_initialized (rsp->_rs_initialized)
#define rs_stir_pid (rsp->_rs_stir_pid)
#define rs (rsp->_rs)
#define rs_buf (rsp->_rs_buf)
#define rs_have (rsp->_rs_have)
#define rs_count (rsp->_rs_count)

extern int __sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen);

/*
 * Look up for an usable random state
 */
static inline random_state_t *
_rs_get_state(void)
{
	random_state_t *qe;

	if (__isthreaded)
		_pthread_mutex_lock(&rsq_mtx);
	while ((qe = TAILQ_FIRST(&rsq_head)) == NULL) {
		if (rsq_ncpu == -1) {
			size_t size = sizeof(rsq_ncpu);

			if (sysctlbyname("hw.ncpu", &rsq_ncpu, &size, NULL, 0) < 0 ||
				size != sizeof(rsq_ncpu))
				rsq_ncpu = 1;
			/*
			 * We are here for the first time.  Use the statically
			 * allocated state so that we have at least one.
			 *
			 * assert(rsq_count == 0);
			 */
			qe = &rs0;
		} else if (rsq_count < rsq_ncpu)
			qe = calloc(1, sizeof(*qe));

		if (qe != NULL) {
			rsq_count++;
			break;
		} else {
			/*
			 * We do not have any available state at this
			 * time.  Sleep and wait for a wakeup.
			 *
			 * assert(__isthreaded);
			 */
			_pthread_cond_wait(&rsq_cv, &rsq_mtx);
		}
	}

	/*
	 * assert(qe != NULL);
	 *
	 * qe is either first element of rsq, or newly allocated.
	 * Remove it from the head if it's the first case.
	 */
	if (qe == TAILQ_FIRST(&rsq_head))
		TAILQ_REMOVE(&rsq_head, qe, entries);
	if (__isthreaded)
		_pthread_mutex_unlock(&rsq_mtx);

	return (qe);
}

/*
 * Put random state back to available random state queue and wake up potential
 * waiter.
 */
static inline void
_rs_put_state(random_state_t *rsp)
{

	if (__isthreaded)
		_pthread_mutex_lock(&rsq_mtx);
	TAILQ_INSERT_TAIL(&rsq_head, rsp, entries);
	if (__isthreaded) {
		_pthread_cond_signal(&rsq_cv);
		_pthread_mutex_unlock(&rsq_mtx);
	}
}

static inline void _rs_rekey(random_state_t *rsp, u_char *dat, size_t datlen);

static inline void
_rs_init(random_state_t *rsp, u_char *buf, size_t n)
{
	if (n < KEYSZ + IVSZ)
		return;
	chacha_keysetup(&rs, buf, KEYSZ * 8, 0);
	chacha_ivsetup(&rs, buf + KEYSZ);
}

static size_t
_rs_sysctl(u_char *buf, size_t size)
{
	int mib[2];
	size_t len, done = 0;

	mib[0] = CTL_KERN;
	mib[1] = KERN_ARND;

	do {
		len = size;
		if (__sysctl(mib, 2, buf, &len, NULL, 0) == -1)
			return (done);
		done += len;
		buf += len;
		size -= len;
	} while (size > 0);

	return (done);
}

static void
_rs_stir(random_state_t *rsp)
{
	int done, fd;
	union {
		u_char	_rnd[KEYSZ + IVSZ];
		struct {
			struct timeval	tv;
			pid_t		pid;
		} _pi;
	} rdat;
#define	rnd	(rdat._rnd)

	done = 0;
	if (_rs_sysctl((u_char *)&rnd, sizeof(rnd)) == sizeof(rnd))
		done = 1;
	if (!done) {
		fd = _open(RANDOMDEV, O_RDONLY | O_CLOEXEC, 0);
		if (fd >= 0) {
			if (_read(fd, &rnd, sizeof(rnd)) == sizeof(rnd))
				done = 1;
			(void)_close(fd);
		}
	}
	if (!done) {
		(void)gettimeofday(&rdat._pi.tv, NULL);
		rdat._pi.pid = getpid();
		/* We'll just take whatever was on the stack too... */
	}

	if (!rs_initialized) {
		rs_initialized = 1;
		_rs_init(rsp, rnd, sizeof(rnd));
	} else
		_rs_rekey(rsp, rnd, sizeof(rnd));
	memset(rnd, 0, sizeof(rnd));

	/* invalidate rs_buf */
	rs_have = 0;
	memset(rs_buf, 0, RSBUFSZ);

	rs_count = 1600000;
}

static inline void
_rs_stir_if_needed(random_state_t *rsp, size_t len)
{
	pid_t pid = getpid();

	if (rs_count <= len || !rs_initialized || rs_stir_pid != pid) {
		rs_stir_pid = pid;
		_rs_stir(rsp);
	} else
		rs_count -= len;
}

static inline void
_rs_rekey(random_state_t *rsp, u_char *dat, size_t datlen)
{
#ifndef KEYSTREAM_ONLY
	memset(rs_buf, 0,RSBUFSZ);
#endif
	/* fill rs_buf with the keystream */
	chacha_encrypt_bytes(&rs, rs_buf, rs_buf, RSBUFSZ);
	/* mix in optional user provided data */
	if (dat) {
		size_t i, m;

		m = MIN(datlen, KEYSZ + IVSZ);
		for (i = 0; i < m; i++)
			rs_buf[i] ^= dat[i];
	}
	/* immediately reinit for backtracking resistance */
	_rs_init(rsp, rs_buf, KEYSZ + IVSZ);
	memset(rs_buf, 0, KEYSZ + IVSZ);
	rs_have = RSBUFSZ - KEYSZ - IVSZ;
}

static inline void
_rs_random_buf(random_state_t *rsp, void *_buf, size_t n)
{
	u_char *buf = (u_char *)_buf;
	size_t m;

	_rs_stir_if_needed(rsp, n);
	while (n > 0) {
		if (rs_have > 0) {
			m = MIN(n, rs_have);
			memcpy(buf, rs_buf + RSBUFSZ - rs_have, m);
			memset(rs_buf + RSBUFSZ - rs_have, 0, m);
			buf += m;
			n -= m;
			rs_have -= m;
		}
		if (rs_have == 0)
			_rs_rekey(rsp, NULL, 0);
	}
}

static inline void
_rs_random_u32(random_state_t *rsp, u_int32_t *val)
{
	_rs_stir_if_needed(rsp, sizeof(*val));
	if (rs_have < sizeof(*val))
		_rs_rekey(rsp, NULL, 0);
	memcpy(val, rs_buf + RSBUFSZ - rs_have, sizeof(*val));
	memset(rs_buf + RSBUFSZ - rs_have, 0, sizeof(*val));
	rs_have -= sizeof(*val);
	return;
}

u_int32_t
arc4random(void)
{
	u_int32_t val;
	random_state_t *rsp;

	rsp = _rs_get_state();
	_rs_random_u32(rsp, &val);
	_rs_put_state(rsp);
	return val;
}

void
arc4random_buf(void *buf, size_t n)
{
	random_state_t *rsp;

	rsp = _rs_get_state();
	_rs_random_buf(rsp, buf, n);
	_rs_put_state(rsp);
}

/*
 * Calculate a uniformly distributed random number less than upper_bound
 * avoiding "modulo bias".
 *
 * Uniformity is achieved by generating new random numbers until the one
 * returned is outside the range [0, 2**32 % upper_bound).  This
 * guarantees the selected random number will be inside
 * [2**32 % upper_bound, 2**32) which maps back to [0, upper_bound)
 * after reduction modulo upper_bound.
 */
u_int32_t
arc4random_uniform(u_int32_t upper_bound)
{
	u_int32_t r, min;

	if (upper_bound < 2)
		return 0;

	/* 2**32 % x == (2**32 - x) % x */
	min = -upper_bound % upper_bound;

	/*
	 * This could theoretically loop forever but each retry has
	 * p > 0.5 (worst case, usually far better) of selecting a
	 * number inside the range we need, so it should rarely need
	 * to re-roll.
	 */
	for (;;) {
		r = arc4random();
		if (r >= min)
			break;
	}

	return r % upper_bound;
}

#if 0
/*-------- Test code for i386 --------*/
#include <stdio.h>
#include <machine/pctr.h>
int
main(int argc, char **argv)
{
	const int iter = 1000000;
	int     i;
	pctrval v;

	v = rdtsc();
	for (i = 0; i < iter; i++)
		arc4random();
	v = rdtsc() - v;
	v /= iter;

	printf("%qd cycles\n", v);
	exit(0);
}
#endif
