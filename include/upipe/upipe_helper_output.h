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
 * @short Upipe helper functions for output
 */

#ifndef _UPIPE_UPIPE_HELPER_OUTPUT_H_
/** @hidden */
#define _UPIPE_UPIPE_HELPER_OUTPUT_H_

#include <upipe/ubase.h>
#include <upipe/uref.h>
#include <upipe/uref_flow.h>
#include <upipe/upipe.h>

#include <stdbool.h>

/** @This declares nine functions dealing with the output of a pipe,
 * and an associated uref which is the flow definition on the output.
 *
 * You must add three members to your private upipe structure, for instance:
 * @code
 *  struct upipe *output;
 *  struct uref *flow_def;
 *  bool flow_def_sent;
 * @end code
 *
 * You must also declare @ref #UPIPE_HELPER_UPIPE prior to using this macro.
 *
 * Supposing the name of your structure is upipe_foo, it declares:
 * @list
 * @item @code
 *  void upipe_foo_init_output(struct upipe *upipe)
 * @end code
 * Typically called in your upipe_foo_alloc() function.
 *
 * @item @code
 *  void upipe_foo_flow_delete(struct upipe *upipe)
 * @end code
 * Not normally called from your functions. It sends a flow deletion packet
 * to the current output, and is used by the other declared functions.
 *
 * @item @code
 *  void upipe_foo_flow_def(struct upipe *upipe)
 * @end code
 * Not normally called from your functions. It sends a flow definition packet
 * to the current output, and is used by the other declared functions.
 *
 * @item @code
 *  void upipe_foo_output(struct upipe *upipe, struct uref *uref)
 * @end code
 * Called whenever you need to send a packet to your output. It takes care
 * of sending the flow definition if necessary.
 *
 * @item @code
 *  void upipe_foo_store_flow_def(struct upipe *upipe, struct uref *flow_def)
 * @end code
 * Called whenever you change the flow definition on this output. You cannot
 * call it directly from your UPIPE_SET_FLOW_DEF handler because you need
 * to duplicate the uref first.
 *
 * @item @code
 *  bool upipe_foo_get_flow_def(struct upipe *upipe, struct uref **p)
 * @end code
 * Typically called from your upipe_foo_control() handler, such as:
 * @code
 *  case UPIPE_GET_FLOW_DEF: {
 *      struct uref **p = va_arg(args, struct uref **);
 *      return upipe_foo_get_flow_def(upipe, p);
 *  }
 * @end code
 *
 * @item @code
 *  bool upipe_foo_get_output(struct upipe *upipe, struct upipe **p)
 * @end code
 * Typically called from your upipe_foo_control() handler, such as:
 * @code
 *  case UPIPE_GET_OUTPUT: {
 *      struct upipe **p = va_arg(args, struct upipe **);
 *      return upipe_foo_get_output(upipe, p);
 *  }
 * @end code
 *
 * @item @code
 *  bool upipe_foo_set_output(struct upipe *upipe, struct upipe *output)
 * @end code
 * Typically called from your upipe_foo_control() handler, such as:
 * @code
 *  case UPIPE_SET_OUTPUT: {
 *      struct upipe *output = va_arg(args, struct upipe *);
 *      return upipe_foo_set_output(upipe, output);
 *  }
 * @end code
 *
 * @item @code
 *  void upipe_foo_clean_output(struct upipe *upipe)
 * @end code
 * Typically called from your upipe_foo_free() function.
 * @end list
 *
 * @param STRUCTURE name of your private upipe structure 
 * @param OUTPUT name of the @tt {struct upipe *} field of
 * your private upipe structure
 * @param FLOW_DEF name of the @tt{struct uref *} field of
 * your private upipe structure
 * @param FLOW_DEF_SENT name of the @tt{bool} field of
 * your private upipe structure
 */
#define UPIPE_HELPER_OUTPUT(STRUCTURE, OUTPUT, FLOW_DEF, FLOW_DEF_SENT)     \
/** @internal @This initializes the private members for this helper.        \
 *                                                                          \
 * @param upipe description structure of the pipe                           \
 */                                                                         \
static void STRUCTURE##_init_output(struct upipe *upipe)                    \
{                                                                           \
    struct STRUCTURE *STRUCTURE = STRUCTURE##_from_upipe(upipe);            \
    STRUCTURE->OUTPUT = NULL;                                               \
    STRUCTURE->FLOW_DEF = NULL;                                             \
    STRUCTURE->FLOW_DEF_SENT = false;                                       \
}                                                                           \
/** @internal @This outputs a flow definition control packet.               \
 *                                                                          \
 * @param upipe description structure of the pipe                           \
 * @param upump pump that generated the buffer                              \
 */                                                                         \
static void STRUCTURE##_flow_def(struct upipe *upipe, struct upump *upump)  \
{                                                                           \
    struct STRUCTURE *STRUCTURE = STRUCTURE##_from_upipe(upipe);            \
    if (unlikely(STRUCTURE->FLOW_DEF == NULL))                              \
        return;                                                             \
    struct uref *uref = uref_dup(STRUCTURE->FLOW_DEF);                      \
    if (unlikely(uref == NULL)) {                                           \
        upipe_throw_aerror(upipe);                                          \
        return;                                                             \
    }                                                                       \
    upipe_input(STRUCTURE->OUTPUT, uref, upump);                            \
    STRUCTURE->FLOW_DEF_SENT = true;                                        \
}                                                                           \
/** @internal @This sends a uref to the output. Note that uref is then      \
 * owned by the callee and shouldn't be used any longer.                    \
 *                                                                          \
 * @param upipe description structure of the pipe                           \
 * @param uref uref structure to send                                       \
 * @param upump pump that generated the buffer                              \
 */                                                                         \
static void STRUCTURE##_output(struct upipe *upipe, struct uref *uref,      \
                               struct upump *upump)                         \
{                                                                           \
    struct STRUCTURE *STRUCTURE = STRUCTURE##_from_upipe(upipe);            \
    if (unlikely(STRUCTURE->OUTPUT == NULL && STRUCTURE->FLOW_DEF != NULL)) \
        upipe_throw_need_output(upipe, STRUCTURE->FLOW_DEF);                \
    if (unlikely(STRUCTURE->OUTPUT == NULL)) {                              \
        upipe_err(upipe, "no output defined");                              \
        uref_free(uref);                                                    \
        return;                                                             \
    }                                                                       \
    if (unlikely(!STRUCTURE->FLOW_DEF_SENT))                                \
        STRUCTURE##_flow_def(upipe, upump);                                 \
    if (unlikely(!STRUCTURE->FLOW_DEF_SENT)) {                              \
        upipe_err(upipe, "no flow_def defined");                            \
        uref_free(uref);                                                    \
        return;                                                             \
    }                                                                       \
                                                                            \
    upipe_input(STRUCTURE->OUTPUT, uref, upump);                            \
}                                                                           \
/** @internal @This stores the flow definition to use on the output. It     \
 * also schedules a flow definition packet next time a packet must be       \
 * output.                                                                  \
 * It cannot directly handle the set_flow_def control command because the   \
 * uref first needs to be duplicated.                                       \
 *                                                                          \
 * @param upipe description structure of the pipe                           \
 * @param flow_def control packet describing the output (we keep a pointer  \
 * to it so make sure it belongs to us)                                     \
 */                                                                         \
static void STRUCTURE##_store_flow_def(struct upipe *upipe,                 \
                                       struct uref *flow_def)               \
{                                                                           \
    struct STRUCTURE *STRUCTURE = STRUCTURE##_from_upipe(upipe);            \
    if (unlikely(STRUCTURE->FLOW_DEF != NULL)) {                            \
        uref_free(STRUCTURE->FLOW_DEF);                                     \
        STRUCTURE->FLOW_DEF_SENT = false;                                   \
    }                                                                       \
    STRUCTURE->FLOW_DEF = flow_def;                                         \
}                                                                           \
/** @internal @This handles the get_flow_def control command.               \
 *                                                                          \
 * @param upipe description structure of the pipe                           \
 * @param p filled in with the flow definition (to use on the output)       \
 * @return false in case of error                                           \
 */                                                                         \
static bool STRUCTURE##_get_flow_def(struct upipe *upipe, struct uref **p)  \
{                                                                           \
    struct STRUCTURE *STRUCTURE = STRUCTURE##_from_upipe(upipe);            \
    assert(p != NULL);                                                      \
    *p = STRUCTURE->FLOW_DEF;                                               \
    return true;                                                            \
}                                                                           \
/** @internal @This handles the get_output control command.                 \
 *                                                                          \
 * @param upipe description structure of the pipe                           \
 * @param p filled in with the output                                       \
 * @return false in case of error                                           \
 */                                                                         \
static bool STRUCTURE##_get_output(struct upipe *upipe, struct upipe **p)   \
{                                                                           \
    struct STRUCTURE *STRUCTURE = STRUCTURE##_from_upipe(upipe);            \
    assert(p != NULL);                                                      \
    *p = STRUCTURE->OUTPUT;                                                 \
    return true;                                                            \
}                                                                           \
/** @internal @This handles the set_output control command, and properly    \
 * deletes and replays flows on old and new outputs.                        \
 *                                                                          \
 * @param upipe description structure of the pipe                           \
 * @param output new output                                                 \
 * @return false in case of error                                           \
 */                                                                         \
static bool STRUCTURE##_set_output(struct upipe *upipe,                     \
                                   struct upipe *output)                    \
{                                                                           \
    struct STRUCTURE *STRUCTURE = STRUCTURE##_from_upipe(upipe);            \
    if (unlikely(STRUCTURE->OUTPUT != NULL))                                \
        upipe_release(STRUCTURE->OUTPUT);                                   \
                                                                            \
    STRUCTURE->OUTPUT = output;                                             \
    if (likely(output != NULL))                                             \
        upipe_use(STRUCTURE->OUTPUT);                                       \
    return true;                                                            \
}                                                                           \
/** @internal @This cleans up the private members for this helper.          \
 *                                                                          \
 * @param upipe description structure of the pipe                           \
 */                                                                         \
static void STRUCTURE##_clean_output(struct upipe *upipe)                   \
{                                                                           \
    struct STRUCTURE *STRUCTURE = STRUCTURE##_from_upipe(upipe);            \
    if (likely(STRUCTURE->OUTPUT != NULL))                                  \
        upipe_release(STRUCTURE->OUTPUT);                                   \
    if (likely(STRUCTURE->FLOW_DEF != NULL))                                \
        uref_free(STRUCTURE->FLOW_DEF);                                     \
}

#endif