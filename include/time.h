#ifndef __TIME_H
#define __TIME_H

#include "types.h"
#include "param.h"

struct timespec {
	uint64 sec;			/* seconds */
	uint64 nsec;		/* nanoseconds */
};

struct timeval {
	uint64 sec;			/* seconds */
	uint64 usec;		/* microseconds or milliseconds? */
};

#define CLOCK_REALTIME 		0

/**
 * 'time' is read from mtime register
 */
static inline void convert_to_timespec(uint64 time, struct timespec *ts)
{
	ts->sec = time / CLK_FREQ;
	ts->nsec = (time % CLK_FREQ)
				* 1000 * 1000 / (CLK_FREQ / 1000);
}

static inline void convert_to_timeval(uint64 time, struct timeval *tv)
{
	tv->sec = time / CLK_FREQ;
	tv->usec = (time % CLK_FREQ) * 1000 / (CLK_FREQ / 1000);
}

static inline uint64 convert_from_timespec(const struct timespec *ts)
{
	uint64 time = ts->sec * CLK_FREQ
					+ ts->nsec * (CLK_FREQ / 1000 / 100) / 10 / 1000;
	return time;
}

#endif