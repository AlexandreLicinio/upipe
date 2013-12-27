/*
 * Copyright (C) 2012-2013 OpenHeadend S.A.R.L.
 *
 * Authors: Benjamin Cohen
 *          Christophe Massiot
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
 * @short Upipe swscale (ffmpeg) module
 */

#include <upipe/ubase.h>
#include <upipe/uprobe.h>
#include <upipe/uref.h>
#include <upipe/ubuf.h>
#include <upipe/uref_pic_flow.h>
#include <upipe/uref_pic.h>
#include <upipe/upipe.h>
#include <upipe/uref_flow.h>
#include <upipe/uref_dump.h>
#include <upipe/upipe_helper_upipe.h>
#include <upipe/upipe_helper_urefcount.h>
#include <upipe/upipe_helper_flow.h>
#include <upipe/upipe_helper_flow_def.h>
#include <upipe/upipe_helper_ubuf_mgr.h>
#include <upipe/upipe_helper_output.h>
#include <upipe-swscale/upipe_sws.h>
#include <upipe-av/upipe_av_pixfmt.h>

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

#include <libswscale/swscale.h>

/** upipe_sws structure with swscale parameters */ 
struct upipe_sws {
    /** refcount management structure */
    struct urefcount urefcount;

    /** input flow */
    struct uref *flow_def_input;
    /** attributes added by the pipe */
    struct uref *flow_def_attr;
    /** output flow */
    struct uref *flow_def;
    /** true if the flow definition has already been sent */
    bool flow_def_sent;
    /** output pipe */
    struct upipe *output;

    /** ubuf manager */
    struct ubuf_mgr *ubuf_mgr;

    /** swscale flags */
    int flags;
    /** swscale image conversion context */
    struct SwsContext *convert_ctx;
    /** input pixel format */
    enum PixelFormat input_pix_fmt;
    /** requested output pixel format */
    enum PixelFormat output_pix_fmt;
    /** input chroma map */
    const char *input_chroma_map[UPIPE_AV_MAX_PLANES];
    /** output chroma map */
    const char *output_chroma_map[UPIPE_AV_MAX_PLANES];

    /** public upipe structure */
    struct upipe upipe;
};

UPIPE_HELPER_UPIPE(upipe_sws, upipe, UPIPE_SWS_SIGNATURE);
UPIPE_HELPER_UREFCOUNT(upipe_sws, urefcount, upipe_sws_free)
UPIPE_HELPER_FLOW(upipe_sws, "pic.");
UPIPE_HELPER_OUTPUT(upipe_sws, output, flow_def, flow_def_sent)
UPIPE_HELPER_FLOW_DEF(upipe_sws, flow_def_input, flow_def_attr)
UPIPE_HELPER_UBUF_MGR(upipe_sws, ubuf_mgr);

/** @internal @This receives incoming uref.
 *
 * @param upipe description structure of the pipe
 * @param uref uref structure describing the picture
 * @param upump pump that generated the buffer
 */
static void upipe_sws_input(struct upipe *upipe, struct uref *uref,
                            struct upump *upump)
{
    struct upipe_sws *upipe_sws = upipe_sws_from_upipe(upipe);
    int i;
    /* check ubuf manager */
    if (unlikely(!upipe_sws->ubuf_mgr)) {
        upipe_throw_need_ubuf_mgr(upipe, upipe_sws->flow_def_attr);
        if (unlikely(!upipe_sws->ubuf_mgr)) {
            upipe_err(upipe, "ubuf_mgr not set !");
            uref_free(uref);
            return;
        }
    }

    size_t input_hsize, input_vsize;
    if (!uref_pic_size(uref, &input_hsize, &input_vsize, NULL)) {
        upipe_warn(upipe, "invalid buffer received");
        uref_free(uref);
        return;
    }

    uint64_t output_hsize, output_vsize;
    if (!uref_pic_flow_get_hsize(upipe_sws->flow_def_attr, &output_hsize) ||
        !uref_pic_flow_get_vsize(upipe_sws->flow_def_attr, &output_vsize)) {
        /* comes handy in case of format conversion with no rescaling */
        output_hsize = input_hsize;
        output_vsize = input_vsize;
    }

    upipe_sws->convert_ctx = sws_getCachedContext(upipe_sws->convert_ctx,
                input_hsize, input_vsize, upipe_sws->input_pix_fmt,
                output_hsize, output_vsize, upipe_sws->output_pix_fmt,
                upipe_sws->flags, NULL, NULL, NULL);

    if (unlikely(upipe_sws->convert_ctx == NULL)) {
        upipe_err(upipe, "sws_getContext failed");
        uref_free(uref);
        return;
    }

    /* map input */
    const uint8_t *input_planes[UPIPE_AV_MAX_PLANES + 1];
    int input_strides[UPIPE_AV_MAX_PLANES + 1];
    for (i = 0; i < UPIPE_AV_MAX_PLANES &&
                upipe_sws->input_chroma_map[i] != NULL; i++) {
        const uint8_t *data;
        size_t stride;
        if (unlikely(!uref_pic_plane_read(uref,
                                          upipe_sws->input_chroma_map[i],
                                          0, 0, -1, -1, &data) ||
                     !uref_pic_plane_size(uref,
                                          upipe_sws->input_chroma_map[i],
                                          &stride, NULL, NULL, NULL))) {
            upipe_warn(upipe, "invalid buffer received");
            uref_free(uref);
            return;
        }
        input_planes[i] = data;
        input_strides[i] = stride;
    }
    input_planes[i] = NULL;
    input_strides[i] = 0;

    /* allocate dest ubuf */
    struct ubuf *ubuf = ubuf_pic_alloc(upipe_sws->ubuf_mgr,
                                       output_hsize, output_vsize);
    if (unlikely(ubuf == NULL)) {
        uref_free(uref);
        upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
        return;
    }

    /* map output */
    uint8_t *output_planes[UPIPE_AV_MAX_PLANES + 1];
    int output_strides[UPIPE_AV_MAX_PLANES + 1];
    for (i = 0; i < UPIPE_AV_MAX_PLANES &&
                upipe_sws->output_chroma_map[i] != NULL; i++) {
        uint8_t *data;
        size_t stride;
        if (unlikely(!ubuf_pic_plane_write(ubuf,
                                           upipe_sws->output_chroma_map[i],
                                           0, 0, -1, -1, &data) ||
                     !ubuf_pic_plane_size(ubuf,
                                          upipe_sws->output_chroma_map[i],
                                          &stride, NULL, NULL, NULL))) {
            upipe_warn(upipe, "invalid buffer received");
            ubuf_free(ubuf);
            uref_free(uref);
            return;
        }
        output_planes[i] = data;
        output_strides[i] = stride;
    }
    output_planes[i] = NULL;
    output_strides[i] = 0;

    /* fire ! */
    int ret = sws_scale(upipe_sws->convert_ctx,
                        input_planes, input_strides, 0, input_vsize,
                        output_planes, output_strides);

    /* unmap pictures */
    for (i = 0; i < UPIPE_AV_MAX_PLANES &&
                upipe_sws->input_chroma_map[i] != NULL; i++)
        uref_pic_plane_unmap(uref, upipe_sws->input_chroma_map[i],
                             0, 0, -1, -1);
    for (i = 0; i < UPIPE_AV_MAX_PLANES &&
                upipe_sws->output_chroma_map[i] != NULL; i++)
        ubuf_pic_plane_unmap(ubuf, upipe_sws->output_chroma_map[i],
                             0, 0, -1, -1);

    /* clean and attach */
    if (unlikely(ret <= 0)) {
        upipe_warn(upipe, "error during sws conversion");
        ubuf_free(ubuf);
        uref_free(uref);
        return;
    }
    uref_attach_ubuf(uref, ubuf);

    struct urational sar;
    if (uref_pic_flow_get_sar(upipe_sws->flow_def_attr, &sar))
        uref_pic_flow_delete_sar(uref);
    else if (uref_pic_flow_get_sar(uref, &sar)) {
        sar.num *= input_hsize * output_vsize;
        sar.den *= input_vsize * output_hsize;
        urational_simplify(&sar);
        if (unlikely(!uref_pic_flow_set_sar(uref, sar)))
            upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
    }
    upipe_sws_output(upipe, uref, upump);
}

/** @internal @This sets the input flow definition.
 *
 * @param upipe description structure of the pipe
 * @param flow_def flow definition packet
 * @return false if the flow definition is not handled
 */
static bool upipe_sws_set_flow_def(struct upipe *upipe, struct uref *flow_def)
{
    if (flow_def == NULL)
        return false;

    if (unlikely(!uref_flow_match_def(flow_def, "pic.")))
        return false;

    struct upipe_sws *upipe_sws = upipe_sws_from_upipe(upipe);
    if ((upipe_sws->input_pix_fmt =
                upipe_av_pixfmt_from_flow_def(flow_def, NULL,
                            upipe_sws->input_chroma_map)) == AV_PIX_FMT_NONE ||
        !sws_isSupportedInput(upipe_sws->input_pix_fmt)) {
        upipe_err(upipe, "incompatible flow def");
        uref_dump(flow_def, upipe->uprobe);
        return false;
    }

    flow_def = uref_dup(flow_def);
    if (unlikely(flow_def == NULL)) {
        upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
        return false;
    }

    struct urational sar;
    uint64_t input_hsize, input_vsize, output_hsize, output_vsize;
    if (!uref_pic_flow_get_sar(upipe_sws->flow_def_attr, &sar) &&
        uref_pic_flow_get_sar(flow_def, &sar) &&
        uref_pic_flow_get_hsize(flow_def, &input_hsize) &&
        uref_pic_flow_get_vsize(flow_def, &input_vsize) &&
        uref_pic_flow_get_hsize(upipe_sws->flow_def_attr, &output_hsize) &&
        uref_pic_flow_get_vsize(upipe_sws->flow_def_attr, &output_vsize)) {
        sar.num *= input_hsize * output_vsize;
        sar.den *= input_vsize * output_hsize;
        urational_simplify(&sar);
        if (unlikely(!uref_pic_flow_set_sar(flow_def, sar)))
            upipe_throw_fatal(upipe, UBASE_ERR_ALLOC);
    }
    flow_def = upipe_sws_store_flow_def_input(upipe, flow_def);
    if (flow_def != NULL)
        upipe_sws_store_flow_def(upipe, flow_def);
    return true;
}

/** @internal @This gets the swscale flags.
 *
 * @param upipe description structure of the pipe
 * @param flags_p filled in with the swscale flags
 * @return false in case of error
 */
static bool _upipe_sws_get_flags(struct upipe *upipe, int *flags_p)
{
    struct upipe_sws *upipe_sws = upipe_sws_from_upipe(upipe);
    *flags_p = upipe_sws->flags;
    return true;
}

/** @internal @This sets the swscale flags.
 *
 * @param upipe description structure of the pipe
 * @param flags swscale flags
 * @return false in case of error
 */
static bool _upipe_sws_set_flags(struct upipe *upipe, int flags)
{
    struct upipe_sws *upipe_sws = upipe_sws_from_upipe(upipe);
    upipe_sws->flags = flags;
    return true;
}

/** @internal @This processes control commands on a file source pipe, and
 * checks the status of the pipe afterwards.
 *
 * @param upipe description structure of the pipe
 * @param command type of command to process
 * @param args arguments of the command
 * @return false in case of error
 */
static bool upipe_sws_control(struct upipe *upipe, enum upipe_command command,
                               va_list args)
{
    switch (command) {
        /* generic commands */
        case UPIPE_GET_UBUF_MGR: {
            struct ubuf_mgr **p = va_arg(args, struct ubuf_mgr **);
            return upipe_sws_get_ubuf_mgr(upipe, p);
        }
        case UPIPE_SET_UBUF_MGR: {
            struct ubuf_mgr *ubuf_mgr = va_arg(args, struct ubuf_mgr *);
            return upipe_sws_set_ubuf_mgr(upipe, ubuf_mgr);
        }
        case UPIPE_GET_OUTPUT: {
            struct upipe **p = va_arg(args, struct upipe **);
            return upipe_sws_get_output(upipe, p);
        }
        case UPIPE_SET_OUTPUT: {
            struct upipe *output = va_arg(args, struct upipe *);
            return upipe_sws_set_output(upipe, output);
        }
        case UPIPE_GET_FLOW_DEF: {
            struct uref **p = va_arg(args, struct uref **);
            return upipe_sws_get_flow_def(upipe, p);
        }
        case UPIPE_SET_FLOW_DEF: {
            struct uref *flow = va_arg(args, struct uref *);
            return upipe_sws_set_flow_def(upipe, flow);
        }

        /* specific commands */
        case UPIPE_SWS_GET_FLAGS: {
            int signature = va_arg(args, unsigned int);
            assert(signature == UPIPE_SWS_SIGNATURE);
            int *flags_p = va_arg(args, int *);
            return _upipe_sws_get_flags(upipe, flags_p);
        }
        case UPIPE_SWS_SET_FLAGS: {
            int signature = va_arg(args, unsigned int);
            assert(signature == UPIPE_SWS_SIGNATURE);
            int flags = va_arg(args, int);
            return _upipe_sws_set_flags(upipe, flags);
        }
        default:
            return false;
    }
}

/** @internal @This allocates a swscale pipe.
 *
 * @param mgr common management structure
 * @param uprobe structure used to raise events
 * @param signature signature of the pipe allocator
 * @param args optional arguments
 * @return pointer to upipe or NULL in case of allocation error
 */
static struct upipe *upipe_sws_alloc(struct upipe_mgr *mgr,
                                     struct uprobe *uprobe,
                                     uint32_t signature, va_list args)
{
    struct uref *flow_def;
    struct upipe *upipe = upipe_sws_alloc_flow(mgr, uprobe, signature,
                                               args, &flow_def);
    if (unlikely(upipe == NULL))
        return NULL;

    struct upipe_sws *upipe_sws = upipe_sws_from_upipe(upipe);
    upipe_sws->output_pix_fmt = upipe_av_pixfmt_from_flow_def(flow_def, NULL,
                                                 upipe_sws->output_chroma_map);
    if (upipe_sws->output_pix_fmt == AV_PIX_FMT_NONE ||
        !sws_isSupportedOutput(upipe_sws->output_pix_fmt)) {
        uref_free(flow_def);
        upipe_sws_free_flow(upipe);
        return NULL;
    }

    upipe_sws_init_urefcount(upipe);
    upipe_sws_init_ubuf_mgr(upipe);
    upipe_sws_init_output(upipe);
    upipe_sws_init_flow_def(upipe);
    upipe_sws_store_flow_def_attr(upipe, flow_def);

    upipe_sws->convert_ctx = NULL;
    upipe_sws->flags = SWS_BICUBIC;

    upipe_throw_ready(upipe);
    return upipe;
}

/** @This frees a upipe.
 *
 * @param upipe description structure of the pipe
 */
static void upipe_sws_free(struct upipe *upipe)
{
    struct upipe_sws *upipe_sws = upipe_sws_from_upipe(upipe);
    if (likely(upipe_sws->convert_ctx))
        sws_freeContext(upipe_sws->convert_ctx);

    upipe_throw_dead(upipe);
    upipe_sws_clean_output(upipe);
    upipe_sws_clean_flow_def(upipe);
    upipe_sws_clean_ubuf_mgr(upipe);
    upipe_sws_clean_urefcount(upipe);
    upipe_sws_free_flow(upipe);
}

/** module manager static descriptor */
static struct upipe_mgr upipe_sws_mgr = {
    .refcount = NULL,
    .signature = UPIPE_SWS_SIGNATURE,

    .upipe_alloc = upipe_sws_alloc,
    .upipe_input = upipe_sws_input,
    .upipe_control = upipe_sws_control
};

/** @This returns the management structure for swscale pipes
 *
 * @return pointer to manager
 */
struct upipe_mgr *upipe_sws_mgr_alloc(void)
{
    return &upipe_sws_mgr;
}

