/*
 * (C) Copyright 2014 Kurento (http://kurento.org/)
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "kmsalphablending.h"
#include "kmsgenericstructure.h"
#include <commons/kmsagnosticcaps.h>
#include <commons/kms-core-marshal.h>
#include <commons/kmshubport.h>
#include <commons/kmsloop.h>

#define PLUGIN_NAME "alphablending"

#define KMS_ALPHA_BLENDING_LOCK(mixer) \
  (g_rec_mutex_lock (&( (KmsAlphaBlending *) mixer)->priv->mutex))

#define KMS_ALPHA_BLENDING_UNLOCK(mixer) \
  (g_rec_mutex_unlock (&( (KmsAlphaBlending *) mixer)->priv->mutex))

GST_DEBUG_CATEGORY_STATIC (kms_alpha_blending_debug_category);
#define GST_CAT_DEFAULT kms_alpha_blending_debug_category

#define KMS_ALPHA_BLENDING_GET_PRIVATE(obj) (\
  G_TYPE_INSTANCE_GET_PRIVATE (               \
    (obj),                                    \
    KMS_TYPE_ALPHA_BLENDING,                 \
    KmsAlphaBlendingPrivate                  \
  )                                           \
)

#define AUDIO_SINK_PAD_PREFIX_COMP "audio_sink_"
#define VIDEO_SINK_PAD_PREFIX_COMP "video_sink_"
#define AUDIO_SRC_PAD_PREFIX_COMP "audio_src_"
#define VIDEO_SRC_PAD_PREFIX_COMP "video_src_"
#define AUDIO_SINK_PAD_NAME_COMP AUDIO_SINK_PAD_PREFIX_COMP "%u"
#define VIDEO_SINK_PAD_NAME_COMP VIDEO_SINK_PAD_PREFIX_COMP "%u"
#define AUDIO_SRC_PAD_NAME_COMP AUDIO_SRC_PAD_PREFIX_COMP "%u"
#define VIDEO_SRC_PAD_NAME_COMP VIDEO_SRC_PAD_PREFIX_COMP "%u"

#define AUDIO_FAKESINK "audio_fakesink_%u"

#define AUDIO FALSE

#define MIXER "mixer"
#define ID "id"
#define INPUT "input"
#define CONFIGURATED "configurated"
#define REMOVING "removing"
#define EOS_MANAGED "eos_managed"
#define VIDEOCONVERT "videoconvert"
#define VIDEO_MIXER_PAD "video_mixer_pad"
#define CAPSFILTER "capsfilter"
#define VIDEOSCALE "videoscale"
#define QUEUE "queue"
#define VIDEORATE "videorate"
#define VIDEOCONVERT_SINK_PAD "videoconvert_sink_pad"
#define PROBE_ID "probe_id"
#define LINK_PROBE_ID "link_probe_id"

static GstStaticPadTemplate audio_sink_factory =
GST_STATIC_PAD_TEMPLATE (AUDIO_SINK_PAD_NAME_COMP,
    GST_PAD_SINK,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS (KMS_AGNOSTIC_RAW_AUDIO_CAPS)
    );

static GstStaticPadTemplate video_sink_factory =
GST_STATIC_PAD_TEMPLATE (VIDEO_SINK_PAD_NAME_COMP,
    GST_PAD_SINK,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS (KMS_AGNOSTIC_RAW_VIDEO_CAPS)
    );

static GstStaticPadTemplate audio_src_factory =
GST_STATIC_PAD_TEMPLATE (AUDIO_SRC_PAD_NAME_COMP,
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS (KMS_AGNOSTIC_RAW_AUDIO_CAPS)
    );

static GstStaticPadTemplate video_src_factory =
GST_STATIC_PAD_TEMPLATE (VIDEO_SRC_PAD_NAME_COMP,
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS (KMS_AGNOSTIC_RAW_VIDEO_CAPS)
    );

enum
{
  PROP_0,
  PROP_SET_MASTER,
  N_PROPERTIES
};

enum
{
  SIGNAL_SET_PORT_PROPERTIES,
  LAST_SIGNAL
};

static guint kms_alpha_blending_signals[LAST_SIGNAL] = { 0 };

struct _KmsAlphaBlendingPrivate
{
  GstElement *videomixer;
  GstElement *audiomixer;
  GstElement *videotestsrc;
  GstElement *videotestsrc_capsfilter;
  GHashTable *ports;
  GstElement *mixer_audio_agnostic;
  GstElement *mixer_video_agnostic;
  KmsLoop *loop;
  GRecMutex mutex;
  gint n_elems;
  gint output_width, output_height;
  int master_port;
  int z_master;
};

/* class initialization */

G_DEFINE_TYPE_WITH_CODE (KmsAlphaBlending, kms_alpha_blending,
    KMS_TYPE_BASE_HUB,
    GST_DEBUG_CATEGORY_INIT (kms_alpha_blending_debug_category, PLUGIN_NAME,
        0, "debug category for alphablending element"));

static gint
compare_port_data (gconstpointer a, gconstpointer b)
{
  KmsGenericStructure *port_data_a = KMS_GENERIC_STRUCTURE (a);
  KmsGenericStructure *port_data_b = KMS_GENERIC_STRUCTURE (b);
  gpointer id1, id2;

  id1 = kms_generic_structure_get (port_data_a, ID);
  id2 = kms_generic_structure_get (port_data_b, ID);

  if (id1 == NULL || id2 == NULL) {
    return -1;
  }

  return GPOINTER_TO_INT (id1) - GPOINTER_TO_INT (id2);
}

static void
release_gint (gpointer data)
{
  g_slice_free (gint, data);
}

static gint *
create_gint (gint value)
{
  gint *p = g_slice_new (gint);

  *p = value;
  return p;
}

static void
configure_port (KmsGenericStructure * port_data)
{
  KmsAlphaBlending *mixer;
  GstPad *video_mixer_pad;
  GstElement *capsfilter;
  GstCaps *filtercaps;
  gboolean configurated;

  mixer = KMS_ALPHA_BLENDING (kms_generic_structure_get (port_data, MIXER));
  configurated =
      GPOINTER_TO_INT (kms_generic_structure_get (port_data, CONFIGURATED));
  video_mixer_pad = kms_generic_structure_get (port_data, VIDEO_MIXER_PAD);

  if (configurated) {
    gint _relative_x, _relative_y, _relative_width, _relative_height;

    _relative_x =
        GPOINTER_TO_INT (kms_generic_structure_get (port_data,
            "relative_x")) * mixer->priv->output_width;
    _relative_y =
        GPOINTER_TO_INT (kms_generic_structure_get (port_data,
            "relative_y")) * mixer->priv->output_height;
    _relative_width =
        GPOINTER_TO_INT (kms_generic_structure_get (port_data,
            "relative_width")) * mixer->priv->output_width;
    _relative_height =
        GPOINTER_TO_INT (kms_generic_structure_get (port_data,
            "relative_height")) * mixer->priv->output_height;

    filtercaps =
        gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING, "AYUV",
        "width", G_TYPE_INT, _relative_width, "height",
        G_TYPE_INT, _relative_height, "framerate", GST_TYPE_FRACTION, 15, 1,
        NULL);

    if (video_mixer_pad != NULL) {
      g_object_set (video_mixer_pad, "xpos", _relative_x, "ypos", _relative_y,
          "zorder", GPOINTER_TO_INT (kms_generic_structure_get (port_data,
                  "z_order")), "alpha", 1.0, NULL);
    }
  } else {
    filtercaps =
        gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING, "AYUV",
        "width", G_TYPE_INT, mixer->priv->output_width, "height",
        G_TYPE_INT, mixer->priv->output_height,
        "framerate", GST_TYPE_FRACTION, 15, 1, NULL);
    if (video_mixer_pad != NULL) {
      g_object_set (video_mixer_pad, "xpos", 0, "ypos", 0, "alpha",
          1.0, "zorder", 1, NULL);
    }
  }

  capsfilter = kms_generic_structure_get (port_data, CAPSFILTER);
  if (capsfilter != NULL) {
    g_object_set (G_OBJECT (capsfilter), "caps", filtercaps, NULL);
  }
  gst_caps_unref (filtercaps);
}

static void
kms_alpha_blending_reconfigure_ports (gpointer data)
{
  KmsAlphaBlending *self = KMS_ALPHA_BLENDING (data);
  GstCaps *filtercaps;
  GList *l;
  GList *values = g_hash_table_get_values (self->priv->ports);

  values = g_list_sort (values, compare_port_data);

  for (l = values; l != NULL; l = l->next) {
    KmsGenericStructure *port_data = KMS_GENERIC_STRUCTURE (l->data);
    gboolean input;
    gint id;

    input = GPOINTER_TO_INT (kms_generic_structure_get (port_data, INPUT));
    if (input == FALSE) {
      continue;
    }

    id = GPOINTER_TO_INT (kms_generic_structure_get (port_data, ID));
    if (id == self->priv->master_port) {
      GstPad *video_mixer_pad;
      GstElement *capsfilter;

      filtercaps =
          gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING, "AYUV",
          "width", G_TYPE_INT, self->priv->output_width, "height",
          G_TYPE_INT, self->priv->output_height, NULL);

      capsfilter = kms_generic_structure_get (port_data, CAPSFILTER);
      if (capsfilter != NULL) {
        g_object_set (G_OBJECT (capsfilter), "caps", filtercaps, NULL);
      }
      video_mixer_pad = kms_generic_structure_get (port_data, VIDEO_MIXER_PAD);
      if (video_mixer_pad != NULL) {
        g_object_set (video_mixer_pad, "xpos", 0, "ypos", 0, "alpha",
            1.0, "zorder", self->priv->z_master, NULL);
      }
    } else {
      configure_port (port_data);
    }
  }
  g_list_free (values);
  //reconfigure videotestsrc input
  filtercaps =
      gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING, "AYUV",
      "width", G_TYPE_INT, self->priv->output_width, "height",
      G_TYPE_INT, self->priv->output_height, "framerate", GST_TYPE_FRACTION, 15,
      1, NULL);
  g_object_set (G_OBJECT (self->priv->videotestsrc_capsfilter), "caps",
      filtercaps, NULL);
  gst_caps_unref (filtercaps);
}

static void
kms_alpha_blending_set_master_port (KmsAlphaBlending * alpha_blending)
{
  GstPad *pad;
  KmsGenericStructure *port_data;
  gint *key;
  GstCaps *caps;
  gint width, height;
  const GstStructure *str;
  GstElement *videoconvert;

  GST_DEBUG ("set master");

  //get the element with id == master_port
  key = create_gint (alpha_blending->priv->master_port);
  port_data =
      KMS_GENERIC_STRUCTURE (g_hash_table_lookup (alpha_blending->priv->ports,
          key));

  release_gint (key);

  if (port_data == NULL) {
    return;
  }
  //configure the output size to master size
  videoconvert = kms_generic_structure_get (port_data, VIDEOCONVERT);
  pad = gst_element_get_static_pad (videoconvert, "sink");
  caps = gst_pad_get_current_caps (pad);

  if (caps != NULL) {
    str = gst_caps_get_structure (caps, 0);
    if (gst_structure_get_int (str, "width", &width) &&
        gst_structure_get_int (str, "height", &height)) {
      KmsAlphaBlending *mixer;

      mixer = KMS_ALPHA_BLENDING (kms_generic_structure_get (port_data, MIXER));
      mixer->priv->output_height = height;
      mixer->priv->output_width = width;
      kms_alpha_blending_reconfigure_ports (alpha_blending);
    }
    gst_caps_unref (caps);
  }
  g_object_unref (pad);
}

static void
kms_alpha_blending_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  KmsAlphaBlending *self = KMS_ALPHA_BLENDING (object);

  KMS_ALPHA_BLENDING_LOCK (self);

  switch (property_id) {
    case PROP_SET_MASTER:
    {
      GstStructure *master;

      master = g_value_dup_boxed (value);
      gst_structure_get (master, "port", G_TYPE_INT,
          &self->priv->master_port, NULL);
      gst_structure_get (master, "z_order", G_TYPE_INT,
          &self->priv->z_master, NULL);

      kms_alpha_blending_set_master_port (self);
      gst_structure_free (master);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }

  KMS_ALPHA_BLENDING_UNLOCK (self);
}

static void
kms_alpha_blending_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  KmsAlphaBlending *self = KMS_ALPHA_BLENDING (object);

  KMS_ALPHA_BLENDING_LOCK (self);

  switch (property_id) {
    case PROP_SET_MASTER:
    {
      GstStructure *data;

      data = gst_structure_new ("data",
          "master", G_TYPE_INT, self->priv->master_port,
          "z_order", G_TYPE_INT, self->priv->z_master, NULL);
      g_value_set_boxed (value, data);
      gst_structure_free (data);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }

  KMS_ALPHA_BLENDING_UNLOCK (self);
}

static gboolean
remove_elements_from_pipeline (gpointer data)
{
  KmsGenericStructure *port_data = KMS_GENERIC_STRUCTURE (data);
  KmsAlphaBlending *self;
  GstElement *videoconvert, *videoscale, *videorate, *capsfilter, *queue;
  GstPad *video_mixer_pad, *videoconvert_sink_pad;
  gint id;

  self = KMS_ALPHA_BLENDING (kms_generic_structure_get (port_data, MIXER));

  KMS_ALPHA_BLENDING_LOCK (self);

  capsfilter = kms_generic_structure_get (port_data, CAPSFILTER);
  gst_element_unlink (capsfilter, self->priv->videomixer);

  video_mixer_pad = kms_generic_structure_get (port_data, VIDEO_MIXER_PAD);
  if (video_mixer_pad != NULL) {
    gst_element_release_request_pad (self->priv->videomixer, video_mixer_pad);
    g_object_unref (video_mixer_pad);
    kms_generic_structure_set (port_data, VIDEO_MIXER_PAD, NULL);
  }

  videoconvert =
      g_object_ref (kms_generic_structure_get (port_data, VIDEOCONVERT));
  videorate = g_object_ref (kms_generic_structure_get (port_data, VIDEORATE));
  queue = g_object_ref (kms_generic_structure_get (port_data, QUEUE));
  videoscale = g_object_ref (kms_generic_structure_get (port_data, VIDEOSCALE));
  g_object_ref (capsfilter);

  videoconvert_sink_pad =
      kms_generic_structure_get (port_data, VIDEOCONVERT_SINK_PAD);
  g_object_unref (videoconvert_sink_pad);

  kms_generic_structure_set (port_data, VIDEOCONVERT_SINK_PAD, NULL);
  kms_generic_structure_set (port_data, VIDEOCONVERT, NULL);
  kms_generic_structure_set (port_data, VIDEORATE, NULL);
  kms_generic_structure_set (port_data, QUEUE, NULL);
  kms_generic_structure_set (port_data, VIDEOSCALE, NULL);
  kms_generic_structure_set (port_data, CAPSFILTER, NULL);

  gst_bin_remove_many (GST_BIN (self), videoconvert, videoscale, capsfilter,
      videorate, queue, NULL);

  id = GPOINTER_TO_INT (kms_generic_structure_get (port_data, ID));
  kms_base_hub_unlink_video_src (KMS_BASE_HUB (self), id);

  KMS_ALPHA_BLENDING_UNLOCK (self);

  gst_element_set_state (videoconvert, GST_STATE_NULL);
  gst_element_set_state (videoscale, GST_STATE_NULL);
  gst_element_set_state (videorate, GST_STATE_NULL);
  gst_element_set_state (capsfilter, GST_STATE_NULL);
  gst_element_set_state (queue, GST_STATE_NULL);

  g_object_unref (videoconvert);
  g_object_unref (videoscale);
  g_object_unref (videorate);
  g_object_unref (capsfilter);
  g_object_unref (queue);

  return G_SOURCE_REMOVE;
}

static GstPadProbeReturn
cb_EOS_received (GstPad * pad, GstPadProbeInfo * info, gpointer data)
{
  KmsGenericStructure *port_data = KMS_GENERIC_STRUCTURE (data);
  KmsAlphaBlending *self;
  GstEvent *event;
  gboolean removing;
  gint probe_id;

  self = KMS_ALPHA_BLENDING (kms_generic_structure_get (port_data, MIXER));

  if (GST_EVENT_TYPE (GST_PAD_PROBE_INFO_EVENT (info)) != GST_EVENT_EOS) {
    return GST_PAD_PROBE_OK;
  }

  KMS_ALPHA_BLENDING_LOCK (self);

  removing = GPOINTER_TO_INT (kms_generic_structure_get (port_data, REMOVING));
  if (!removing) {
    kms_generic_structure_set (port_data, EOS_MANAGED, GINT_TO_POINTER (TRUE));
    KMS_ALPHA_BLENDING_UNLOCK (self);
    return GST_PAD_PROBE_OK;
  }

  probe_id = GPOINTER_TO_INT (kms_generic_structure_get (port_data, PROBE_ID));
  if (probe_id > 0) {
    gst_pad_remove_probe (pad, probe_id);
    kms_generic_structure_set (port_data, PROBE_ID, GINT_TO_POINTER (0));
  }

  KMS_ALPHA_BLENDING_UNLOCK (self);

  event = gst_event_new_eos ();
  gst_pad_send_event (pad, event);

  kms_loop_idle_add_full (self->priv->loop, G_PRIORITY_DEFAULT,
      remove_elements_from_pipeline, kms_generic_structure_ref (data),
      (GDestroyNotify) kms_generic_structure_unref);

  return GST_PAD_PROBE_DROP;
}

static void
kms_alpha_blending_port_data_destroy (gpointer data)
{
  KmsGenericStructure *port_data = KMS_GENERIC_STRUCTURE (data);
  KmsAlphaBlending *self;
  gboolean input;
  gint id;

  self = KMS_ALPHA_BLENDING (kms_generic_structure_get (port_data, MIXER));

#if AUDIO
  GstPad *audiosink;
  gchar *padname;
#endif

  KMS_ALPHA_BLENDING_LOCK (self);

  kms_generic_structure_set (port_data, REMOVING, GINT_TO_POINTER (TRUE));
  id = GPOINTER_TO_INT (kms_generic_structure_get (port_data, ID));

  kms_base_hub_unlink_video_sink (KMS_BASE_HUB (self), id);
  kms_base_hub_unlink_audio_sink (KMS_BASE_HUB (self), id);

  input = GPOINTER_TO_INT (kms_generic_structure_get (port_data, INPUT));
  if (input) {
    GstElement *videoconvert, *videorate;
    GstEvent *event;
    gboolean result;
    GstPad *pad;

    videorate = kms_generic_structure_get (port_data, VIDEORATE);
    videoconvert = kms_generic_structure_get (port_data, VIDEOCONVERT);

    if (videorate == NULL) {
      KMS_ALPHA_BLENDING_UNLOCK (self);
      return;
    }

    pad = gst_element_get_static_pad (videorate, "sink");

    if (pad == NULL) {
      KMS_ALPHA_BLENDING_UNLOCK (self);
      return;
    }

    if (!GST_OBJECT_FLAG_IS_SET (pad, GST_PAD_FLAG_EOS)) {

      event = gst_event_new_eos ();
      result = gst_pad_send_event (pad, event);

      if (input && self->priv->n_elems > 0) {
        kms_generic_structure_set (port_data, INPUT, GINT_TO_POINTER (FALSE));
        self->priv->n_elems--;
      }

      if (!result) {
        GST_WARNING ("EOS event did not send");
      }

      gst_element_unlink (videoconvert, videorate);
      g_object_unref (pad);

      KMS_ALPHA_BLENDING_UNLOCK (self);
    } else {
      gboolean remove = FALSE;

      /* EOS callback was triggered before we could remove the port data */
      /* so we have to remove elements to avoid memory leaks. */
      remove =
          GPOINTER_TO_INT (kms_generic_structure_get (port_data, EOS_MANAGED));

      gst_element_unlink (videoconvert, videorate);
      g_object_unref (pad);

      KMS_ALPHA_BLENDING_UNLOCK (self);

      if (remove) {
        /* Remove pipeline without helding the mutex */
        kms_loop_idle_add_full (self->priv->loop, G_PRIORITY_DEFAULT,
            remove_elements_from_pipeline,
            kms_generic_structure_ref (data),
            (GDestroyNotify) kms_generic_structure_unref);
      }
    }
  } else {
    GstElement *videoconvert;
    GstPad *video_mixer_pad, *videoconvert_sink_pad;
    gint probe_id, link_probe_id;

    videoconvert =
        g_object_ref (kms_generic_structure_get (port_data, VIDEOCONVERT));
    kms_generic_structure_set (port_data, VIDEOCONVERT, NULL);

    probe_id =
        GPOINTER_TO_INT (kms_generic_structure_get (port_data, PROBE_ID));
    video_mixer_pad = kms_generic_structure_get (port_data, VIDEO_MIXER_PAD);

    if (probe_id > 0) {
      gst_pad_remove_probe (video_mixer_pad, probe_id);
    }

    link_probe_id =
        GPOINTER_TO_INT (kms_generic_structure_get (port_data, LINK_PROBE_ID));
    videoconvert_sink_pad =
        kms_generic_structure_get (port_data, VIDEOCONVERT_SINK_PAD);

    if (link_probe_id > 0) {
      gst_pad_remove_probe (videoconvert_sink_pad, link_probe_id);
    }
    KMS_ALPHA_BLENDING_UNLOCK (self);

    gst_bin_remove (GST_BIN (self), videoconvert);
    gst_element_set_state (videoconvert, GST_STATE_NULL);
    g_object_unref (videoconvert);
  }

#if AUDIO
  padname = g_strdup_printf (AUDIO_SINK_PAD, id);
  audiosink = gst_element_get_static_pad (self->priv->audiomixer, padname);

  gst_element_release_request_pad (self->priv->audiomixer, audiosink);

  gst_object_unref (audiosink);
  g_free (padname);
#else
  {
    gchar *name = g_strdup_printf (AUDIO_FAKESINK, id);
    GstElement *fakesink = gst_bin_get_by_name (GST_BIN (self), name);

    gst_bin_remove (GST_BIN (self), fakesink);

    gst_element_set_state (fakesink, GST_STATE_NULL);
    g_object_unref (fakesink);
    g_free (name);
  }
#endif

  kms_generic_structure_unref (port_data);
}

static GstPadProbeReturn
link_to_videomixer (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstPadTemplate *sink_pad_template;
  KmsGenericStructure *data = KMS_GENERIC_STRUCTURE (user_data);
  KmsAlphaBlending *mixer;
  GstElement *videoconvert, *videoscale, *capsfilter, *videorate, *queue;
  GstPad *video_mixer_pad;
  gint id, probe_id;

  if (GST_EVENT_TYPE (GST_PAD_PROBE_INFO_EVENT (info)) != GST_EVENT_CAPS) {
    return GST_PAD_PROBE_PASS;
  }

  mixer = KMS_ALPHA_BLENDING (kms_generic_structure_get (data, MIXER));
  GST_DEBUG ("stream start detected");

  KMS_ALPHA_BLENDING_LOCK (mixer);

  kms_generic_structure_set (data, LINK_PROBE_ID, GINT_TO_POINTER (0));

  sink_pad_template =
      gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS (mixer->priv->
          videomixer), "sink_%u");

  if (G_UNLIKELY (sink_pad_template == NULL)) {
    GST_ERROR_OBJECT (mixer, "Error taking a new pad from videomixer");
    KMS_ALPHA_BLENDING_UNLOCK (mixer);
    return GST_PAD_PROBE_DROP;
  }

  id = GPOINTER_TO_INT (kms_generic_structure_get (data, ID));
  if (mixer->priv->master_port == id) {
    //master_port, reconfigurate the output_width and heigth_width
    //and all the ports already created
    GstEvent *event;
    GstCaps *caps;
    gint width, height;
    const GstStructure *str;

    event = gst_pad_probe_info_get_event (info);
    gst_event_parse_caps (event, &caps);

    GST_DEBUG ("caps %" GST_PTR_FORMAT, caps);
    if (caps != NULL) {
      str = gst_caps_get_structure (caps, 0);
      if (gst_structure_get_int (str, "width", &width) &&
          gst_structure_get_int (str, "height", &height)) {
        mixer->priv->output_height = height;
        mixer->priv->output_width = width;
      }
    }
  }

  if (mixer->priv->videotestsrc == NULL) {
    GstCaps *filtercaps;
    GstPad *pad;

    mixer->priv->videotestsrc = gst_element_factory_make ("videotestsrc", NULL);
    mixer->priv->videotestsrc_capsfilter =
        gst_element_factory_make (CAPSFILTER, NULL);

    g_object_set (mixer->priv->videotestsrc, "is-live", TRUE, "pattern",
        /*black */ 2, NULL);

    filtercaps =
        gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING, "AYUV",
        "width", G_TYPE_INT, mixer->priv->output_width,
        "height", G_TYPE_INT, mixer->priv->output_height,
        "framerate", GST_TYPE_FRACTION, 15, 1, NULL);
    g_object_set (G_OBJECT (mixer->priv->videotestsrc_capsfilter), "caps",
        filtercaps, NULL);
    gst_caps_unref (filtercaps);

    gst_bin_add_many (GST_BIN (mixer), mixer->priv->videotestsrc,
        mixer->priv->videotestsrc_capsfilter, NULL);

    gst_element_link (mixer->priv->videotestsrc,
        mixer->priv->videotestsrc_capsfilter);

    /*link capsfilter -> videomixer */
    pad = gst_element_request_pad (mixer->priv->videomixer,
        sink_pad_template, NULL, NULL);
    gst_element_link_pads (mixer->priv->videotestsrc_capsfilter, NULL,
        mixer->priv->videomixer, GST_OBJECT_NAME (pad));
    g_object_set (pad, "xpos", 0, "ypos", 0, "alpha", 0.0, "zorder", 0, NULL);
    g_object_unref (pad);

    gst_element_sync_state_with_parent (mixer->priv->videotestsrc_capsfilter);
    gst_element_sync_state_with_parent (mixer->priv->videotestsrc);
  }

  videoscale = gst_element_factory_make (VIDEOSCALE, NULL);
  capsfilter = gst_element_factory_make (CAPSFILTER, NULL);
  videorate = gst_element_factory_make (VIDEORATE, NULL);
  queue = gst_element_factory_make (QUEUE, NULL);

  kms_generic_structure_set (data, VIDEOSCALE, videoscale);
  kms_generic_structure_set (data, CAPSFILTER, capsfilter);
  kms_generic_structure_set (data, VIDEORATE, videorate);
  kms_generic_structure_set (data, QUEUE, queue);
  kms_generic_structure_set (data, INPUT, GINT_TO_POINTER (TRUE));

  gst_bin_add_many (GST_BIN (mixer), queue, videorate,
      videoscale, capsfilter, NULL);

  g_object_set (videorate, "average-period", 200 * GST_MSECOND, NULL);
  g_object_set (queue, "flush-on-eos", TRUE, NULL);

  gst_element_link_many (videorate, queue, videoscale, capsfilter, NULL);

  /*link capsfilter -> videomixer */
  video_mixer_pad =
      gst_element_request_pad (mixer->priv->videomixer,
      sink_pad_template, NULL, NULL);
  kms_generic_structure_set (data, VIDEO_MIXER_PAD, video_mixer_pad);
  gst_element_link_pads (capsfilter, NULL,
      mixer->priv->videomixer, GST_OBJECT_NAME (video_mixer_pad));

  videoconvert = kms_generic_structure_get (data, VIDEOCONVERT);
  gst_element_link (videoconvert, videorate);

  probe_id = gst_pad_add_probe (video_mixer_pad,
      GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      (GstPadProbeCallback) cb_EOS_received,
      kms_generic_structure_ref (data),
      (GDestroyNotify) kms_generic_structure_unref);

  kms_generic_structure_set (data, PROBE_ID, GINT_TO_POINTER (probe_id));

  gst_element_sync_state_with_parent (videoscale);
  gst_element_sync_state_with_parent (capsfilter);
  gst_element_sync_state_with_parent (videorate);
  gst_element_sync_state_with_parent (queue);

  /* configure videomixer pad */
  mixer->priv->n_elems++;

  if (mixer->priv->master_port == id) {
    kms_alpha_blending_reconfigure_ports (mixer);
  } else {
    configure_port (data);
  }

  KMS_ALPHA_BLENDING_UNLOCK (mixer);

  return GST_PAD_PROBE_REMOVE;
}

static void
kms_alpha_blending_unhandle_port (KmsBaseHub * mixer, gint id)
{
  KmsAlphaBlending *self = KMS_ALPHA_BLENDING (mixer);

  GST_DEBUG ("unhandle id %d", id);

  KMS_ALPHA_BLENDING_LOCK (self);

  g_hash_table_remove (self->priv->ports, &id);

  KMS_ALPHA_BLENDING_UNLOCK (self);

  KMS_BASE_HUB_CLASS (G_OBJECT_CLASS
      (kms_alpha_blending_parent_class))->unhandle_port (mixer, id);

  GST_DEBUG ("end unhandle port id %d", id);
}

static KmsGenericStructure *
kms_alpha_blending_port_data_create (KmsAlphaBlending * mixer, gint id)
{
  KmsGenericStructure *data;
  GstElement *videoconvert;
  GstPad *videoconvert_sink_pad;
  gint link_probe_id;

#if AUDIO
  gchar *padname;
#endif

  data = kms_generic_structure_new ();
  kms_generic_structure_set (data, MIXER, mixer);
  kms_generic_structure_set (data, ID, GINT_TO_POINTER (id));
  kms_generic_structure_set (data, INPUT, GINT_TO_POINTER (FALSE));
  kms_generic_structure_set (data, CONFIGURATED, GINT_TO_POINTER (FALSE));
  kms_generic_structure_set (data, REMOVING, GINT_TO_POINTER (FALSE));
  kms_generic_structure_set (data, EOS_MANAGED, GINT_TO_POINTER (FALSE));

  videoconvert = gst_element_factory_make (VIDEOCONVERT, NULL);
  kms_generic_structure_set (data, VIDEOCONVERT, videoconvert);

  gst_bin_add_many (GST_BIN (mixer), videoconvert, NULL);

  gst_element_sync_state_with_parent (videoconvert);

  /*link basemixer -> video_agnostic */
  kms_base_hub_link_video_sink (KMS_BASE_HUB (mixer), id, videoconvert, "sink",
      FALSE);

#if AUDIO
  padname = g_strdup_printf (AUDIO_SINK_PAD, id);
  kms_base_hub_link_audio_sink (KMS_BASE_HUB (mixer), id,
      mixer->priv->audiomixer, padname, FALSE);
  g_free (padname);
#else
  {
    GstElement *fakesink;
    gchar *name = g_strdup_printf (AUDIO_FAKESINK, id);

    fakesink = gst_element_factory_make ("fakesink", name);
    g_free (name);
    g_object_set (fakesink, "async", FALSE, NULL);
    gst_bin_add (GST_BIN (mixer), fakesink);
    gst_element_sync_state_with_parent (fakesink);

    kms_base_hub_link_audio_sink (KMS_BASE_HUB (mixer), id,
        fakesink, "sink", FALSE);
  }
#endif

  videoconvert_sink_pad = gst_element_get_static_pad (videoconvert, "sink");
  kms_generic_structure_set (data, VIDEOCONVERT_SINK_PAD,
      videoconvert_sink_pad);

  link_probe_id = gst_pad_add_probe (videoconvert_sink_pad,
      GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_BLOCK,
      (GstPadProbeCallback) link_to_videomixer,
      kms_generic_structure_ref (data),
      (GDestroyNotify) kms_generic_structure_unref);
  kms_generic_structure_set (data, LINK_PROBE_ID,
      GINT_TO_POINTER (link_probe_id));

  return data;
}

#if AUDIO
static gint
get_stream_id_from_padname (const gchar * name)
{
  gint64 id;

  if (name == NULL)
    return -1;

  if (!g_str_has_prefix (name, AUDIO_SRC_PAD_PREFIX))
    return -1;

  id = g_ascii_strtoll (name + LENGTH_AUDIO_SRC_PAD_PREFIX, NULL, 10);
  if (id > G_MAXINT)
    return -1;

  return id;
}

static void
pad_added_cb (GstElement * element, GstPad * pad, gpointer data)
{
  gint id;
  KmsAlphaBlending *self = KMS_ALPHA_BLENDING (data);

  if (gst_pad_get_direction (pad) != GST_PAD_SRC)
    return;

  id = get_stream_id_from_padname (GST_OBJECT_NAME (pad));

  if (id < 0) {
    GST_ERROR_OBJECT (self, "Invalid HubPort for %" GST_PTR_FORMAT, pad);
    return;
  }

  kms_base_hub_link_audio_src (KMS_BASE_HUB (self), id,
      self->priv->audiomixer, GST_OBJECT_NAME (pad), TRUE);
}
#endif

#if AUDIO
static void
pad_removed_cb (GstElement * element, GstPad * pad, gpointer data)
{
  GST_DEBUG ("Removed pad %" GST_PTR_FORMAT, pad);
}
#endif

static gint
kms_alpha_blending_handle_port (KmsBaseHub * mixer,
    GstElement * mixer_end_point)
{
  KmsAlphaBlending *self = KMS_ALPHA_BLENDING (mixer);
  KmsGenericStructure *port_data;
  gint port_id;

  port_id = KMS_BASE_HUB_CLASS (G_OBJECT_CLASS
      (kms_alpha_blending_parent_class))->handle_port (mixer, mixer_end_point);

  if (port_id < 0) {
    return port_id;
  }

  KMS_ALPHA_BLENDING_LOCK (self);

  if (self->priv->videomixer == NULL) {
    GstElement *videorate_mixer;

    videorate_mixer = gst_element_factory_make (VIDEORATE, NULL);
    self->priv->videomixer = gst_element_factory_make ("compositor", NULL);
    g_object_set (G_OBJECT (self->priv->videomixer), "background", 1, NULL);
    self->priv->mixer_video_agnostic =
        gst_element_factory_make ("agnosticbin", NULL);

    gst_bin_add_many (GST_BIN (mixer), self->priv->videomixer, videorate_mixer,
        self->priv->mixer_video_agnostic, NULL);

    gst_element_sync_state_with_parent (self->priv->videomixer);
    gst_element_sync_state_with_parent (videorate_mixer);
    gst_element_sync_state_with_parent (self->priv->mixer_video_agnostic);

    gst_element_link_many (self->priv->videomixer, videorate_mixer,
        self->priv->mixer_video_agnostic, NULL);
  }
#if AUDIO
  if (self->priv->audiomixer == NULL) {
    self->priv->audiomixer = gst_element_factory_make ("kmsaudiomixer", NULL);

    gst_bin_add (GST_BIN (mixer), self->priv->audiomixer);

    gst_element_sync_state_with_parent (self->priv->audiomixer);
    g_signal_connect (self->priv->audiomixer, "pad-added",
        G_CALLBACK (pad_added_cb), self);
    g_signal_connect (self->priv->audiomixer, "pad-removed",
        G_CALLBACK (pad_removed_cb), self);
  }
#endif

  kms_base_hub_link_video_src (KMS_BASE_HUB (self), port_id,
      self->priv->mixer_video_agnostic, "src_%u", TRUE);

  port_data = kms_alpha_blending_port_data_create (self, port_id);

  g_hash_table_insert (self->priv->ports, create_gint (port_id), port_data);

  KMS_ALPHA_BLENDING_UNLOCK (self);

  return port_id;
}

static void
kms_alpha_blending_set_port_properties (KmsAlphaBlending * self,
    GstStructure * properties)
{
  gint port, z_order;
  gint *key;
  gfloat relative_x, relative_y, relative_width, relative_height;
  KmsGenericStructure *port_data;
  gboolean fields_ok = TRUE;

  GST_DEBUG ("setting port properties");

  fields_ok = fields_ok
      && gst_structure_get (properties, "relative_x", G_TYPE_FLOAT, &relative_x,
      NULL);
  fields_ok = fields_ok
      && gst_structure_get (properties, "relative_y", G_TYPE_FLOAT, &relative_y,
      NULL);
  fields_ok = fields_ok
      && gst_structure_get (properties, "relative_width", G_TYPE_FLOAT,
      &relative_width, NULL);
  fields_ok = fields_ok
      && gst_structure_get (properties, "relative_height", G_TYPE_FLOAT,
      &relative_height, NULL);
  fields_ok = fields_ok
      && gst_structure_get (properties, "port", G_TYPE_INT, &port, NULL);
  fields_ok = fields_ok
      && gst_structure_get (properties, "z_order", G_TYPE_INT, &z_order, NULL);

  if (!fields_ok) {
    GST_WARNING_OBJECT (self, "Invalid properties structure received");
    return;
  }
  KMS_ALPHA_BLENDING_LOCK (self);
  key = create_gint (port);
  port_data = KMS_GENERIC_STRUCTURE (g_hash_table_lookup (self->priv->ports,
          key));
  release_gint (key);

  if (port_data == NULL) {
    KMS_ALPHA_BLENDING_UNLOCK (self);
    return;
  }
  kms_generic_structure_set (port_data, "relative_x",
      GINT_TO_POINTER (relative_x));
  kms_generic_structure_set (port_data, "relative_width",
      GINT_TO_POINTER (relative_width));
  kms_generic_structure_set (port_data, "relative_height",
      GINT_TO_POINTER (relative_height));
  kms_generic_structure_set (port_data, "z_order", GINT_TO_POINTER (z_order));
  kms_generic_structure_set (port_data, CONFIGURATED, GINT_TO_POINTER (TRUE));
  configure_port (port_data);

  KMS_ALPHA_BLENDING_UNLOCK (self);
}

static void
kms_alpha_blending_dispose (GObject * object)
{
  KmsAlphaBlending *self = KMS_ALPHA_BLENDING (object);

  KMS_ALPHA_BLENDING_LOCK (self);
  g_hash_table_remove_all (self->priv->ports);
  KMS_ALPHA_BLENDING_UNLOCK (self);
  g_clear_object (&self->priv->loop);

  G_OBJECT_CLASS (kms_alpha_blending_parent_class)->dispose (object);
}

static void
kms_alpha_blending_finalize (GObject * object)
{
  KmsAlphaBlending *self = KMS_ALPHA_BLENDING (object);

  g_rec_mutex_clear (&self->priv->mutex);

  if (self->priv->ports != NULL) {
    g_hash_table_unref (self->priv->ports);
    self->priv->ports = NULL;
  }

  G_OBJECT_CLASS (kms_alpha_blending_parent_class)->finalize (object);
}

static void
kms_alpha_blending_class_init (KmsAlphaBlendingClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  KmsBaseHubClass *base_hub_class = KMS_BASE_HUB_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "AlphaBlending", "Generic", "Mixer element using the alpha channel "
      " in one output flow", "David Fernandez <d.fernandezlop@gmail.com>");

  klass->set_port_properties = GST_DEBUG_FUNCPTR
      (kms_alpha_blending_set_port_properties);

  gobject_class->set_property = kms_alpha_blending_set_property;
  gobject_class->get_property = kms_alpha_blending_get_property;
  gobject_class->dispose = GST_DEBUG_FUNCPTR (kms_alpha_blending_dispose);
  gobject_class->finalize = GST_DEBUG_FUNCPTR (kms_alpha_blending_finalize);

  base_hub_class->handle_port =
      GST_DEBUG_FUNCPTR (kms_alpha_blending_handle_port);
  base_hub_class->unhandle_port =
      GST_DEBUG_FUNCPTR (kms_alpha_blending_unhandle_port);

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&audio_src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&video_src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&audio_sink_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&video_sink_factory));

  g_object_class_install_property (gobject_class, PROP_SET_MASTER,
      g_param_spec_boxed ("set-master", "set master",
          "Set the master port",
          GST_TYPE_STRUCTURE, (GParamFlags) G_PARAM_READWRITE));

  /* Signals initialization */
  kms_alpha_blending_signals[SIGNAL_SET_PORT_PROPERTIES] =
      g_signal_new ("set-port-properties",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_ACTION | G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (KmsAlphaBlendingClass, set_port_properties), NULL, NULL,
      __kms_core_marshal_VOID__BOXED, G_TYPE_NONE, 1, GST_TYPE_STRUCTURE);

  /* Registers a private structure for the instantiatable type */
  g_type_class_add_private (klass, sizeof (KmsAlphaBlendingPrivate));
}

static void
kms_alpha_blending_init (KmsAlphaBlending * self)
{
  self->priv = KMS_ALPHA_BLENDING_GET_PRIVATE (self);

  g_rec_mutex_init (&self->priv->mutex);

  self->priv->ports = g_hash_table_new_full (g_int_hash, g_int_equal,
      release_gint, kms_alpha_blending_port_data_destroy);
  self->priv->n_elems = 0;
  self->priv->master_port = 0;
  self->priv->z_master = 5;
  self->priv->output_height = 480;
  self->priv->output_width = 640;

  self->priv->loop = kms_loop_new ();
}

gboolean
kms_alpha_blending_plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, PLUGIN_NAME, GST_RANK_NONE,
      KMS_TYPE_ALPHA_BLENDING);
}
