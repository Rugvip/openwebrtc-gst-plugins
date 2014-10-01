/*
 * Copyright (c) 2014, Ericsson AB. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or other
 * materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 */

#ifndef _GST_VIDEOREPAIR_H_
#define _GST_VIDEOREPAIR_H_

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_VIDEOREPAIR          (gst_videorepair_get_type())
#define GST_VIDEOREPAIR(obj)          (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_VIDEOREPAIR, GstVideoRepair))
#define GST_VIDEOREPAIR_CLASS(klass)  (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_VIDEOREPAIR, GstVideoRepairClass))
#define GST_IS_VIDEOREPAIR(obj)       (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_VIDEOREPAIR))
#define GST_IS_VIDEOREPAIR_CLASS(obj) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_VIDEOREPAIR))

typedef struct _GstVideoRepair GstVideoRepair;
typedef struct _GstVideoRepairClass GstVideoRepairClass;

struct _GstVideoRepair
{
    GstElement base_videorepair;

    GstPad *sinkpad;
    GstPad *srcpad;

    gboolean needs_intra;
    gboolean drop_until_intra;
    guint retry_interval;
    guint drop_count;
};

struct _GstVideoRepairClass
{
    GstElementClass base_videorepair_class;
};

GType gst_videorepair_get_type(void);

G_END_DECLS

#endif
