#include "simclock.h"

#define SECONDS_PER_DAY 86400L

static long offset_seconds = 0;

time_t sim_now(void) {
    return time(NULL) + (time_t)offset_seconds;
}

void sim_advance_days(int days) {
    offset_seconds += (long)days * SECONDS_PER_DAY;
}

long sim_offset_days(void) {
    return offset_seconds / SECONDS_PER_DAY;
}
