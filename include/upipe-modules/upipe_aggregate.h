/*
 * Copyright (C) 2012-2015 OpenHeadend S.A.R.L.
 *
 * Authors: Benjamin Cohen
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
 * @short Upipe module to aggregate complete packets up to specified MTU
 */

#ifndef _UPIPE_MODULES_UPIPE_AGG_H_
/** @hidden */
#define _UPIPE_MODULES_UPIPE_AGG_H_
#ifdef __cplusplus
extern "C" {
#endif

#include <upipe/upipe.h>

#define UPIPE_AGG_SIGNATURE UBASE_FOURCC('a','g','g','g')

/** @This extends upipe_command with specific commands for ts check. */
enum upipe_agg_command {
    UPIPE_AGG_SENTINEL = UPIPE_CONTROL_LOCAL,

    /** returns the configured mtu of output packets (int *) */
    UPIPE_AGG_GET_MTU,
    /** sets the configured mtu of output packets (int) */
    UPIPE_AGG_SET_MTU
};

/** @This returns the management structure for all agg pipes.
 *
 * @return pointer to manager
 */
struct upipe_mgr *upipe_agg_mgr_alloc(void);

/** @This returns the configured mtu of TS packets.
 *
 * @param upipe description structure of the pipe
 * @param mtu_p filled in with the configured mtu, in octets
 * @return false in case of error
 */
static inline bool upipe_agg_get_mtu(struct upipe *upipe, int *mtu_p)
{
    return upipe_control(upipe, UPIPE_AGG_GET_MTU, UPIPE_AGG_SIGNATURE, mtu_p);
}

/** @This sets the configured mtu of TS packets.
 * @param upipe description structure of the pipe
 * @param mtu configured mtu, in octets
 * @return false in case of error
 */
static inline bool upipe_agg_set_mtu(struct upipe *upipe, int mtu)
{
    return upipe_control(upipe, UPIPE_AGG_SET_MTU, UPIPE_AGG_SIGNATURE, mtu);
}

#ifdef __cplusplus
}
#endif
#endif