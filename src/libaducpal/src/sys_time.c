#include "aducpal/sys_time.h"
#include "aducpal/time.h" // clock_gettime

#ifdef ADUCPAL_USE_PAL

// Avoiding bringing in stdio.h
#    ifndef NULL
#        define NULL ((void*)0)
#    endif

int ADUCPAL_gettimeofday(struct timeval* tv, void* z)
{
    if (z != NULL)
    {
        __debugbreak();
    }

    struct timespec ts;
    ADUCPAL_clock_gettime(CLOCK_REALTIME, &ts);

    tv->tv_sec = ts.tv_sec;
    // Not used in this project.
    // tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}

#endif // #ifdef ADUCPAL_USE_PAL