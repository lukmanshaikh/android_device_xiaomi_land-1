/*
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * *    * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_NIDEBUG 0

#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdlib.h>

#define LOG_TAG "QTI PowerHAL"
#include <log/log.h>
#include <hardware/hardware.h>
#include <hardware/power.h>

#include "utils.h"
#include "metadata-defs.h"
#include "hint-data.h"
#include "performance.h"
#include "power-common.h"

static int saved_interactive_mode = -1;
static int display_hint_sent;
static int video_encode_hint_sent;

extern void interaction(int duration, int num_args, int opt_list[]);

static void process_video_encode_hint(void *metadata)
{
    char governor[80];
    struct video_encode_metadata_t video_encode_metadata;

    ALOGI("Got process_video_encode_hint");

    if (get_scaling_governor_check_cores(governor,
                sizeof(governor),CPU0) == -1) {
        if (get_scaling_governor_check_cores(governor,
                    sizeof(governor),CPU1) == -1) {
            if (get_scaling_governor_check_cores(governor,
                        sizeof(governor),CPU2) == -1) {
                if (get_scaling_governor_check_cores(governor,
                            sizeof(governor),CPU3) == -1) {
                    ALOGE("Can't obtain scaling governor.");
                    return;
                }
            }
        }
    }

    if (!metadata) {
        return;
    }

    /* Initialize encode metadata struct fields. */
    memset(&video_encode_metadata, 0, sizeof(struct video_encode_metadata_t));
    video_encode_metadata.state = -1;
    video_encode_metadata.hint_id = DEFAULT_VIDEO_ENCODE_HINT_ID;

    if (parse_video_encode_metadata((char *)metadata,
                &video_encode_metadata) == -1) {
        ALOGE("Error occurred while parsing metadata.");
        return;
    }

    if (video_encode_metadata.state == 1) {
        if (is_interactive_governor(governor)) {
            /* Sched_load and migration_notif*/
            int resource_values[] = {
                INT_OP_CLUSTER0_USE_SCHED_LOAD, 0x1,
                INT_OP_CLUSTER1_USE_SCHED_LOAD, 0x1,
                INT_OP_CLUSTER0_USE_MIGRATION_NOTIF, 0x1,
                INT_OP_CLUSTER1_USE_MIGRATION_NOTIF, 0x1,
                INT_OP_CLUSTER0_TIMER_RATE, BIG_LITTLE_TR_MS_40,
                INT_OP_CLUSTER1_TIMER_RATE, BIG_LITTLE_TR_MS_40
            };
            if (!video_encode_hint_sent) {
                perform_hint_action(video_encode_metadata.hint_id,
                        resource_values, ARRAY_SIZE(resource_values));
                video_encode_hint_sent = 1;
            }
        }
    } else if (video_encode_metadata.state == 0) {
        if (is_interactive_governor(governor)) {
            undo_hint_action(video_encode_metadata.hint_id);
            video_encode_hint_sent = 0;
        }
    }
}

int power_hint_override(power_hint_t hint, void *data)
{
    int duration, duration_hint;
    static struct timespec s_previous_boost_timespec;
    struct timespec cur_boost_timespec;
    long long elapsed_time;
    int resources_interaction_fling_boost[] = {
        MIN_FREQ_BIG_CORE_0, 0x514,
        SCHED_BOOST_ON_V3, 0x1,
    };

    switch (hint) {
    	case POWER_HINT_INTERACTION:
            duration = 500;
            duration_hint = 0;

            if (data) {
                duration_hint = *((int *)data);
            }

            duration = duration_hint > 0 ? duration_hint : 500;

            clock_gettime(CLOCK_MONOTONIC, &cur_boost_timespec);
            elapsed_time = calc_timespan_us(s_previous_boost_timespec, cur_boost_timespec);
            if (elapsed_time > 750000)
                elapsed_time = 750000;
            // don't hint if it's been less than 250ms since last boost
            // also detect if we're doing anything resembling a fling
            // support additional boosting in case of flings
            else if (elapsed_time < 250000 && duration <= 750)
                return HINT_HANDLED;

            s_previous_boost_timespec = cur_boost_timespec;

            if (duration >= 1500) {
                interaction(duration, ARRAY_SIZE(resources_interaction_fling_boost),
                        resources_interaction_fling_boost);
            }
            return HINT_HANDLED;
        case POWER_HINT_VIDEO_ENCODE:
            process_video_encode_hint(data);
            return HINT_HANDLED;
        default:
            break;
    }
    return HINT_NONE;
}

int set_interactive_override(int on)
{
    char governor[80];
    char tmp_str[NODE_MAX];
    struct video_encode_metadata_t video_encode_metadata;
    int rc;

    ALOGI("Got set_interactive hint");

    if (get_scaling_governor_check_cores(governor, sizeof(governor),CPU0) == -1) {
        if (get_scaling_governor_check_cores(governor, sizeof(governor),CPU1) == -1) {
            if (get_scaling_governor_check_cores(governor, sizeof(governor),CPU2) == -1) {
                if (get_scaling_governor_check_cores(governor, sizeof(governor),CPU3) == -1) {
                    ALOGE("Can't obtain scaling governor.");
                    return HINT_NONE;
                }
            }
        }
    }

    if (!on) {
        /* Display off. */
        if (is_interactive_governor(governor)) {
            int resource_values[] = {
                INT_OP_CLUSTER0_TIMER_RATE, BIG_LITTLE_TR_MS_50,
                INT_OP_CLUSTER1_TIMER_RATE, BIG_LITTLE_TR_MS_50,
                INT_OP_NOTIFY_ON_MIGRATE, 0x00
            };

            if (!display_hint_sent) {
                perform_hint_action(
                        DISPLAY_STATE_HINT_ID,
                        resource_values,
                        ARRAY_SIZE(resource_values));
                display_hint_sent = 1;
            }
        } /* Perf time rate set for CORE0,CORE4 8952 target*/
    } else {
        /* Display on. */
        if (is_interactive_governor(governor)) {
            undo_hint_action(DISPLAY_STATE_HINT_ID);
            display_hint_sent = 0;
        }
    }
    saved_interactive_mode = !!on;
    return HINT_HANDLED;
}
