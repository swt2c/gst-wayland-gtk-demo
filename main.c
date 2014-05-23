/*
 * Copyright (C) 2014 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */
#include <gst/video/videooverlay.h>
#include <gst/wayland/wayland.h>
#include <gtk/gtk.h>
#include <gdk/gdkwayland.h>

struct AppData {
  GtkWidget *video_window;
  GtkWidget *app_window;
  GstElement *pipeline;
  GstElement *sink;
  struct wl_display *display_handle;
  struct wl_surface *window_handle;
  guint width, height;
  struct wl_subcompositor *subcompositor;
  struct wl_subsurface *subsurface;
  gboolean video_frozen;
};

static void
registry_handle_global (void *data, struct wl_registry *registry,
    uint32_t id, const char *interface, uint32_t version)
{
  struct wl_subcompositor ** wlsubcompositor = data;

  if (g_strcmp0 (interface, "wl_subcompositor") == 0) {
    *wlsubcompositor =
        wl_registry_bind (registry, id, &wl_subcompositor_interface, 1);
  }
}

static const struct wl_registry_listener registry_listener = {
  registry_handle_global,
  NULL
};

static struct wl_subcompositor *
wayland_find_subcompositor (GdkDisplay *display)
{
  struct wl_display *wldisplay;
  struct wl_registry *wlregistry;
  struct wl_subcompositor *wlsubcompositor = NULL;

  wldisplay = gdk_wayland_display_get_wl_display (display);
  wlregistry = wl_display_get_registry (wldisplay);
  wl_registry_add_listener (wlregistry, &registry_listener, &wlsubcompositor);
  wl_display_roundtrip (wldisplay);
  wl_registry_destroy (wlregistry);

  return wlsubcompositor;
}

static GstBusSyncReply
bus_sync_handler (GstBus * bus, GstMessage * message, gpointer user_data)
{
  struct AppData *d = user_data;

  if (gst_is_wayland_display_handle_need_context_message (message)) {
    if (d->display_handle != 0) {
      GstContext *context =
          gst_wayland_display_handle_context_new (d->display_handle);
      gst_element_set_context(d->pipeline, context);
    } else {
      g_warning ("Should have obtained display_handle by now!\n");
    }

    gst_message_unref (message);
    return GST_BUS_DROP;
  } else if (gst_is_video_overlay_prepare_window_handle_message (message)) {
    if (d->window_handle != 0) {
      GstVideoOverlay *overlay;

      // GST_MESSAGE_SRC (message) will be the video sink element
      overlay = GST_VIDEO_OVERLAY (GST_MESSAGE_SRC (message));
      gst_video_overlay_set_window_handle (overlay, (guintptr) d->window_handle);
      gst_wayland_video_set_surface_size (GST_WAYLAND_VIDEO (overlay),
          d->width, d->height);
    } else {
      g_warning ("Should have obtained window_handle by now!\n");
    }

    gst_message_unref (message);
    return GST_BUS_DROP;
  }

  return GST_BUS_PASS;
}

static void
video_widget_realize_cb (GtkWidget * widget, gpointer data)
{
  struct AppData *d = data;
  GdkDisplay *display;
  GdkWindow *window;
  struct wl_surface *parent_surface;
  struct wl_surface *surface;
  gdouble x, y;

  display = gtk_widget_get_display (widget);
  window = gtk_widget_get_window (widget);

  gdk_window_coords_to_parent (window, 0.0, 0.0, &x, &y);

  parent_surface = gdk_wayland_window_get_wl_surface (window);
  gdk_window_ensure_native (window);
  gdk_wayland_window_set_use_custom_surface (window);
  surface = gdk_wayland_window_get_wl_surface (window);

  g_print ("realize: parent_surface: %p, surface %p, x %d, y %d\n",
      parent_surface, surface, (int) x, (int) y);

  d->subsurface = wl_subcompositor_get_subsurface (d->subcompositor, surface,
      parent_surface);
  wl_subsurface_set_position (d->subsurface, x, y);
  wl_subsurface_set_desync (d->subsurface);

  d->display_handle = gdk_wayland_display_get_wl_display (display);
  d->window_handle = surface;
  d->width = gtk_widget_get_allocated_width (widget);
  d->height = gtk_widget_get_allocated_height (widget);
  d->video_frozen = FALSE;
}

static void
video_widget_unrealize_cb (GtkWidget * widget, gpointer data)
{
  struct AppData *d = data;

  if (!d->window_handle)
    return;

  g_print ("unrealize_cb\n");
  gst_wayland_video_pause_rendering (GST_WAYLAND_VIDEO (d->sink));
  d->video_frozen = TRUE;
}

static gboolean
app_window_configure_cb (GtkWidget * widget, GdkEvent * event, gpointer data)
{
  struct AppData *d = data;
  GdkEventConfigure *cfg_event = (GdkEventConfigure *) event;

  if (!d->window_handle)
    return FALSE;

  g_print ("app_configure_cb x %d, y %d, w %d, h %d\n", cfg_event->x,
        cfg_event->y, cfg_event->width, cfg_event->height);

  if (!d->video_frozen) {
    d->video_frozen = TRUE;
    gst_wayland_video_pause_rendering (GST_WAYLAND_VIDEO (d->sink));
    wl_subsurface_set_sync (d->subsurface);
  }

  return FALSE;
}

static gboolean
video_widget_configure_cb (GtkWidget * widget, GdkEvent * event, gpointer data)
{
  struct AppData *d = data;
  GdkEventConfigure *cfg_event = (GdkEventConfigure *) event;

  if (!d->window_handle)
    return FALSE;

  g_print ("configure_cb x %d, y %d, w %d, h %d\n", cfg_event->x,
      cfg_event->y, cfg_event->width, cfg_event->height);

  if (cfg_event->x != 0 || cfg_event->y != 0) {
    wl_subsurface_set_position (d->subsurface, cfg_event->x,
        cfg_event->y);
    gst_wayland_video_set_surface_size (GST_WAYLAND_VIDEO (d->sink),
        cfg_event->width, cfg_event->height);
  }

  return FALSE;
}

static gboolean
video_widget_draw_cb (GtkWidget * widget, gpointer cr, gpointer data)
{
  struct AppData *d = data;

  if (!d->window_handle)
    return FALSE;

  if (d->video_frozen) {
    g_print ("draw_cb\n");
    d->video_frozen = FALSE;
    gst_wayland_video_resume_rendering (GST_WAYLAND_VIDEO (d->sink));
    wl_subsurface_set_desync (d->subsurface);
  }

  return FALSE;
}

int
main (int argc, char **argv)
{
  struct AppData data = {0};
  GstBus *bus;

  gtk_init (&argc, &argv);
  gst_init (&argc, &argv);

  // try to find the wayland subcompositor object
  data.subcompositor =
      wayland_find_subcompositor (gdk_display_get_default ());
  g_print ("wl_subcompositor: %p\n", data.subcompositor);

  // create the window
  data.app_window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title (GTK_WINDOW (data.app_window), "GStreamer Wayland Demo");
  g_signal_connect (data.app_window, "destroy",
      G_CALLBACK (gtk_main_quit), NULL);
  g_signal_connect (data.app_window, "configure-event",
      G_CALLBACK (app_window_configure_cb), &data);

  data.video_window = gtk_drawing_area_new ();
  g_signal_connect (data.video_window, "realize",
      G_CALLBACK (video_widget_realize_cb), &data);
  g_signal_connect (data.video_window, "unrealize",
      G_CALLBACK (video_widget_unrealize_cb), &data);
  g_signal_connect (data.video_window, "configure-event",
      G_CALLBACK (video_widget_configure_cb), &data);
  g_signal_connect (data.video_window, "draw",
      G_CALLBACK (video_widget_draw_cb), &data);
//   g_signal_connect (data.video_window, "damage-event",
//       G_CALLBACK (video_widget_draw_cb), &data);

  gtk_widget_set_double_buffered (data.video_window, FALSE);
  gtk_widget_set_app_paintable (data.video_window, TRUE);
  gtk_container_add (GTK_CONTAINER (data.app_window), data.video_window);

  // show the GUI
  gtk_widget_show_all (data.app_window);

  // realize window now so that the video window gets created and we can
  // obtain its window handle before the pipeline is started up and the video
  // sink asks for the handle of the window to render onto
  gtk_widget_realize (data.app_window);

  // we should have the handle now
  g_assert (data.window_handle != 0);

  data.pipeline = gst_parse_launch ("videotestsrc pattern=18 ! waylandsink name=sink", NULL);
  data.sink = gst_bin_get_by_name (GST_BIN (data.pipeline), "sink");

  // set up sync handler for setting the xid once the pipeline is started
  bus = gst_pipeline_get_bus (GST_PIPELINE (data.pipeline));
  gst_bus_set_sync_handler (bus, (GstBusSyncHandler) bus_sync_handler, &data,
      NULL);
  gst_object_unref (bus);

  // play
  gst_element_set_state (data.pipeline, GST_STATE_PLAYING);
  gtk_main ();
  gst_element_set_state (data.pipeline, GST_STATE_NULL);

  gst_object_unref (data.sink);
  gst_object_unref (data.pipeline);

  return 0;
}
