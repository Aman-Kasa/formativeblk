#ifndef SIMCLOCK_H
#define SIMCLOCK_H

#include <time.h>

/* A simulated clock for demonstrating late returns and overdue books without
 * waiting two real weeks. sim_now() is the real time plus an offset that the
 * `advance <days>` command moves forward. Every lending event is timestamped
 * with sim_now(), so on-time / late / overdue decisions are consistent with
 * the timestamps stored in the blocks. The offset lasts for one session. */

time_t sim_now(void);
void sim_advance_days(int days);
long sim_offset_days(void);

#endif
