/*
 *  Copyright (C) 2007-2015 Lonelycoder AB
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  This program is also available under a commercial proprietary license.
 *  For more information, contact andreas@lonelycoder.com
 */
#include <time.h>
#include "main.h"
#include "prop/prop.h"
#include "callout.h"
#include "arch/arch.h"

static LIST_HEAD(, callout) callouts;

static hts_mutex_t callout_mutex;
static hts_cond_t callout_cond;

/**
 *htsmsg_t *props
 */
static int
calloutcmp(callout_t *a, callout_t *b)
{
  if(a->c_deadline < b->c_deadline)
    return -1;
  else if(a->c_deadline > b->c_deadline)
    return 1;
 return 0;
}


/**
 *
 */
static void
callout_arm_abs(callout_t *d, callout_callback_t *callback, void *opaque,
		uint64_t deadline, const char *file, int line)
{
  hts_mutex_lock(&callout_mutex);

  if(d == NULL)
    d = malloc(sizeof(callout_t));
  else if(d->c_callback != NULL)
    LIST_REMOVE(d, c_link);
    
  d->c_callback = callback;
  d->c_opaque = opaque;
  d->c_deadline = deadline;
  d->c_armed_by_file = file;
  d->c_armed_by_line = line;

  LIST_INSERT_SORTED(&callouts, d, c_link, calloutcmp, callout_t);
  hts_cond_signal(&callout_cond);
  hts_mutex_unlock(&callout_mutex);
}

/**
 *
 */
void
callout_arm_x(callout_t *d, callout_callback_t *callback,
              void *opaque, int delta,
              const char *file, int line)
{
  uint64_t deadline = arch_get_ts() + delta * 1000000LL;
  callout_arm_abs(d, callback, opaque, deadline, file, line);
}

/**
 *
 */
void
callout_arm_hires_x(callout_t *d, callout_callback_t *callback,
                    void *opaque, uint64_t delta,
                    const char *file, int line)
{
  uint64_t deadline = arch_get_ts() + delta;
  callout_arm_abs(d, callback, opaque, deadline, file, line);
}

/**
 *
 */
void
callout_disarm(callout_t *d)
{
  hts_mutex_lock(&callout_mutex);
  if(d->c_callback) {
    LIST_REMOVE(d, c_link);
    d->c_callback = NULL;
  }
  hts_mutex_unlock(&callout_mutex);
}


/**
 *
 */
static void *
callout_loop(void *aux)
{
  uint64_t now;
  callout_t *c;
  callout_callback_t *cc;

  hts_mutex_lock(&callout_mutex);

  while(1) {

    now = arch_get_ts();

    while((c = LIST_FIRST(&callouts)) != NULL && c->c_deadline <= now) {
      cc = c->c_callback;
      LIST_REMOVE(c, c_link);
      c->c_callback = NULL;
      const char *file = c->c_armed_by_file;
      int line         = c->c_armed_by_line;
      hts_mutex_unlock(&callout_mutex);
      cc(c, c->c_opaque);
      hts_mutex_lock(&callout_mutex);
      int64_t ts = arch_get_ts();
      if(ts - now > 1000000)
        TRACE(TRACE_DEBUG, "Callout", "%s:%d executed for %dus",
              file, line, (int)(ts - now));
      now = ts;
    }

    if((c = LIST_FIRST(&callouts)) != NULL) {

      int timeout = (c->c_deadline - now + 999) / 1000;
      hts_cond_wait_timeout(&callout_cond, &callout_mutex, timeout);
    } else {
      hts_cond_wait(&callout_cond, &callout_mutex);
    }
  }

  return NULL;
}


static callout_t callout_clock;

static prop_t *prop_clock;

/**
 *
 */
static void
set_global_clock(struct callout *c, void *aux)
{
  callout_update_clock_props();
}


void
callout_update_clock_props(void)
{
  time_t now;
  struct tm tm;

  time(&now);
  if(now < 1000000000) {
    prop_set(prop_clock, "valid", PROP_SET_INT, 0);
    callout_arm(&callout_clock, set_global_clock, NULL, 2);
    return;
  }
  prop_set(prop_clock, "valid", PROP_SET_INT, 1);

  prop_set(prop_clock, "unixtime", PROP_SET_INT, now);

  arch_localtime(&now, &tm);

  prop_set(prop_clock, "hour",      PROP_SET_INT, tm.tm_hour);
  prop_set(prop_clock, "minute",    PROP_SET_INT, tm.tm_min);
  prop_set(prop_clock, "dayminute", PROP_SET_INT, tm.tm_hour * 60 + tm.tm_min);

  callout_arm(&callout_clock, set_global_clock, NULL, 61 - tm.tm_sec);
}


/**
 *
 */
void
callout_init(void)
{

  hts_mutex_init(&callout_mutex);
  hts_cond_init(&callout_cond, &callout_mutex);

  hts_thread_create_detached("callout", callout_loop, NULL,
			     THREAD_PRIO_BGTASK);

  prop_clock = prop_create(prop_get_global(), "clock");
  set_global_clock(NULL, NULL);
}
