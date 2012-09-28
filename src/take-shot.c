/* GFreenect Utils : take-shot.c
 *
 * Copyright (c) 2012 Igalia, S.L.
 *
 * Author: Joaquim Rocha <jrocha@igalia.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <gfreenect.h>
#include <math.h>
#include <string.h>
#include <glib-object.h>
#include <clutter/clutter.h>
#include <clutter/clutter-keysyms.h>

static GFreenectDevice *kinect = NULL;
static ClutterActor *info_text;
static ClutterActor *depth_tex;
static ClutterActor *video_tex;
static ClutterActor *instructions;

static guint THRESHOLD_BEGIN = 500;
/* Adjust this value to increase of decrease
   the threshold */
static guint THRESHOLD_END   = 8000;

static guint shot_timeout_id = 0;
static gboolean record_shot = FALSE;
static gint DEFAULT_SECONDS_TO_SHOOT = 2;
static gint seconds_to_shoot = 2;
static gint seconds_to_record = 5;
static GTimer *timer;

static gint width = 640;
static gint height = 480;

static gchar *directory_name;

static gboolean VERTICAL = FALSE;

typedef struct
{
  guint16 *reduced_buffer;
  gint width;
  gint height;
  gint reduced_width;
  gint reduced_height;
} BufferInfo;

static void
set_orientation ()
{
  ClutterActor *stage;

  stage = clutter_stage_get_default ();

  clutter_actor_set_size (video_tex, width, height);
  clutter_actor_set_size (depth_tex, width, height);
  clutter_cairo_texture_set_surface_size (CLUTTER_CAIRO_TEXTURE (depth_tex), width, height);
  clutter_cairo_texture_set_surface_size (CLUTTER_CAIRO_TEXTURE (video_tex), width, height);
  clutter_actor_set_size (stage, width * 2, height + 250);
  clutter_actor_set_position (video_tex, width, 0.0);
  clutter_actor_set_position (info_text, 50, height + 20);
  clutter_actor_set_position (instructions, 50, height + 70);
}

static void
grayscale_buffer_set_value (guchar *buffer, gint index, guchar value)
{
  buffer[index * 3] = value;
  buffer[index * 3 + 1] = value;
  buffer[index * 3 + 2] = value;
}

static BufferInfo *
process_buffer (guint16 *buffer,
                guint width,
                guint height,
                guint dimension_factor,
                guint threshold_begin,
                guint threshold_end)
{
  BufferInfo *buffer_info;
  gint i, j, reduced_width, reduced_height;
  guint16 *reduced_buffer;

  g_return_val_if_fail (buffer != NULL, NULL);

  reduced_width = (width - width % dimension_factor) / dimension_factor;
  reduced_height = (height - height % dimension_factor) / dimension_factor;

  reduced_buffer = g_slice_alloc0 (reduced_width * reduced_height *
                                   sizeof (guint16));

  for (i = 0; i < reduced_width; i++)
    {
      for (j = 0; j < reduced_height; j++)
        {
          gint index;
          guint16 value;

          index = j * width * dimension_factor + i * dimension_factor;
          value = buffer[index];

          if (value < threshold_begin || value > threshold_end)
            {
              reduced_buffer[j * reduced_width + i] = 0;
              continue;
            }

          reduced_buffer[j * reduced_width + i] = value;
        }
    }

  buffer_info = g_slice_new0 (BufferInfo);
  buffer_info->reduced_buffer = reduced_buffer;
  buffer_info->reduced_width = reduced_width;
  buffer_info->reduced_height = reduced_height;
  buffer_info->width = width;
  buffer_info->height = height;

  return buffer_info;
}

static guchar *
create_grayscale_buffer (BufferInfo *buffer_info, gint dimension_reduction)
{
  gint i,j;
  gint size;
  guchar *grayscale_buffer;
  guint16 *reduced_buffer;

  reduced_buffer = buffer_info->reduced_buffer;

  size = buffer_info->width * buffer_info->height * sizeof (guchar) * 3;
  grayscale_buffer = g_slice_alloc (size);
  /*Paint is white*/
  memset (grayscale_buffer, 255, size);

  for (i = 0; i < buffer_info->reduced_width; i++)
    {
      for (j = 0; j < buffer_info->reduced_height; j++)
        {
          if (reduced_buffer[j * buffer_info->reduced_width + i] != 0)
            {
              gint index = j * dimension_reduction * buffer_info->width +
                i * dimension_reduction;
              grayscale_buffer_set_value (grayscale_buffer, index, 0);
            }
        }
    }

  return grayscale_buffer;
}

static guint16 *
read_file_to_buffer (gchar *name, gsize count, GError *e)
{
  GError *error = NULL;
  guint16 *depth = NULL;
  GFile *new_file = g_file_new_for_path (name);
  GFileInputStream *input_stream = g_file_read (new_file,
                                                NULL,
                                                &error);
  if (error != NULL)
    {
      g_debug ("ERROR: %s", error->message);
    }
  else
    {
      gsize bread = 0;
      depth = g_slice_alloc (count);
      g_input_stream_read_all ((GInputStream *) input_stream,
                               depth,
                               count,
                               &bread,
                               NULL,
                               &error);

      if (error != NULL)
        {
          g_debug ("ERROR: %s", error->message);
        }
    }
  return depth;
}

static void
on_depth_frame (GFreenectDevice *kinect, gpointer user_data)
{
  gint width, height;
  guchar *grayscale_buffer;
  guint16 *depth;
  guint16 *transformed_depth;
  gchar *contents;
  BufferInfo *buffer_info;
  gsize len;
  GError *error = NULL;
  GFreenectFrameMode frame_mode;

  depth = (guint16 *) gfreenect_device_get_depth_frame_raw (kinect,
                                                            &len,
                                                            &frame_mode);
  if (error != NULL)
    {
      g_debug ("ERROR Opening: %s", error->message);
    }

  width = frame_mode.width;
  height = frame_mode.height;
  if (VERTICAL)
    {
      guint i, j;

      transformed_depth = g_slice_alloc (width * height * sizeof (guint16));

      for (j = 0; j < width; j++)
        {
          for (i = height - 1; i > 0; i--)
            {
              transformed_depth[((width -1 - j) * height + ((height - 1) - i))] = depth[i * width + j];
            }
        }

      depth = transformed_depth;

      width = frame_mode.height;
      height = frame_mode.width;
    }

  buffer_info = process_buffer (depth,
                                width,
                                height,
                                1,
                                THRESHOLD_BEGIN,
                                THRESHOLD_END);

  grayscale_buffer = create_grayscale_buffer (buffer_info,
                                              1);

  g_slice_free1 (sizeof (guint16) * width * height, transformed_depth);

  if (record_shot)
    {
      g_debug ("Taking video...");
      GError *error = NULL;
      gchar *name = g_strdup_printf ("depth-data-%ld", g_get_real_time ());
      name = g_strconcat (directory_name, "/", name, NULL);
      g_printf ("name = %s\n", name);
      g_file_set_contents (name, (gchar *) buffer_info->reduced_buffer,
                           width * height * sizeof (guint16), &error);
      if (error != NULL)
        {
          g_debug ("ERROR: %s", error->message);
        }
      else
        {
          g_print ("Created file: %s\n", name);
        }
      if (g_timer_elapsed (timer, NULL) >= seconds_to_record)
        {
          record_shot = FALSE;
          g_print ("Recording stopped\n");
        }
    }

  if (! clutter_texture_set_from_rgb_data (CLUTTER_TEXTURE (depth_tex),
                                           grayscale_buffer,
                                           FALSE,
                                           width, height,
                                           0,
                                           3,
                                           CLUTTER_TEXTURE_NONE,
                                           &error))
    {
      g_debug ("Error setting texture area: %s", error->message);
      g_error_free (error);
    }
  g_slice_free1 (width * height * sizeof (guchar) * 3, grayscale_buffer);
  g_slice_free1 (buffer_info->reduced_width * buffer_info->reduced_height *
                 sizeof (guint16), buffer_info->reduced_buffer);
  g_slice_free (BufferInfo, buffer_info);
}

static void
on_video_frame (GFreenectDevice *kinect, gpointer user_data)
{
  guchar *buffer;
  guchar *transformed_buffer;
  guint width, height;
  GError *error = NULL;
  GFreenectFrameMode frame_mode;

  buffer = gfreenect_device_get_video_frame_rgb (kinect, NULL, &frame_mode);

  width = frame_mode.width;
  height = frame_mode.height;

  if (VERTICAL)
    {
      guint i, j;

      transformed_buffer = g_slice_alloc (sizeof (gchar) * width * height * 3);

      for (j = 0; j < width; j++)
        {
          for (i = height - 1; i > 0; i--)
            {
              transformed_buffer[((width -1 - j) * height + ((height - 1) - i)) * 3] = buffer[(i * width + j) * 3];
              transformed_buffer[((width -1 - j) * height + ((height - 1) - i)) * 3 + 1] = buffer[(i * width + j) * 3 + 1];
              transformed_buffer[((width -1 - j) * height + ((height - 1) - i)) * 3 + 2] = buffer[(i * width + j) * 3 + 2];
            }
        }

      buffer = transformed_buffer;
      width = frame_mode.height;
      height = frame_mode.width;
    }


  if (! clutter_texture_set_from_rgb_data (CLUTTER_TEXTURE (video_tex),
                                           buffer,
                                           FALSE,
                                           width, height,
                                           0,
                                           frame_mode.bits_per_pixel / 8,
                                           CLUTTER_TEXTURE_NONE,
                                           &error))
    {
      g_debug ("Error setting texture area: %s", error->message);
      g_error_free (error);
    }

  g_slice_free1 (sizeof (gchar) * width * height * 3, transformed_buffer);
}

static void
set_info_text (gint seconds)
{
  gchar *title, *threshold;
  gchar *record_status = NULL;
  threshold = g_strdup_printf ("<b>Threshold:</b> %d",
                               THRESHOLD_END);
  if (seconds == 0)
    {
      record_status = g_strdup (" <b>SAVING DEPTH FILE!</b>");
    }
  else if (seconds > 0)
    {
      record_status = g_strdup_printf (" <b>Taking video in:</b> %d seconds",
                                       seconds);
    }

  title = g_strconcat (threshold, record_status, NULL);
  clutter_text_set_markup (CLUTTER_TEXT (info_text), title);
  g_free (title);
  g_free (threshold);
  g_free (record_status);
}

static void
set_threshold (gint difference)
{
  gint new_threshold = THRESHOLD_END + difference;
  if (new_threshold >= THRESHOLD_BEGIN + 300 &&
      new_threshold <= 8000)
    THRESHOLD_END = new_threshold;
}

static void
set_tilt_angle (GFreenectDevice *kinect, gdouble difference)
{
  GError *error = NULL;
  gdouble angle;
  angle = gfreenect_device_get_tilt_angle_sync (kinect, NULL, &error);
  if (error != NULL)
    {
      g_error_free (error);
      return;
    }

  if (angle >= -31 && angle <= 31)
    gfreenect_device_set_tilt_angle (kinect,
                                     angle + difference,
                                     NULL,
                                     NULL,
                                     NULL);
}

static gboolean
decrease_time_to_take_shot (gpointer data)
{
  gboolean call_again = TRUE;
  set_info_text (seconds_to_shoot);
  if (seconds_to_shoot < 0)
    {
      seconds_to_shoot = DEFAULT_SECONDS_TO_SHOOT;
      call_again = FALSE;
    }
  else if (seconds_to_shoot == 0)
    {
      record_shot = TRUE;
      g_timer_start (timer);
    }
  seconds_to_shoot--;
  return call_again;
}

static void
take_shot (void)
{
  if (shot_timeout_id != 0)
    {
      g_source_remove (shot_timeout_id);
      seconds_to_shoot = DEFAULT_SECONDS_TO_SHOOT;
    }
  shot_timeout_id = g_timeout_add_seconds (1,
                                           decrease_time_to_take_shot,
                                           NULL);
}

static gboolean
on_key_press (ClutterActor *actor,
                ClutterEvent *event,
                gpointer data)
{
  GFreenectDevice *kinect;
  gint seconds = -1;
  gint aux;
  gdouble angle;
  guint key;
  g_return_val_if_fail (event != NULL, FALSE);

  kinect = GFREENECT_DEVICE (data);

  key = clutter_event_get_key_symbol (event);
  switch (key)
    {
    case CLUTTER_KEY_o:
      VERTICAL = !VERTICAL;
      aux = width;
      width = height;
      height = aux;
      set_orientation ();
      break;
    case CLUTTER_KEY_space:
      seconds = 3;
      take_shot ();
      break;
    case CLUTTER_KEY_plus:
      set_threshold (100);
      break;
    case CLUTTER_KEY_minus:
      set_threshold (-100);
      break;
    case CLUTTER_KEY_Up:
      set_tilt_angle (kinect, 5);
      break;
    case CLUTTER_KEY_Down:
      set_tilt_angle (kinect, -5);
      break;
    }
  set_info_text (seconds);
  return TRUE;
}

static ClutterActor *
create_instructions (void)
{
  ClutterActor *text;

  text = clutter_text_new ();
  clutter_text_set_markup (CLUTTER_TEXT (text),
                         "<b>Instructions:</b>\n"
                         "\tTake shot and save:  \tSpace bar\n"
                         "\tSet tilt angle:  \t\t\t\tUp/Down Arrows\n"
                         "\tIncrease threshold:  \t\t\t+/-\n"
                         "\tSwitch orientation: \t\t\to");
  return text;
}

static void
on_destroy (ClutterActor *actor, gpointer data)
{
  GFreenectDevice *device = GFREENECT_DEVICE (data);
  gfreenect_device_stop_depth_stream (device, NULL);
  gfreenect_device_stop_video_stream (device, NULL);
  clutter_main_quit ();
}

static void
on_new_kinect_device (GObject      *obj,
                      GAsyncResult *res,
                      gpointer      user_data)
{
  ClutterActor *stage;
  GError *error = NULL;

  kinect = gfreenect_device_new_finish (res, &error);
  if (kinect == NULL)
    {
      g_debug ("Failed to created kinect device: %s", error->message);
      g_error_free (error);
      clutter_main_quit ();
      return;
    }

  g_debug ("Kinect device created!");

  stage = clutter_stage_get_default ();
  clutter_stage_set_title (CLUTTER_STAGE (stage), "Kinect Test");
  clutter_stage_set_user_resizable (CLUTTER_STAGE (stage), TRUE);

  g_signal_connect (stage, "destroy", G_CALLBACK (on_destroy), kinect);
  g_signal_connect (stage,
                    "key-press-event",
                    G_CALLBACK (on_key_press),
                    kinect);

  depth_tex = clutter_cairo_texture_new (width, height);
  clutter_container_add_actor (CLUTTER_CONTAINER (stage), depth_tex);

  video_tex = clutter_cairo_texture_new (width, height);
  clutter_container_add_actor (CLUTTER_CONTAINER (stage), video_tex);

  info_text = clutter_text_new ();
  set_info_text (-1);
  clutter_container_add_actor (CLUTTER_CONTAINER (stage), info_text);

  instructions = create_instructions ();
  clutter_container_add_actor (CLUTTER_CONTAINER (stage), instructions);

  set_orientation ();

  clutter_actor_show_all (stage);

  g_signal_connect (kinect,
                    "depth-frame",
                    G_CALLBACK (on_depth_frame),
                    NULL);

  g_signal_connect (kinect,
                    "video-frame",
                    G_CALLBACK (on_video_frame),
                    NULL);

  gfreenect_device_set_tilt_angle (kinect, 0, NULL, NULL, NULL);

  gfreenect_device_start_depth_stream (kinect,
                                       GFREENECT_DEPTH_FORMAT_MM,
                                       NULL);

  gfreenect_device_start_video_stream (kinect,
                                       GFREENECT_RESOLUTION_MEDIUM,
                                       GFREENECT_VIDEO_FORMAT_RGB, NULL);
}

static void
quit (gint signale)
{
  signal (SIGINT, 0);

  clutter_main_quit ();
}

int
main (int argc, char *argv[])
{
  if (clutter_init (&argc, &argv) != CLUTTER_INIT_SUCCESS)
    return -1;

  gfreenect_device_new (0,
                        GFREENECT_SUBDEVICE_CAMERA,
                        NULL,
                        on_new_kinect_device,
                        NULL);

  signal (SIGINT, quit);

  if (argc < 2)
    {
      g_printf ("Use: %s VIDEO_DIRECTORY\n", argv[0]);
      return 0;
    }

  directory_name = argv[1];

  if (g_mkdir (directory_name, 0750) != 0)
    {
      g_printf ("Error creating directory %s\n", directory_name);
      return -1;
    }

  timer = g_timer_new ();

  clutter_main ();

  if (kinect != NULL)
    g_object_unref (kinect);

  return 0;
}

