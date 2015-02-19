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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gsterdtlssrtpdec.h"

#include "erdtlsconnection.h"

static GstStaticPadTemplate sink_template =
    GST_STATIC_PAD_TEMPLATE("sink",
        GST_PAD_SINK,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS_ANY
    );

static GstStaticPadTemplate rtp_src_template =
    GST_STATIC_PAD_TEMPLATE("rtp_src",
        GST_PAD_SRC,
        GST_PAD_ALWAYS,
        GST_STATIC_CAPS("application/x-rtp;application/x-rtcp")
    );

static GstStaticPadTemplate data_src_template =
    GST_STATIC_PAD_TEMPLATE("data_src",
        GST_PAD_SRC,
        GST_PAD_REQUEST,
        GST_STATIC_CAPS_ANY
    );

GST_DEBUG_CATEGORY_STATIC(er_dtls_srtp_dec_debug);
#define GST_CAT_DEFAULT er_dtls_srtp_dec_debug

#define gst_er_dtls_srtp_dec_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE(GstErDtlsSrtpDec, gst_er_dtls_srtp_dec, GST_TYPE_ER_DTLS_SRTP_BIN,
    GST_DEBUG_CATEGORY_INIT(er_dtls_srtp_dec_debug, "erdtlssrtpdec", 0, "Ericsson DTLS Decoder"));

#define UNUSED(param) while (0) { (void)(param); }

enum {
    PROP_0,
    PROP_PEM,
    PROP_PEER_PEM,
    NUM_PROPERTIES
};

static GParamSpec *properties[NUM_PROPERTIES];

#define DEFAULT_PEM NULL
#define DEFAULT_PEER_PEM NULL

static void gst_er_dtls_srtp_dec_set_property(GObject *, guint prop_id, const GValue *, GParamSpec *);
static void gst_er_dtls_srtp_dec_get_property(GObject *, guint prop_id, GValue *, GParamSpec *);

static GstPad *gst_er_dtls_srtp_dec_request_new_pad(GstElement *, GstPadTemplate *, const gchar *name, const GstCaps *);
static GstCaps *on_decoder_request_key(GstElement *srtp_decoder, guint ssrc, GstErDtlsSrtpBin *);
static void on_peer_pem(GstElement *srtp_decoder, GParamSpec *pspec, GstErDtlsSrtpDec *self);

static void gst_er_dtls_srtp_dec_remove_dtls_element(GstErDtlsSrtpBin *);
static GstPadProbeReturn remove_dtls_decoder_probe_callback(GstPad *, GstPadProbeInfo *, GstElement *);

static GstPadProbeReturn drop_funnel_rtcp_caps(GstPad *, GstPadProbeInfo *, gpointer);

static void gst_er_dtls_srtp_dec_class_init(GstErDtlsSrtpDecClass *klass)
{
    GObjectClass *gobject_class;
    GstElementClass *element_class;
    GstErDtlsSrtpBinClass *dtls_srtp_bin_class;

    gobject_class = (GObjectClass *) klass;
    element_class = (GstElementClass *) klass;
    dtls_srtp_bin_class = (GstErDtlsSrtpBinClass *) klass;

    gobject_class->set_property = GST_DEBUG_FUNCPTR(gst_er_dtls_srtp_dec_set_property);
    gobject_class->get_property = GST_DEBUG_FUNCPTR(gst_er_dtls_srtp_dec_get_property);

    element_class->request_new_pad = GST_DEBUG_FUNCPTR(gst_er_dtls_srtp_dec_request_new_pad);

    dtls_srtp_bin_class->remove_dtls_element = GST_DEBUG_FUNCPTR(gst_er_dtls_srtp_dec_remove_dtls_element);

    properties[PROP_PEM] =
        g_param_spec_string("pem",
            "PEM string",
            "A string containing a X509 certificate and RSA private key in PEM format",
            DEFAULT_PEM,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_PEER_PEM] =
        g_param_spec_string("peer-pem",
            "Peer PEM string",
            "The X509 certificate received in the DTLS handshake, in PEM format",
            DEFAULT_PEER_PEM,
            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(gobject_class, NUM_PROPERTIES, properties);

    gst_element_class_add_pad_template(element_class,
        gst_static_pad_template_get(&sink_template));
    gst_element_class_add_pad_template(element_class,
        gst_static_pad_template_get(&rtp_src_template));
    gst_element_class_add_pad_template(element_class,
        gst_static_pad_template_get(&data_src_template));

    gst_element_class_set_static_metadata(element_class,
        "DTLS-SRTP Decoder",
        "Decoder/Network/DTLS/SRTP",
        "Decodes SRTP packets with a key received from DTLS",
        "Patrik Oldsberg patrik.oldsberg@ericsson.com");
}

static void gst_er_dtls_srtp_dec_init(GstErDtlsSrtpDec *self)
{
    GstElementClass *klass = GST_ELEMENT_GET_CLASS(GST_ELEMENT(self));
    GstPadTemplate *templ;
    GstPad *target_pad, *ghost_pad, *pad;
    gboolean ret;

/*
                                 +--------------------+
            +--------------+  .-o|      erdtlsdec     |o-R----------data
            |          dtls|o-'  +--------------------+
    sink---o|  dtlsdemux   |
            |       srt(c)p|o-.  +-----------+     +-----------+
            +--------------+  '-o|srtp    rtp|o---o|rtp        |
                                 |  srtpdec  |     |   funnel  |o---rt(c)p
                                o|srtcp  rtcp|o---o|rtcp       |
                                 +-----------+     +-----------+
*/

    self->srtp_dec = gst_element_factory_make("srtpdec", "srtp-decoder");
    if (!self->srtp_dec) {
        GST_ERROR_OBJECT(self, "failed to create srtp_dec, is the srtp plugin registered?");
        return;
    }
    self->dtls_srtp_demux = gst_element_factory_make("erdtlssrtpdemux", "dtls-srtp-demux");
    if (!self->dtls_srtp_demux) {
        GST_ERROR_OBJECT(self, "failed to create dtls_srtp_demux");
        return;
    }
    self->bin.dtls_element = gst_element_factory_make("erdtlsdec", "dtls-decoder");
    if (!self->bin.dtls_element) {
        GST_ERROR_OBJECT(self, "failed to create dtls_dec");
        return;
    }
    self->funnel = gst_element_factory_make("funnel", "funnel");
    if (!self->funnel) {
        GST_ERROR_OBJECT(self, "failed to create funnel");
        return;
    }

    gst_bin_add_many(GST_BIN(self),
        self->dtls_srtp_demux,
        self->bin.dtls_element,
        self->srtp_dec,
        self->funnel,
        NULL);

    ret = gst_element_link_pads(self->dtls_srtp_demux, "dtls_src", self->bin.dtls_element, NULL);
    g_return_if_fail(ret);
    ret = gst_element_link_pads(self->dtls_srtp_demux, "rtp_src", self->srtp_dec, "rtp_sink");
    g_return_if_fail(ret);
    ret = gst_element_link_pads(self->srtp_dec, "rtp_src", self->funnel, "sink_0");
    g_return_if_fail(ret);
    ret = gst_element_link_pads(self->srtp_dec, "rtcp_src", self->funnel, "sink_1");
    g_return_if_fail(ret);

    pad = gst_element_get_static_pad(self->funnel, "sink_1");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, drop_funnel_rtcp_caps, NULL, NULL);
    gst_object_unref(pad);

    templ = gst_element_class_get_pad_template(klass, "rtp_src");
    target_pad = gst_element_get_static_pad(self->funnel, "src");
    ghost_pad = gst_ghost_pad_new_from_template("rtp_src", target_pad, templ);
    gst_object_unref(target_pad);
    g_return_if_fail(ghost_pad);

    ret = gst_element_add_pad(GST_ELEMENT(self), ghost_pad);
    g_return_if_fail(ret);

    templ = gst_element_class_get_pad_template(klass, "sink");
    target_pad = gst_element_get_static_pad(self->dtls_srtp_demux, "sink");
    ghost_pad = gst_ghost_pad_new_from_template("sink", target_pad, templ);
    gst_object_unref(target_pad);
    g_return_if_fail(ghost_pad);

    ret = gst_element_add_pad(GST_ELEMENT(self), ghost_pad);
    g_return_if_fail(ret);

    g_signal_connect(self->srtp_dec, "request-key", G_CALLBACK(on_decoder_request_key), self);
    g_signal_connect(self->bin.dtls_element, "notify::peer-pem", G_CALLBACK(on_peer_pem), self);
}

static void gst_er_dtls_srtp_dec_set_property(GObject *object,
    guint prop_id, const GValue *value, GParamSpec *pspec)
{
    GstErDtlsSrtpDec *self = GST_ER_DTLS_SRTP_DEC(object);

    switch (prop_id) {
    case PROP_PEM:
        if (self->bin.dtls_element) {
            g_object_set_property(G_OBJECT(self->bin.dtls_element), "pem", value);
        } else {
            GST_WARNING_OBJECT(self, "tried to set pem after disabling DTLS");
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(self, prop_id, pspec);
    }
}

static void gst_er_dtls_srtp_dec_get_property(GObject *object,
    guint prop_id, GValue *value, GParamSpec *pspec)
{
    GstErDtlsSrtpDec *self = GST_ER_DTLS_SRTP_DEC(object);

    switch (prop_id) {
    case PROP_PEM:
        if (self->bin.dtls_element) {
            g_object_get_property(G_OBJECT(self->bin.dtls_element), "pem", value);
        } else {
            GST_WARNING_OBJECT(self, "tried to get pem after disabling DTLS");
        }
        break;
    case PROP_PEER_PEM:
        if (self->bin.dtls_element) {
            g_object_get_property(G_OBJECT(self->bin.dtls_element), "peer-pem", value);
        } else {
            GST_WARNING_OBJECT(self, "tried to get peer-pem after disabling DTLS");
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(self, prop_id, pspec);
    }
}

static GstPad *gst_er_dtls_srtp_dec_request_new_pad(GstElement *element,
    GstPadTemplate *templ, const gchar *name, const GstCaps *caps)
{
    GstErDtlsSrtpDec *self = GST_ER_DTLS_SRTP_DEC(element);
    GstElementClass *klass = GST_ELEMENT_GET_CLASS(element);
    GstPad *ghost_pad = NULL;
    gboolean ret;

    GST_DEBUG_OBJECT(element, "pad requested");

    g_return_val_if_fail(self->bin.dtls_element, NULL);
    g_return_val_if_fail(!self->bin.key_is_set, NULL);

    if (templ == gst_element_class_get_pad_template(klass, "data_src")) {
        GstPad *target_pad;

        target_pad = gst_element_get_request_pad(self->bin.dtls_element, "src");

        ghost_pad = gst_ghost_pad_new_from_template(name, target_pad, templ);
        gst_object_unref(target_pad);
        g_return_val_if_fail(ghost_pad, NULL);

        ret = gst_pad_set_active(ghost_pad, TRUE);
        g_return_val_if_fail(ret, NULL);
        ret = gst_element_add_pad(element, ghost_pad);
        g_return_val_if_fail(ret, NULL);

        GST_LOG_OBJECT(self, "added data src pad");

        if (caps) {
            g_object_set(ghost_pad, "caps", caps, NULL);
        }

        return ghost_pad;
    }

    g_return_val_if_reached(NULL);
}

static GstCaps *on_decoder_request_key(GstElement *srtp_decoder,
    guint ssrc, GstErDtlsSrtpBin *bin)
{
    GstCaps *key_caps;
    GstBuffer *key_buffer = NULL;
    guint cipher;
    guint auth;

    if (bin->key_is_set) {
        if (bin->key) {
            if (bin->srtp_cipher && bin->srtp_auth && bin->srtcp_cipher && bin->srtcp_auth) {
                GST_DEBUG_OBJECT(bin, "setting srtp key");
                return gst_caps_new_simple("application/x-srtp",
                    "srtp-key", GST_TYPE_BUFFER, gst_buffer_copy(bin->key),
                    "srtp-auth", G_TYPE_STRING, bin->srtp_auth,
                    "srtcp-auth", G_TYPE_STRING, bin->srtcp_auth,
                    "srtp-cipher", G_TYPE_STRING, bin->srtp_cipher,
                    "srtcp-cipher", G_TYPE_STRING, bin->srtcp_cipher,
                    NULL);
            } else {
                GST_WARNING_OBJECT(bin, "srtp key is set but not all ciphers and auths");
                return NULL;
            }
        }

        GST_DEBUG_OBJECT(bin, "setting srtp key to null");
        return gst_caps_new_simple("application/x-srtp",
            "srtp-key", GST_TYPE_BUFFER, NULL,
            "srtp-auth", G_TYPE_STRING, "null",
            "srtcp-auth", G_TYPE_STRING, "null",
            "srtp-cipher", G_TYPE_STRING, "null",
            "srtcp-cipher", G_TYPE_STRING, "null",
            NULL);
    }

    if (bin->dtls_element) {
        g_object_get(bin->dtls_element,
            "decoder-key", &key_buffer,
            NULL);
    }

    if (key_buffer) {
        g_object_get(bin->dtls_element,
            "srtp-cipher", &cipher,
            "srtp-auth", &auth,
            NULL);

        g_return_val_if_fail(cipher == ER_DTLS_SRTP_CIPHER_AES_128_ICM, NULL);

        key_caps = gst_caps_new_simple("application/x-srtp",
            "srtp-key", GST_TYPE_BUFFER, key_buffer,
            "srtp-cipher", G_TYPE_STRING, "aes-128-icm",
            "srtcp-cipher", G_TYPE_STRING, "aes-128-icm",
            NULL);

        switch (auth) {
        case ER_DTLS_SRTP_AUTH_HMAC_SHA1_32:
            gst_caps_set_simple(key_caps,
                "srtp-auth", G_TYPE_STRING, "hmac-sha1-32",
                "srtcp-auth", G_TYPE_STRING, "hmac-sha1-32",
                NULL);
            break;
        case ER_DTLS_SRTP_AUTH_HMAC_SHA1_80:
            gst_caps_set_simple(key_caps,
                "srtp-auth", G_TYPE_STRING, "hmac-sha1-80",
                "srtcp-auth", G_TYPE_STRING, "hmac-sha1-80",
                NULL);
            break;
        default:
            g_return_val_if_reached(NULL);
            break;
        }

        return key_caps;
    }

    return NULL;
}

static void on_peer_pem(GstElement *srtp_decoder, GParamSpec *pspec, GstErDtlsSrtpDec *self)
{
    UNUSED(srtp_decoder);
    UNUSED(pspec);
    g_return_if_fail(self);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_PEER_PEM]);
}

static void gst_er_dtls_srtp_dec_remove_dtls_element(GstErDtlsSrtpBin *bin)
{
    GstErDtlsSrtpDec *self = GST_ER_DTLS_SRTP_DEC(bin);
    GstPad *demux_pad;
    gulong id;

    if (!bin->dtls_element) {
        return;
    }

    demux_pad = gst_element_get_static_pad(self->dtls_srtp_demux, "dtls_src");

    id = gst_pad_add_probe(demux_pad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
        (GstPadProbeCallback) remove_dtls_decoder_probe_callback, bin->dtls_element, NULL);
    g_return_if_fail(id);
    bin->dtls_element = NULL;

    gst_pad_push_event(demux_pad, gst_event_new_custom(GST_EVENT_CUSTOM_DOWNSTREAM, gst_structure_new_empty("dummy")));

    gst_object_unref(demux_pad);
}

static GstPadProbeReturn remove_dtls_decoder_probe_callback(GstPad *pad,
    GstPadProbeInfo *info, GstElement *element)
{
    gst_pad_remove_probe(pad, GST_PAD_PROBE_INFO_ID(info));

    gst_element_set_state(GST_ELEMENT(element), GST_STATE_NULL);
    gst_bin_remove(GST_BIN(GST_ELEMENT_PARENT(element)), element);

    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn drop_funnel_rtcp_caps(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    /* FIXME: This is needed for setting the proper caps until
     * GStreamer supports MIXED caps or another mechanism to
     * prevent renegotiation all the time when two different caps
     * are going over the same pad
     */
    if (GST_EVENT_TYPE (info->data) == GST_EVENT_CAPS) {
      GstCaps *caps, *peercaps;
      GstStructure *s;

      gst_event_parse_caps (GST_EVENT (info->data), &caps);
      s = gst_caps_get_structure (caps, 0);
      if (gst_structure_has_name (s, "application/x-rtcp")) {
        peercaps = gst_pad_query_caps (pad, NULL);

        /* If the peer does not accept RTCP, we are linked to
         * the RTP sinkpad of rtpbin. In that case we have to
         * drop the RTCP caps and assume that we sent RTP caps
         * before here, which is very likely but not guaranteed
         * if for some reason we receive RTCP before any RTP.
         * In that unlikely case we will get event misordering
         * warnings later, instead of getting them always as
         * happens now.
         */
        if (peercaps && !gst_caps_is_subset (caps, peercaps)) {
          gst_caps_unref (peercaps);
          return GST_PAD_PROBE_DROP;
        }
        gst_caps_replace (&peercaps, NULL);
      }
    }

    return GST_PAD_PROBE_OK;
}
