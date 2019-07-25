/******************************************************************************
*
*   Copyright (c) 2019 Intel.
*
*   Licensed under the Apache License, Version 2.0 (the "License");
*   you may not use this file except in compliance with the License.
*   You may obtain a copy of the License at
*
*       http://www.apache.org/licenses/LICENSE-2.0
*
*   Unless required by applicable law or agreed to in writing, software
*   distributed under the License is distributed on an "AS IS" BASIS,
*   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*   See the License for the specific language governing permissions and
*   limitations under the License.
*
*******************************************************************************/


/**
 * @brief This file provides implementation to Timing for XRAN.
 *
 * @file xran_timer.c
 * @ingroup group_lte_source_xran
 * @author Intel Corporation
 *
 **/

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "xran_timer.h"
#include "xran_printf.h"
#ifndef MLOG_ENABLED
#include "mlog_lnx_xRAN.h"
#else
#include "mlog_lnx.h"
#endif
#include "xran_lib_mlog_tasks_id.h"
#include "ethdi.h"

#define NSEC_PER_SEC  1000000000L
#define NSEC_PER_USEC 1000L
#define THRESHOLD        35  /**< the avg cost of clock_gettime() in ns */
#define TIMECOMPENSATION 2   /**< time compensation in us, avg latency of clock_nanosleep */

#define SEC_MOD_STOP (30)

static struct timespec started_time;
static struct timespec last_time;
static struct timespec cur_time;

static struct timespec* p_cur_time = &cur_time;
static struct timespec* p_last_time = &last_time;

static struct timespec* p_temp_time;

static unsigned long current_second = 0;
static unsigned long started_second = 0;
extern uint32_t xran_lib_ota_sym;
extern uint32_t xran_lib_ota_tti;
extern uint32_t xran_lib_ota_sym_idx;

static int debugStop = 0;

uint64_t timing_get_current_second(void)
{
    return current_second;
}

int timing_set_debug_stop(int value)
{
    debugStop = value;

    if(debugStop){
        clock_gettime(CLOCK_REALTIME, &started_time);
        started_second =started_time.tv_sec;
    }
    return debugStop;
}

int timing_get_debug_stop(void)
{
    return debugStop;
}

long poll_next_tick(long interval_ns)
{
    long target_time;
    long delta;
    static int counter = 0;
    static long sym_acc = 0;
    static long sym_cnt = 0;

    if(counter){
       clock_gettime(CLOCK_REALTIME, p_last_time);
       current_second = p_last_time->tv_sec;
       counter = 1;
    }

    target_time = (p_last_time->tv_sec * NSEC_PER_SEC + p_last_time->tv_nsec + interval_ns);

    while(1)
    {
        clock_gettime(CLOCK_REALTIME, p_cur_time);
        delta = (p_cur_time->tv_sec * NSEC_PER_SEC + p_cur_time->tv_nsec) - target_time;
        if(delta > 0 || (delta < 0 && abs(delta) < THRESHOLD)) {
            if(current_second != p_cur_time->tv_sec){
                current_second = p_cur_time->tv_sec;
                xran_lib_ota_sym_idx = 0;
                xran_lib_ota_tti = 0;
                xran_lib_ota_sym = 0;
                sym_cnt = 0;
                sym_acc = 0;
                print_dbg("ToS:C Sync timestamp: [%ld.%09ld]\n", p_cur_time->tv_sec, p_cur_time->tv_nsec);
                if(debugStop){
                    if(p_cur_time->tv_sec > started_second && ((p_cur_time->tv_sec % SEC_MOD_STOP) == 0)){
                        uint64_t t1;
                        printf("STOP:[%ld.%09ld]\n", p_cur_time->tv_sec, p_cur_time->tv_nsec);
                        t1 = MLogTick();
                        rte_pause();
                        MLogTask(PID_TIME_SYSTIME_STOP, t1, MLogTick());
                        xran_if_current_state = XRAN_STOPPED;
                    }
                }
                p_cur_time->tv_nsec = 0; // adjust to 1pps
            } else {
                xran_lib_ota_sym_idx = XranIncrementSymIdx(xran_lib_ota_sym_idx, 14*8);
                /* adjust to sym boundary */
                if(sym_cnt & 1)
                    sym_acc += 8928L;
                else
                    sym_acc += 8929L;
                /* fine tune to second boundary */
                if(sym_cnt % 13 == 0)
                    sym_acc += 1;

                p_cur_time->tv_nsec = sym_acc;
                sym_cnt++;
            }
            if(debugStop && delta < interval_ns*10)
                MLogTask(PID_TIME_SYSTIME_POLL, (p_last_time->tv_sec * NSEC_PER_SEC + p_last_time->tv_nsec), (p_cur_time->tv_sec * NSEC_PER_SEC + p_cur_time->tv_nsec));
            p_temp_time = p_last_time;
            p_last_time = p_cur_time;
            p_cur_time  = p_temp_time;
            break;
        }
  }

  return delta;
}

long sleep_next_tick(long interval)
{
   struct timespec start_time;
   struct timespec cur_time;
   //struct timespec target_time_convert;
   struct timespec sleep_target_time_convert;
   long target_time;
   long sleep_target_time;
   long delta;

   clock_gettime(CLOCK_REALTIME, &start_time);
   target_time = (start_time.tv_sec * NSEC_PER_SEC + start_time.tv_nsec + interval * NSEC_PER_USEC) / (interval * NSEC_PER_USEC) * interval;
   //printf("target: %ld, current: %ld, %ld\n", target_time, start_time.tv_sec, start_time.tv_nsec);
   sleep_target_time = target_time - TIMECOMPENSATION;
   sleep_target_time_convert.tv_sec = sleep_target_time * NSEC_PER_USEC / NSEC_PER_SEC;
   sleep_target_time_convert.tv_nsec = (sleep_target_time * NSEC_PER_USEC) % NSEC_PER_SEC;

   //target_time_convert.tv_sec = target_time * NSEC_PER_USEC / NSEC_PER_SEC;
   //target_time_convert.tv_nsec = (target_time * NSEC_PER_USEC) % NSEC_PER_SEC;

   clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &sleep_target_time_convert, NULL);

   clock_gettime(CLOCK_REALTIME, &cur_time);

   delta = (cur_time.tv_sec * NSEC_PER_SEC + cur_time.tv_nsec) - target_time * NSEC_PER_USEC;

   return delta;
}


