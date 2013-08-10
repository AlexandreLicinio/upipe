/*
 * Copyright (C) 2012-2013 OpenHeadend S.A.R.L.
 *
 * Authors: Christophe Massiot
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/** @file
 * @short Upipe module facilitating trick play operations
 */

#include <upipe/ubase.h>
#include <upipe/ulist.h>
#include <upipe/uprobe.h>
#include <upipe/uclock.h>
#include <upipe/uref.h>
#include <upipe/uref_clock.h>
#include <upipe/upipe.h>
#include <upipe/upipe_helper_upipe.h>
#include <upipe/upipe_helper_void.h>
#include <upipe/upipe_helper_flow.h>
#include <upipe/upipe_helper_output.h>
#include <upipe/upipe_helper_sink.h>
#include <upipe/upipe_helper_uclock.h>
#include <upipe/upipe_helper_subpipe.h>
#include <upipe-modules/upipe_trickplay.h>

#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <assert.h>

/** @This is the minimum amount of time before presenting a flow. */
#define UPIPE_TRICKP_PTS_DELAY (UCLOCK_FREQ / 10)

/** @hidden */
static uint64_t upipe_trickp_get_systime(struct upipe *upipe, uint64_t ts);
/** @hidden */
static void upipe_trickp_check_start(struct upipe *upipe);
/** @hidden */
static bool upipe_trickp_sub_process(struct upipe *upipe, struct uref *uref,
                                     struct upump *upump);

/** @internal @This is the private context of a trickp pipe. */
struct upipe_trickp {
    /** uclock structure */
    struct uclock *uclock;

    /** origins of timestamps */
    uint64_t ts_origin;
    /** offset of systimes */
    uint64_t systime_offset;

    /** current rate */
    struct urational rate;
    /** list of subs */
    struct ulist subs;

    /** manager to create subs */
    struct upipe_mgr sub_mgr;

    /** public upipe structure */
    struct upipe upipe;
};

UPIPE_HELPER_UPIPE(upipe_trickp, upipe)
UPIPE_HELPER_VOID(upipe_trickp)
UPIPE_HELPER_UCLOCK(upipe_trickp, uclock)

/** @internal @This is the type of the flow (different behaviours). */
enum upipe_trickp_sub_type {
    UPIPE_TRICKP_PIC,
    UPIPE_TRICKP_SOUND,
    UPIPE_TRICKP_SUBPIC
};

/** @internal @This is the private context of an output of a trickp pipe. */
struct upipe_trickp_sub {
    /** structure for double-linked lists */
    struct uchain uchain;

    /** type of the flow */
    enum upipe_trickp_sub_type type;
    /** temporary uref storage */
    struct ulist urefs;
    /** list of blockers */
    struct ulist blockers;

    /** pipe acting as output */
    struct upipe *output;
    /** flow definition packet on this output */
    struct uref *flow_def;
    /** true if the flow definition has already been sent */
    bool flow_def_sent;

    /** public upipe structure */
    struct upipe upipe;
};

UPIPE_HELPER_UPIPE(upipe_trickp_sub, upipe)
UPIPE_HELPER_FLOW(upipe_trickp_sub, NULL)
UPIPE_HELPER_OUTPUT(upipe_trickp_sub, output, flow_def, flow_def_sent)
UPIPE_HELPER_SINK(upipe_trickp_sub, urefs, blockers, upipe_trickp_sub_process)

UPIPE_HELPER_SUBPIPE(upipe_trickp, upipe_trickp_sub, sub, sub_mgr, subs, uchain)

/** @internal @This allocates an output subpipe of a trickp pipe.
 *
 * @param mgr common management structure
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_trickp_sub_alloc(struct upipe_mgr *mgr,
                                            struct uprobe *uprobe,
                                            uint32_t signature,
                                            va_list args)
{
    struct uref *flow_def;
    struct upipe *upipe = upipe_trickp_sub_alloc_flow(mgr, uprobe, signature,
                                                      args, &flow_def);
    if (unlikely(upipe == NULL))
        return NULL;
    upipe_trickp_sub_init_output(upipe);
    upipe_trickp_sub_init_sink(upipe);
    upipe_trickp_sub_init_sub(upipe);
    struct upipe_trickp_sub *upipe_trickp_sub =
        upipe_trickp_sub_from_upipe(upipe);
    ulist_init(&upipe_trickp_sub->urefs);
    upipe_trickp_sub->type = UPIPE_TRICKP_SUBPIC;
    upipe_trickp_sub_store_flow_def(upipe, flow_def);

    struct upipe_trickp *upipe_trickp = upipe_trickp_from_sub_mgr(mgr);
    upipe_use(upipe_trickp_to_upipe(upipe_trickp));

    upipe_throw_ready(upipe);
    const char *def;
    if (likely(uref_flow_get_def(flow_def, &def)) &&
               ubase_ncmp(def, "pic.sub.")) {
        if (!ubase_ncmp(def, "pic."))
            upipe_trickp_sub->type = UPIPE_TRICKP_PIC;
        else
            upipe_trickp_sub->type = UPIPE_TRICKP_SOUND;
    }
    return upipe;
}

/** @internal @This processes data.
 *
 * @param upipe description structure of the pipe
 * @param uref uref structure
 * @param upump pump that generated the buffer
 * @return true if the uref was processed
 */
static bool upipe_trickp_sub_process(struct upipe *upipe, struct uref *uref,
                                     struct upump *upump)
{
    struct upipe_trickp *upipe_trickp = upipe_trickp_from_sub_mgr(upipe->mgr);
    if (upipe_trickp->rate.num == 0 || upipe_trickp->rate.den == 0) {
        /* pause */
        return false;
    }

    uref_clock_set_rate(uref, upipe_trickp->rate);
    uint64_t pts;
    if (likely(uref_clock_get_pts(uref, &pts)))
        uref_clock_set_pts_sys(uref,
                upipe_trickp_get_systime(upipe_trickp_to_upipe(upipe_trickp),
                                         pts));
    uint64_t dts;
    if (likely(uref_clock_get_dts(uref, &dts)))
        uref_clock_set_dts_sys(uref,
                upipe_trickp_get_systime(upipe_trickp_to_upipe(upipe_trickp),
                                         dts));

    upipe_trickp_sub_output(upipe, uref, upump);
    return true;
}

/** @internal @This receives data.
 *
 * @param upipe description structure of the pipe
 * @param uref uref structure
 * @param upump pump that generated the buffer
 */
static void upipe_trickp_sub_input(struct upipe *upipe, struct uref *uref,
                                   struct upump *upump)
{
    struct upipe_trickp *upipe_trickp = upipe_trickp_from_sub_mgr(upipe->mgr);
    if (unlikely(upipe_trickp->uclock == NULL)) {
        upipe_throw_need_uclock(upipe);
        if (unlikely(upipe_trickp->uclock == NULL)) {
            uref_free(uref);
            return;
        }
    }

    if (upipe_trickp->rate.num == 0 || upipe_trickp->rate.den == 0) {
        /* pause */
        upipe_trickp_sub_hold_sink(upipe, uref);
        upipe_trickp_sub_block_sink(upipe, upump);
    } else if (upipe_trickp->systime_offset == 0) {
        upipe_trickp_sub_hold_sink(upipe, uref);
        upipe_trickp_check_start(upipe_trickp_to_upipe(upipe_trickp));
    } else if (!upipe_trickp_sub_check_sink(upipe) ||
               !upipe_trickp_sub_process(upipe, uref, upump)) {
        upipe_trickp_sub_hold_sink(upipe, uref);
        upipe_trickp_sub_block_sink(upipe, upump);
    }
}

/** @internal @This processes control commands on an output subpipe of a
 * trickp pipe.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return false in case of error
 */
static bool upipe_trickp_sub_control(struct upipe *upipe,
                                     enum upipe_command command,
                                     va_list args)
{
    switch (command) {
        case UPIPE_GET_FLOW_DEF: {
            struct uref **p = va_arg(args, struct uref **);
            return upipe_trickp_sub_get_flow_def(upipe, p);
        }
        case UPIPE_GET_OUTPUT: {
            struct upipe **p = va_arg(args, struct upipe **);
            return upipe_trickp_sub_get_output(upipe, p);
        }
        case UPIPE_SET_OUTPUT: {
            struct upipe *output = va_arg(args, struct upipe *);
            return upipe_trickp_sub_set_output(upipe, output);
        }

        default:
            return false;
    }
}

/** @This frees a upipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_trickp_sub_free(struct upipe *upipe)
{
    struct upipe_trickp *upipe_trickp =
        upipe_trickp_from_sub_mgr(upipe->mgr);
    upipe_throw_dead(upipe);

    upipe_trickp_sub_clean_output(upipe);
    upipe_trickp_sub_clean_sink(upipe);
    upipe_trickp_sub_clean_sub(upipe);
    upipe_trickp_sub_free_flow(upipe);

    upipe_release(upipe_trickp_to_upipe(upipe_trickp));
}

/** @internal @This initializes the output manager for a trickp pipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_trickp_init_sub_mgr(struct upipe *upipe)
{
    struct upipe_trickp *upipe_trickp =
        upipe_trickp_from_upipe(upipe);
    struct upipe_mgr *sub_mgr = &upipe_trickp->sub_mgr;
    sub_mgr->signature = UPIPE_TRICKP_SUB_SIGNATURE;
    sub_mgr->upipe_alloc = upipe_trickp_sub_alloc;
    sub_mgr->upipe_input = upipe_trickp_sub_input;
    sub_mgr->upipe_control = upipe_trickp_sub_control;
    sub_mgr->upipe_free = upipe_trickp_sub_free;
    sub_mgr->upipe_mgr_free = NULL;
}

/** @internal @This allocates a trickp pipe.
 *
 * @param mgr common management structure
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_trickp_alloc(struct upipe_mgr *mgr,
                                        struct uprobe *uprobe,
                                        uint32_t signature, va_list args)
{
    struct upipe *upipe = upipe_trickp_alloc_void(mgr, uprobe, signature, args);
    if (unlikely(upipe == NULL))
        return NULL;
    upipe_trickp_init_sub_mgr(upipe);
    upipe_trickp_init_sub_subs(upipe);
    upipe_trickp_init_uclock(upipe);
    struct upipe_trickp *upipe_trickp = upipe_trickp_from_upipe(upipe);
    upipe_trickp->systime_offset = 0;
    upipe_trickp->ts_origin = 0;
    upipe_trickp->rate.num = upipe_trickp->rate.den = 1;
    upipe_throw_ready(upipe);
    return upipe;
}

/** @internal @This checks if we have got packets on video and audio inputs, so
 * we are ready to output them.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_trickp_check_start(struct upipe *upipe)
{
    struct upipe_trickp *upipe_trickp = upipe_trickp_from_upipe(upipe);
    uint64_t earliest_ts = UINT64_MAX;
    struct uchain *uchain;
    ulist_foreach (&upipe_trickp->subs, uchain) {
        struct upipe_trickp_sub *upipe_trickp_sub =
            upipe_trickp_sub_from_uchain(uchain);
        if (upipe_trickp_sub->type == UPIPE_TRICKP_SUBPIC)
            continue;

        for ( ; ; ) {
            struct uchain *uchain2;
            uchain2 = ulist_peek(&upipe_trickp_sub->urefs);
            if (uchain2 == NULL)
                return; /* not ready */
            struct uref *uref = uref_from_uchain(uchain2);
            uint64_t ts;
            if (!uref_clock_get_dts(uref, &ts) &&
                !uref_clock_get_pts(uref, &ts)) {
                upipe_warn(upipe, "non-dated uref");
                ulist_pop(&upipe_trickp_sub->urefs);
                uref_free(uref);
                continue;
            }
            if (ts < earliest_ts)
                earliest_ts = ts;
            break;
        }
    }

    upipe_trickp->ts_origin = earliest_ts;
    upipe_trickp->systime_offset = uclock_now(upipe_trickp->uclock) +
                                   UPIPE_TRICKP_PTS_DELAY;

    ulist_foreach (&upipe_trickp->subs, uchain) {
        struct upipe_trickp_sub *upipe_trickp_sub =
            upipe_trickp_sub_from_uchain(uchain);
        if (upipe_trickp_sub_output_sink(
                    upipe_trickp_sub_to_upipe(upipe_trickp_sub)))
            upipe_trickp_sub_unblock_sink(
                    upipe_trickp_sub_to_upipe(upipe_trickp_sub));
    }
}

/** @internal @This returns a systime converted from a timestamp.
 *
 * @param upipe description structure of the pipe
 * @param ts timestamp
 * @return systime
 */
static uint64_t upipe_trickp_get_systime(struct upipe *upipe, uint64_t ts)
{
    struct upipe_trickp *upipe_trickp = upipe_trickp_from_upipe(upipe);
    if (unlikely(ts < upipe_trickp->ts_origin)) {
        upipe_warn(upipe, "got a timestamp in the past");
        ts = upipe_trickp->ts_origin;
    }
    return (ts - upipe_trickp->ts_origin) *
               upipe_trickp->rate.den / upipe_trickp->rate.num +
           upipe_trickp->systime_offset;
}

/** @internal @This resets uclock-related fields.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_trickp_reset_uclock(struct upipe *upipe)
{
    struct upipe_trickp *upipe_trickp = upipe_trickp_from_upipe(upipe);
    upipe_trickp->systime_offset = 0;
    upipe_trickp->ts_origin = 0;
}

/** @This returns the current playing rate.
 *
 * @param upipe description structure of the pipe
 * @param rate_p filled with the current rate
 * @return false in case of error
 */
static inline bool _upipe_trickp_get_rate(struct upipe *upipe,
                                          struct urational *rate_p)
{
    struct upipe_trickp *upipe_trickp = upipe_trickp_from_upipe(upipe);
    *rate_p = upipe_trickp->rate;
    return true;
}

/** @This sets the playing rate.
 *
 * @param upipe description structure of the pipe
 * @param rate new rate (1/1 = normal play, 0 = pause)
 * @return false in case of error
 */
static inline bool _upipe_trickp_set_rate(struct upipe *upipe,
                                          struct urational rate)
{
    struct upipe_trickp *upipe_trickp = upipe_trickp_from_upipe(upipe);
    upipe_trickp->rate = rate;
    upipe_trickp_reset_uclock(upipe);
    upipe_trickp_check_start(upipe);
    return false;
}

/** @internal @This processes control commands on a trickp pipe.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return false in case of error
 */
static bool upipe_trickp_control(struct upipe *upipe,
                                 enum upipe_command command, va_list args)
{
    switch (command) {
        case UPIPE_GET_UCLOCK: {
            struct uclock **p = va_arg(args, struct uclock **);
            return upipe_trickp_get_uclock(upipe, p);
        }
        case UPIPE_SET_UCLOCK: {
            struct uclock *uclock = va_arg(args, struct uclock *);
            upipe_trickp_reset_uclock(upipe);
            return upipe_trickp_set_uclock(upipe, uclock);
        }

        case UPIPE_TRICKP_GET_RATE: {
            unsigned int signature = va_arg(args, unsigned int);
            assert(signature == UPIPE_TRICKP_SIGNATURE);
            struct urational *p = va_arg(args, struct urational *);
            return _upipe_trickp_get_rate(upipe, p);
        }
        case UPIPE_TRICKP_SET_RATE: {
            unsigned int signature = va_arg(args, unsigned int);
            assert(signature == UPIPE_TRICKP_SIGNATURE);
            struct urational rate = va_arg(args, struct urational);
            return _upipe_trickp_set_rate(upipe, rate);
        }

        default:
            return false;
    }
}

/** @This frees a upipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_trickp_free(struct upipe *upipe)
{
    upipe_throw_dead(upipe);
    upipe_trickp_clean_sub_subs(upipe);
    upipe_trickp_clean_uclock(upipe);
    upipe_trickp_free_void(upipe);
}

/** module manager static descriptor */
static struct upipe_mgr upipe_trickp_mgr = {
    .signature = UPIPE_TRICKP_SIGNATURE,

    .upipe_alloc = upipe_trickp_alloc,
    .upipe_input = NULL,
    .upipe_control = upipe_trickp_control,
    .upipe_free = upipe_trickp_free,

    .upipe_mgr_free = NULL
};

/** @This returns the management structure for all trickp pipes.
 *
 * @return pointer to manager
 */
struct upipe_mgr *upipe_trickp_mgr_alloc(void)
{
    return &upipe_trickp_mgr;
}