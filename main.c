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

#define GST_USE_UNSTABLE_API

#include <gst/video/videooverlay.h>
#include <gst/wayland/wayland.h>
#include <gtk/gtk.h>
#include <gdk/gdkwayland.h>
#include <cairo/cairo.h>

struct AppData {
  GtkWidget *video_window;
  GtkWidget *app_window;
  GstElement *pipeline;
  GstVideoOverlay *overlay;
  GstWaylandVideo *wlvideo;
  struct wl_display *display_handle;
  struct wl_surface *window_handle;
  GtkAllocation video_widget_allocation;
  gboolean geometry_changing;
  char **argv;
  int current_uri; /* index for argv */
};

static gchar *
find_file (const gchar * name)
{
  const gchar * const * system_dirs = g_get_system_data_dirs ();
  gchar * ret;
  while (*system_dirs) {
    ret = g_build_filename (*system_dirs, "gst-wayland-gtk-demo", name, NULL);
    if (g_file_test (ret, G_FILE_TEST_EXISTS)) {
      g_print ("Found '%s' at '%s'\n", name, ret);
      return ret;
    }
    g_free (ret);
    system_dirs++;
  }
  return g_strdup (name);
}

static void
on_about_to_finish (GstElement * playbin, struct AppData * d)
{
  if (d->argv[++d->current_uri] == NULL)
    d->current_uri = 1;

  g_print ("Now playing %s\n", d->argv[d->current_uri]);
  g_object_set (playbin, "uri", d->argv[d->current_uri], NULL);
}

static GstBusSyncReply
bus_sync_handler (GstBus * bus, GstMessage * message, gpointer user_data)
{
  struct AppData *d = user_data;

  if (gst_is_wayland_display_handle_need_context_message (message)) {
    if (d->display_handle != 0) {
      GstContext *context;

      context = gst_wayland_display_handle_context_new (d->display_handle);
      gst_element_set_context(GST_ELEMENT (GST_MESSAGE_SRC (message)), context);

      /* HACK save the pointer to the sink (which implements GstWaylandVideo)
       * from this point. Unfortunately, d->overlay can also be the playbin
       * instead of waylandsink */
      d->wlvideo = GST_WAYLAND_VIDEO (GST_MESSAGE_SRC (message));
    } else {
      g_warning ("Should have obtained display_handle by now!\n");
    }

    gst_message_unref (message);
    return GST_BUS_DROP;
  } else if (gst_is_video_overlay_prepare_window_handle_message (message)) {
    if (d->window_handle != 0) {
      /* GST_MESSAGE_SRC (message) will be the overlay object that we have to
       * use. This may be waylandsink, but it may also be playbin. In the latter
       * case, we must make sure to use playbin instead of waylandsink, because
       * playbin resets the window handle and render_rectangle after restarting
       * playback and the actual window size is lost */
      d->overlay = GST_VIDEO_OVERLAY (GST_MESSAGE_SRC (message));

      g_print ("setting window handle and size (%d x %d)\n",
          d->video_widget_allocation.width, d->video_widget_allocation.height);

      gst_video_overlay_set_window_handle (d->overlay, (guintptr) d->window_handle);
      gst_video_overlay_set_render_rectangle (d->overlay,
          d->video_widget_allocation.x, d->video_widget_allocation.y,
          d->video_widget_allocation.width, d->video_widget_allocation.height);
    } else {
      g_warning ("Should have obtained window_handle by now!\n");
    }

    gst_message_unref (message);
    return GST_BUS_DROP;
  }

  return GST_BUS_PASS;
}

static void
on_frame_clock_after_paint (GdkFrameClock * clock, gpointer data)
{
  struct AppData *d = data;

  if (d->geometry_changing) {
    g_print ("end geometry change\n");
    gst_wayland_video_end_geometry_change (d->wlvideo);
    d->geometry_changing = FALSE;
  }
}

static void
video_widget_realize_cb (GtkWidget * widget, gpointer data)
{
  struct AppData *d = data;
  GdkDisplay *display;
  GdkWindow *window;
  GdkFrameClock *frame_clock;

  display = gtk_widget_get_display (widget);
  window = gtk_widget_get_window (widget);

  /* Note that the surface passed to waylandsink here is the top-level
   * surface of the window, since gtk does not implement subsurfaces */
  d->display_handle = gdk_wayland_display_get_wl_display (display);
  d->window_handle = gdk_wayland_window_get_wl_surface (window);
  gtk_widget_get_allocation (widget, &d->video_widget_allocation);

  frame_clock = gtk_widget_get_frame_clock (widget);
  g_signal_connect_data (frame_clock, "after-paint",
      G_CALLBACK (on_frame_clock_after_paint), data, NULL, G_CONNECT_AFTER);
}

static gboolean
video_widget_draw_cb (GtkWidget * widget, cairo_t *cr, gpointer data)
{
  struct AppData *d = data;

  if (!d->window_handle)
    return FALSE;

  gtk_widget_get_allocation (widget, &d->video_widget_allocation);

  g_print ("draw_cb x %d, y %d, w %d, h %d\n",
      d->video_widget_allocation.x, d->video_widget_allocation.y,
      d->video_widget_allocation.width, d->video_widget_allocation.height);

  if (d->wlvideo && d->overlay && !d->geometry_changing) {
    gst_wayland_video_begin_geometry_change (d->wlvideo);
    d->geometry_changing = TRUE;

    gst_video_overlay_set_render_rectangle (d->overlay,
        d->video_widget_allocation.x, d->video_widget_allocation.y,
        d->video_widget_allocation.width, d->video_widget_allocation.height);
  }

  return FALSE;
}

static void
playing_clicked_cb (GtkButton *button, struct AppData * d)
{
  gst_element_set_state (d->pipeline, GST_STATE_PLAYING);
}

static void
paused_clicked_cb (GtkButton *button, struct AppData * d)
{
  gst_element_set_state (d->pipeline, GST_STATE_PAUSED);
}

static void
ready_clicked_cb (GtkButton *button, struct AppData * d)
{
  gst_element_set_state (d->pipeline, GST_STATE_READY);
}

static void
null_clicked_cb (GtkButton *button, struct AppData * d)
{
  gst_element_set_state (d->pipeline, GST_STATE_NULL);
}

static void
build_window (struct AppData * d)
{
  GtkBuilder *builder;
  GtkWidget *button;
  gchar *window_ui;
  GError *error = NULL;

  builder = gtk_builder_new ();
  window_ui = find_file ("window.ui");
  if (!gtk_builder_add_from_file (builder, window_ui, &error)) {
    g_error ("Failed to load window.ui: %s", error->message);
    g_error_free (error);
    goto exit;
  }

  d->app_window = GTK_WIDGET (gtk_builder_get_object (builder, "window"));
  g_object_ref (d->app_window);
  g_signal_connect (d->app_window, "destroy",
      G_CALLBACK (gtk_main_quit), NULL);

  d->video_window = GTK_WIDGET (gtk_builder_get_object (builder, "videoarea"));
  g_signal_connect (d->video_window, "realize",
      G_CALLBACK (video_widget_realize_cb), d);
  g_signal_connect (d->video_window, "draw",
      G_CALLBACK (video_widget_draw_cb), d);

  button = GTK_WIDGET (gtk_builder_get_object (builder, "button_playing"));
  g_signal_connect (button, "clicked", G_CALLBACK (playing_clicked_cb), d);

  button = GTK_WIDGET (gtk_builder_get_object (builder, "button_paused"));
  g_signal_connect (button, "clicked", G_CALLBACK (paused_clicked_cb), d);

  button = GTK_WIDGET (gtk_builder_get_object (builder, "button_ready"));
  g_signal_connect (button, "clicked", G_CALLBACK (ready_clicked_cb), d);

  button = GTK_WIDGET (gtk_builder_get_object (builder, "button_null"));
  g_signal_connect (button, "clicked", G_CALLBACK (null_clicked_cb), d);

exit:
  g_free (window_ui);
  g_object_unref (builder);
}

int
main (int argc, char **argv)
{
  struct AppData data = {0};
  GstBus *bus;

  gdk_set_allowed_backends ("wayland");

  gtk_init (&argc, &argv);
  gst_init (&argc, &argv);

  // create the window
  build_window (&data);

  // show the GUI
  gtk_widget_show_all (data.app_window);

  // realize window now so that the video window gets created and we can
  // obtain its window handle before the pipeline is started up and the video
  // sink asks for the handle of the window to render onto
  gtk_widget_realize (data.app_window);

  // we should have the handle now
  g_assert (data.window_handle != 0);

  if (argc > 1) {
    data.argv = argv;
    data.current_uri = 1;

    data.pipeline = gst_parse_launch ("playbin video-sink=waylandsink", NULL);
    g_object_set (data.pipeline, "uri", argv[data.current_uri], NULL);

    // enable looping
    g_signal_connect (data.pipeline, "about-to-finish",
        G_CALLBACK (on_about_to_finish), &data);
  } else {
    data.pipeline = gst_parse_launch (
        "videotestsrc pattern=18 background-color=0x0000F000 "
        //"v4l2src ! videoconvert "
        "! waylandsink", NULL);
  }

  // set up sync handler for setting the xid once the pipeline is started
  bus = gst_pipeline_get_bus (GST_PIPELINE (data.pipeline));
  gst_bus_set_sync_handler (bus, (GstBusSyncHandler) bus_sync_handler, &data,
      NULL);
  gst_object_unref (bus);

  // play
  gst_element_set_state (data.pipeline, GST_STATE_PLAYING);
  gtk_main ();
  gst_element_set_state (data.pipeline, GST_STATE_NULL);

  gst_object_unref (data.pipeline);

  return 0;
}
