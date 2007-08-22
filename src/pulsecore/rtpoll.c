/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/utsname.h>
#include <sys/types.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <errno.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/rtclock.h>
#include <pulsecore/macro.h>
#include <pulsecore/llist.h>
#include <pulsecore/rtsig.h>
#include <pulsecore/flist.h>

#include "rtpoll.h"

struct pa_rtpoll {

    struct pollfd *pollfd, *pollfd2;
    unsigned n_pollfd_alloc, n_pollfd_used;

    int timer_enabled;
    struct timespec next_elapse;
    pa_usec_t period;

    int scan_for_dead;
    int running, installed, rebuild_needed;

#ifdef HAVE_PPOLL
    int rtsig;
    sigset_t sigset_unblocked;
    timer_t timer;
#ifdef __linux__
    int dont_use_ppoll;
#endif    
#endif
    
    PA_LLIST_HEAD(pa_rtpoll_item, items);
};

struct pa_rtpoll_item {
    pa_rtpoll *rtpoll;
    int dead;

    struct pollfd *pollfd;
    unsigned n_pollfd;

    int (*before_cb)(pa_rtpoll_item *i);
    void (*after_cb)(pa_rtpoll_item *i);
    void *userdata;
    
    PA_LLIST_FIELDS(pa_rtpoll_item);
};

PA_STATIC_FLIST_DECLARE(items, 0, pa_xfree);

static void signal_handler_noop(int s) { }

pa_rtpoll *pa_rtpoll_new(void) {
    pa_rtpoll *p;

    p = pa_xnew(pa_rtpoll, 1);

#ifdef HAVE_PPOLL

#ifdef __linux__
    /* ppoll is broken on Linux < 2.6.16 */
    
    p->dont_use_ppoll = 0;

    {
        struct utsname u;
        unsigned major, minor, micro;
    
        pa_assert_se(uname(&u) == 0);

        if (sscanf(u.release, "%u.%u.%u", &major, &minor, &micro) != 3 ||
            (major < 2) ||
            (major == 2 && minor < 6) ||
            (major == 2 && minor == 6 && micro < 16))

            p->dont_use_ppoll = 1;
    }

#endif

    p->rtsig = -1;
    sigemptyset(&p->sigset_unblocked);
    p->timer = (timer_t) -1;
        
#endif

    p->n_pollfd_alloc = 32;
    p->pollfd = pa_xnew(struct pollfd, p->n_pollfd_alloc);
    p->pollfd2 = pa_xnew(struct pollfd, p->n_pollfd_alloc);
    p->n_pollfd_used = 0;

    p->period = 0;
    memset(&p->next_elapse, 0, sizeof(p->next_elapse));
    p->timer_enabled = 0;

    p->running = 0;
    p->installed = 0;
    p->scan_for_dead = 0;
    p->rebuild_needed = 0;
    
    PA_LLIST_HEAD_INIT(pa_rtpoll_item, p->items);

    return p;
}

void pa_rtpoll_install(pa_rtpoll *p) {
    pa_assert(p);
    pa_assert(!p->installed);
    
    p->installed = 1;

#ifdef HAVE_PPOLL
    if (p->dont_use_ppoll)
        return;

    if ((p->rtsig = pa_rtsig_get_for_thread()) < 0) {
        pa_log_warn("Failed to reserve POSIX realtime signal.");
        return;
    }

    pa_log_debug("Acquired POSIX realtime signal SIGRTMIN+%i", p->rtsig - SIGRTMIN);

    {
        sigset_t ss;
        struct sigaction sa;
        
        pa_assert_se(sigemptyset(&ss) == 0);
        pa_assert_se(sigaddset(&ss, p->rtsig) == 0);
        pa_assert_se(pthread_sigmask(SIG_BLOCK, &ss, &p->sigset_unblocked) == 0);
        pa_assert_se(sigdelset(&p->sigset_unblocked, p->rtsig) == 0);

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = signal_handler_noop;
        pa_assert_se(sigemptyset(&sa.sa_mask) == 0);
        
        pa_assert_se(sigaction(p->rtsig, &sa, NULL) == 0);
        
        /* We never reset the signal handler. Why should we? */
    }
    
#endif
}

static void rtpoll_rebuild(pa_rtpoll *p) {

    struct pollfd *e, *t;
    pa_rtpoll_item *i;
    int ra = 0;
    
    pa_assert(p);

    p->rebuild_needed = 0;

    if (p->n_pollfd_used > p->n_pollfd_alloc) {
        /* Hmm, we have to allocate some more space */
        p->n_pollfd_alloc = p->n_pollfd_used * 2;
        p->pollfd2 = pa_xrealloc(p->pollfd2, p->n_pollfd_alloc * sizeof(struct pollfd));
        ra = 1;
    }

    e = p->pollfd2;

    for (i = p->items; i; i = i->next) {

        if (i->n_pollfd > 0)  {
            size_t l = i->n_pollfd * sizeof(struct pollfd);
            
            if (i->pollfd)
                memcpy(e, i->pollfd, l);
            else
                memset(e, 0, l);

            i->pollfd = e;
        } else
            i->pollfd = NULL;
        
        e += i->n_pollfd;
    }

    pa_assert((unsigned) (e - p->pollfd2) == p->n_pollfd_used);
    t = p->pollfd;
    p->pollfd = p->pollfd2;
    p->pollfd2 = t;
    
    if (ra)
        p->pollfd2 = pa_xrealloc(p->pollfd2, p->n_pollfd_alloc * sizeof(struct pollfd));

}

static void rtpoll_item_destroy(pa_rtpoll_item *i) {
    pa_rtpoll *p;

    pa_assert(i);

    p = i->rtpoll;

    PA_LLIST_REMOVE(pa_rtpoll_item, p->items, i);

    p->n_pollfd_used -= i->n_pollfd;
    
    if (pa_flist_push(PA_STATIC_FLIST_GET(items), i) < 0)
        pa_xfree(i);

    p->rebuild_needed = 1;
}

void pa_rtpoll_free(pa_rtpoll *p) {
    pa_assert(p);

    while (p->items)
        rtpoll_item_destroy(p->items);
    
    pa_xfree(p->pollfd);
    pa_xfree(p->pollfd2);

#ifdef HAVE_PPOLL
    if (p->timer != (timer_t) -1) 
        timer_delete(p->timer);
#endif
    
    pa_xfree(p);
}

int pa_rtpoll_run(pa_rtpoll *p) {
    pa_rtpoll_item *i;
    int r = 0;
    int no_events = 0;
    int saved_errno;
    struct timespec timeout;
    
    pa_assert(p);
    pa_assert(!p->running);
    pa_assert(p->installed);
    
    p->running = 1;

    for (i = p->items; i; i = i->next) {

        if (i->dead)
            continue;
        
        if (!i->before_cb)
            continue;

        if (i->before_cb(i) < 0) {

            /* Hmm, this one doesn't let us enter the poll, so rewind everything */

            for (i = i->prev; i; i = i->prev) {

                if (i->dead)
                    continue;
                
                if (!i->after_cb)
                    continue;

                i->after_cb(i);
            }
            
            goto finish;
        }
    }

    if (p->rebuild_needed)
        rtpoll_rebuild(p);

    /* Calculate timeout */
    if (p->timer_enabled) {
        struct timespec now;
        pa_rtclock_get(&now);

        if (pa_timespec_cmp(&p->next_elapse, &now) <= 0)
            memset(&timeout, 0, sizeof(timeout));
        else
            pa_timespec_store(&timeout, pa_timespec_diff(&p->next_elapse, &now));
    }
    
    /* OK, now let's sleep */
#ifdef HAVE_PPOLL

#ifdef __linux__
    if (!p->dont_use_ppoll)
#endif
        r = ppoll(p->pollfd, p->n_pollfd_used, p->timer_enabled > 0  ? &timeout : NULL, p->rtsig < 0 ? NULL : &p->sigset_unblocked);
#ifdef __linux__
    else
#endif

#else
        r = poll(p->pollfd, p->n_pollfd_used, p->timer_enabled > 0 ? (timeout.tv_sec*1000) + (timeout.tv_nsec / 1000000) : -1);
#endif

    saved_errno = errno;

    if (p->timer_enabled) {
        if (p->period > 0) {
            struct timespec now;
            pa_rtclock_get(&now);

            pa_timespec_add(&p->next_elapse, p->period);

            /* Guarantee that the next timeout will happen in the future */
            if (pa_timespec_cmp(&p->next_elapse, &now) < 0)
                pa_timespec_add(&p->next_elapse, (pa_timespec_diff(&now, &p->next_elapse) / p->period + 1)  * p->period);

        } else
            p->timer_enabled = 0;
    }
    
    if (r == 0 || (r < 0 && (errno == EAGAIN || errno == EINTR))) {
        r = 0;
        no_events = 1;
    }

    for (i = p->items; i; i = i->next) {

        if (i->dead)
            continue;

        if (!i->after_cb)
            continue;

        if (no_events) {
            unsigned j;

            for (j = 0; j < i->n_pollfd; j++)
                i->pollfd[j].revents = 0;
        }
        
        i->after_cb(i);
    }

finish:

    p->running = 0;
        
    if (p->scan_for_dead) {
        pa_rtpoll_item *n;

        p->scan_for_dead = 0;
        
        for (i = p->items; i; i = n) {
            n = i->next;

            if (i->dead)
                rtpoll_item_destroy(i);
        }
    }

    errno = saved_errno;

    return r;
}

static void update_timer(pa_rtpoll *p) {
    pa_assert(p);

#ifdef HAVE_PPOLL

#ifdef __linux__
    if (!p->dont_use_ppoll) {
#endif
        
        if (p->timer == (timer_t) -1) {
            struct sigevent se;

            memset(&se, 0, sizeof(se));
            se.sigev_notify = SIGEV_SIGNAL;
            se.sigev_signo = p->rtsig;

            if (timer_create(CLOCK_MONOTONIC, &se, &p->timer) < 0)
                if (timer_create(CLOCK_REALTIME, &se, &p->timer) < 0) {
                    pa_log_warn("Failed to allocate POSIX timer: %s", pa_cstrerror(errno));
                    p->timer = (timer_t) -1;
                }
        }

        if (p->timer != (timer_t) -1) {
            struct itimerspec its;
            memset(&its, 0, sizeof(its));

            if (p->timer_enabled) {
                its.it_value = p->next_elapse;

                /* Make sure that 0,0 is not understood as
                 * "disarming" */
                if (its.it_value.tv_sec == 0)
                    its.it_value.tv_nsec = 1;
                
                if (p->period > 0)
                    pa_timespec_store(&its.it_interval, p->period);
            }

            pa_assert_se(timer_settime(p->timer, TIMER_ABSTIME, &its, NULL) == 0);
        }

#ifdef __linux__
    }
#endif
    
#endif
}

void pa_rtpoll_set_timer_absolute(pa_rtpoll *p, const struct timespec *ts) {
    pa_assert(p);
    pa_assert(ts);
    
    p->next_elapse = *ts;
    p->period = 0;
    p->timer_enabled = 1;
    
    update_timer(p);
}

void pa_rtpoll_set_timer_periodic(pa_rtpoll *p, pa_usec_t usec) {
    pa_assert(p);

    p->period = usec;
    pa_rtclock_get(&p->next_elapse);
    pa_timespec_add(&p->next_elapse, usec);
    p->timer_enabled = 1;

    update_timer(p);
}

void pa_rtpoll_set_timer_relative(pa_rtpoll *p, pa_usec_t usec) {
    pa_assert(p);

    p->period = 0;
    pa_rtclock_get(&p->next_elapse);
    pa_timespec_add(&p->next_elapse, usec);
    p->timer_enabled = 1;

    update_timer(p);
}

void pa_rtpoll_set_timer_disabled(pa_rtpoll *p) {
    pa_assert(p);

    p->period = 0;
    memset(&p->next_elapse, 0, sizeof(p->next_elapse));
    p->timer_enabled = 0;

    update_timer(p);
}

pa_rtpoll_item *pa_rtpoll_item_new(pa_rtpoll *p, unsigned n_fds) {
    pa_rtpoll_item *i;
    
    pa_assert(p);
    pa_assert(n_fds > 0);

    if (!(i = pa_flist_pop(PA_STATIC_FLIST_GET(items))))
        i = pa_xnew(pa_rtpoll_item, 1);

    i->rtpoll = p;
    i->dead = 0;
    i->n_pollfd = n_fds;
    i->pollfd = NULL;

    i->userdata = NULL;
    i->before_cb = NULL;
    i->after_cb = NULL;
    
    PA_LLIST_PREPEND(pa_rtpoll_item, p->items, i);

    p->rebuild_needed = 1;
    p->n_pollfd_used += n_fds;

    return i;
}

void pa_rtpoll_item_free(pa_rtpoll_item *i) {
    pa_assert(i);

    if (i->rtpoll->running) {
        i->dead = 1;
        i->rtpoll->scan_for_dead = 1;
        return;
    }

    rtpoll_item_destroy(i);
}

struct pollfd *pa_rtpoll_item_get_pollfd(pa_rtpoll_item *i, unsigned *n_fds) {
    pa_assert(i);

    if (i->rtpoll->rebuild_needed)
        rtpoll_rebuild(i->rtpoll);
    
    if (n_fds)
        *n_fds = i->n_pollfd;
    
    return i->pollfd;
}

void pa_rtpoll_item_set_before_callback(pa_rtpoll_item *i, int (*before_cb)(pa_rtpoll_item *i)) {
    pa_assert(i);

    i->before_cb = before_cb;
}

void pa_rtpoll_item_set_after_callback(pa_rtpoll_item *i, void (*after_cb)(pa_rtpoll_item *i)) {
    pa_assert(i);

    i->after_cb = after_cb;
}

void pa_rtpoll_item_set_userdata(pa_rtpoll_item *i, void *userdata) {
    pa_assert(i);

    i->userdata = userdata;
}

void* pa_rtpoll_item_get_userdata(pa_rtpoll_item *i) {
    pa_assert(i);

    return i->userdata;
}

static int fdsem_before(pa_rtpoll_item *i) {
    return pa_fdsem_before_poll(i->userdata);
}

static void fdsem_after(pa_rtpoll_item *i) {
    pa_assert((i->pollfd[0].revents & ~POLLIN) == 0);
    pa_fdsem_after_poll(i->userdata);
}

pa_rtpoll_item *pa_rtpoll_item_new_fdsem(pa_rtpoll *p, pa_fdsem *f) {
    pa_rtpoll_item *i;
    struct pollfd *pollfd;
    
    pa_assert(p);
    pa_assert(f);

    i = pa_rtpoll_item_new(p, 1);

    pollfd = pa_rtpoll_item_get_pollfd(i, NULL);

    pollfd->fd = pa_fdsem_get(f);
    pollfd->events = POLLIN;
    
    i->before_cb = fdsem_before;
    i->after_cb = fdsem_after;
    i->userdata = f;

    return i;
}

static int asyncmsgq_before(pa_rtpoll_item *i) {
    return pa_asyncmsgq_before_poll(i->userdata);
}

static void asyncmsgq_after(pa_rtpoll_item *i) {
    pa_assert((i->pollfd[0].revents & ~POLLIN) == 0);
    pa_asyncmsgq_after_poll(i->userdata);
}

pa_rtpoll_item *pa_rtpoll_item_new_asyncmsgq(pa_rtpoll *p, pa_asyncmsgq *q) {
    pa_rtpoll_item *i;
    struct pollfd *pollfd;
    
    pa_assert(p);
    pa_assert(q);

    i = pa_rtpoll_item_new(p, 1);

    pollfd = pa_rtpoll_item_get_pollfd(i, NULL);
    pollfd->fd = pa_asyncmsgq_get_fd(q);
    pollfd->events = POLLIN;
    
    i->before_cb = asyncmsgq_before;
    i->after_cb = asyncmsgq_after;
    i->userdata = q;

    return i;
}
