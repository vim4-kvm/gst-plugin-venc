/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
GType gst_vim4_v4l2_src_get_type(void);
GType gst_vim4_wave5_enc_get_type(void);

static gboolean plugin_init(GstPlugin *plugin)
{
    return gst_element_register(plugin, "vim4v4l2src", GST_RANK_NONE, gst_vim4_v4l2_src_get_type()) &&
        gst_element_register(plugin, "vim4wave5enc", GST_RANK_NONE, gst_vim4_wave5_enc_get_type());
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, vim4wave5,
    "VIM4 standard V4L2 capture and Wave5 encoding", plugin_init, "0.1.0", "LGPL",
    "vim4-kvm", "https://github.com/vim4-kvm")
