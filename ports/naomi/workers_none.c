/**
 *    Name:        workers_none.c
 *    Description: the video's workers (s32/workers.h) on the NAOMI: one processor, so the caller does all the rows
 *                 (runtime/src/workers.c without its threads, which KallistiOS's pthreads addon would bring)
 *    Created:     13-11-2026
 *
 *    license:BSD-3-Clause
 */
#include "s32/workers.h"

void s32_workers_start(int count)
{
	(void)count;
}

int s32_workers_count(void)
{
	return 0;
}

void s32_workers_rows(s32_rows_work work, void *context, int first, int last)
{
	work(context, first, last);
}
