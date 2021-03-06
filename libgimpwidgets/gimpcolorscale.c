/* LIBGIMP - The GIMP Library
 * Copyright (C) 1995-1997 Peter Mattis and Spencer Kimball
 *
 * gimpcolorscale.c
 * Copyright (C) 2002-2010  Sven Neumann <sven@gimp.org>
 *                          Michael Natterer <mitch@gimp.org>
 *
 * This library is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <string.h>

#include <gegl.h>
#include <gtk/gtk.h>

#include "libgimpbase/gimpbase.h"
#include "libgimpconfig/gimpconfig.h"
#include "libgimpcolor/gimpcolor.h"

#include "gimpwidgetstypes.h"

#include "gimpcairo-utils.h"
#include "gimpcolorscale.h"
#include "gimpwidgetsutils.h"


/**
 * SECTION: gimpcolorscale
 * @title: GimpColorScale
 * @short_description: Fancy colored sliders.
 *
 * Fancy colored sliders.
 **/


enum
{
  PROP_0,
  PROP_CHANNEL
};


typedef struct _GimpLCH  GimpLCH;

struct _GimpLCH
{
  gdouble l, c, h, a;
};


typedef struct _GimpColorScalePrivate GimpColorScalePrivate;

struct _GimpColorScalePrivate
{
  GimpColorConfig    *config;
  GimpColorTransform *transform;
  guchar              oog_color[3];
};

#define GET_PRIVATE(obj) \
        ((GimpColorScalePrivate *) gimp_color_scale_get_instance_private ((GimpColorScale *) (obj)))


static void     gimp_color_scale_dispose           (GObject          *object);
static void     gimp_color_scale_finalize          (GObject          *object);
static void     gimp_color_scale_get_property      (GObject          *object,
                                                    guint             property_id,
                                                    GValue           *value,
                                                    GParamSpec       *pspec);
static void     gimp_color_scale_set_property      (GObject          *object,
                                                    guint             property_id,
                                                    const GValue     *value,
                                                    GParamSpec       *pspec);

static void     gimp_color_scale_size_allocate     (GtkWidget        *widget,
                                                    GtkAllocation    *allocation);
static void     gimp_color_scale_state_changed     (GtkWidget        *widget,
                                                    GtkStateType      previous_state);
static gboolean gimp_color_scale_button_press      (GtkWidget        *widget,
                                                    GdkEventButton   *event);
static gboolean gimp_color_scale_button_release    (GtkWidget        *widget,
                                                    GdkEventButton   *event);
static gboolean gimp_color_scale_scroll            (GtkWidget        *widget,
                                                    GdkEventScroll   *event);
static gboolean gimp_color_scale_expose            (GtkWidget        *widget,
                                                    GdkEventExpose   *event);

static void     gimp_color_scale_render            (GimpColorScale   *scale);
static void     gimp_color_scale_render_alpha      (GimpColorScale   *scale);
static void     gimp_color_scale_render_stipple    (GimpColorScale   *scale);

static void     gimp_color_scale_create_transform  (GimpColorScale   *scale);
static void     gimp_color_scale_destroy_transform (GimpColorScale   *scale);
static void     gimp_color_scale_notify_config     (GimpColorConfig  *config,
                                                    const GParamSpec *pspec,
                                                    GimpColorScale   *scale);


G_DEFINE_TYPE_WITH_PRIVATE (GimpColorScale, gimp_color_scale, GTK_TYPE_SCALE)

#define parent_class gimp_color_scale_parent_class

static const Babl *fish_rgb_to_lch = NULL;
static const Babl *fish_lch_to_rgb = NULL;


static void
gimp_color_scale_class_init (GimpColorScaleClass *klass)
{
  GObjectClass   *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose              = gimp_color_scale_dispose;
  object_class->finalize             = gimp_color_scale_finalize;
  object_class->get_property         = gimp_color_scale_get_property;
  object_class->set_property         = gimp_color_scale_set_property;

  widget_class->size_allocate        = gimp_color_scale_size_allocate;
  widget_class->state_changed        = gimp_color_scale_state_changed;
  widget_class->button_press_event   = gimp_color_scale_button_press;
  widget_class->button_release_event = gimp_color_scale_button_release;
  widget_class->scroll_event         = gimp_color_scale_scroll;
  widget_class->expose_event         = gimp_color_scale_expose;

  /**
   * GimpColorScale:channel:
   *
   * The channel which is edited by the color scale.
   *
   * Since: 2.8
   */
  g_object_class_install_property (object_class, PROP_CHANNEL,
                                   g_param_spec_enum ("channel",
                                                      "Channel",
                                                      "The channel which is edited by the color scale",
                                                      GIMP_TYPE_COLOR_SELECTOR_CHANNEL,
                                                      GIMP_COLOR_SELECTOR_VALUE,
                                                      GIMP_PARAM_READWRITE |
                                                      G_PARAM_CONSTRUCT));

  fish_rgb_to_lch = babl_fish (babl_format ("R'G'B'A double"),
                               babl_format ("CIE LCH(ab) double"));
  fish_lch_to_rgb = babl_fish (babl_format ("CIE LCH(ab) double"),
                               babl_format ("R'G'B' double"));
}

static void
gimp_color_scale_dispose (GObject *object)
{
  GimpColorScale *scale = GIMP_COLOR_SCALE (object);

  gimp_color_scale_set_color_config (scale, NULL);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gimp_color_scale_init (GimpColorScale *scale)
{
  GtkRange *range = GTK_RANGE (scale);

  gtk_range_set_slider_size_fixed (range, TRUE);
  gtk_range_set_flippable (GTK_RANGE (scale), TRUE);

  gtk_scale_set_draw_value (GTK_SCALE (scale), FALSE);

  scale->channel      = GIMP_COLOR_SELECTOR_VALUE;
  scale->needs_render = TRUE;

  gtk_orientable_set_orientation (GTK_ORIENTABLE (range),
                                  GTK_ORIENTATION_HORIZONTAL);

  gimp_rgba_set (&scale->rgb, 0.0, 0.0, 0.0, 1.0);
  gimp_rgb_to_hsv (&scale->rgb, &scale->hsv);

  gimp_widget_track_monitor (GTK_WIDGET (scale),
                             G_CALLBACK (gimp_color_scale_destroy_transform),
                             NULL);
}

static void
gimp_color_scale_finalize (GObject *object)
{
  GimpColorScale *scale = GIMP_COLOR_SCALE (object);

  g_clear_pointer (&scale->buf, g_free);
  scale->width     = 0;
  scale->height    = 0;
  scale->rowstride = 0;

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gimp_color_scale_get_property (GObject    *object,
                               guint       property_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  GimpColorScale *scale = GIMP_COLOR_SCALE (object);

  switch (property_id)
    {
    case PROP_CHANNEL:
      g_value_set_enum (value, scale->channel);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
gimp_color_scale_set_property (GObject      *object,
                               guint         property_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  GimpColorScale *scale = GIMP_COLOR_SCALE (object);

  switch (property_id)
    {
    case PROP_CHANNEL:
      gimp_color_scale_set_channel (scale, g_value_get_enum (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
gimp_color_scale_size_allocate (GtkWidget     *widget,
                                GtkAllocation *allocation)
{
  GimpColorScale *scale = GIMP_COLOR_SCALE (widget);
  GtkRange       *range = GTK_RANGE (widget);
  GdkRectangle    range_rect;
  gint            focus = 0;
  gint            trough_border;
  gint            scale_width;
  gint            scale_height;

  gtk_widget_style_get (widget,
                        "trough-border", &trough_border,
                        NULL);

  if (gtk_widget_get_can_focus (widget))
    {
      gint focus_padding = 0;

      gtk_widget_style_get (widget,
                            "focus-line-width", &focus,
                            "focus-padding",    &focus_padding,
                            NULL);
      focus += focus_padding;
    }

  gtk_range_set_min_slider_size (range,
                                 (MIN (allocation->width,
                                       allocation->height) - 2 * focus) / 2);

  if (GTK_WIDGET_CLASS (parent_class)->size_allocate)
    GTK_WIDGET_CLASS (parent_class)->size_allocate (widget, allocation);

  gtk_range_get_range_rect (range, &range_rect);

  scale_width  = range_rect.width  - 2 * (focus + trough_border);
  scale_height = range_rect.height - 2 * (focus + trough_border);

  switch (gtk_orientable_get_orientation (GTK_ORIENTABLE (range)))
    {
    case GTK_ORIENTATION_HORIZONTAL:
      scale_width  -= gtk_range_get_min_slider_size (range) - 1;
      scale_height -= 2;
      break;

    case GTK_ORIENTATION_VERTICAL:
      scale_width  -= 2;
      scale_height -= gtk_range_get_min_slider_size (range) - 1;
      break;
    }

  if (scale_width != scale->width || scale_height != scale->height)
    {
      scale->width  = scale_width;
      scale->height = scale_height;

      scale->rowstride = scale->width * 4;

      g_free (scale->buf);
      scale->buf = g_new (guchar, scale->rowstride * scale->height);

      scale->needs_render = TRUE;
    }
}

static void
gimp_color_scale_state_changed (GtkWidget    *widget,
                                GtkStateType  previous_state)
{
  if (gtk_widget_get_state (widget) == GTK_STATE_INSENSITIVE ||
      previous_state == GTK_STATE_INSENSITIVE)
    {
      GIMP_COLOR_SCALE (widget)->needs_render = TRUE;
    }

  if (GTK_WIDGET_CLASS (parent_class)->state_changed)
    GTK_WIDGET_CLASS (parent_class)->state_changed (widget, previous_state);
}

static gboolean
gimp_color_scale_button_press (GtkWidget      *widget,
                               GdkEventButton *event)
{
  if (event->button == 1)
    {
      GdkEventButton *my_event;
      gboolean        retval;

      my_event = (GdkEventButton *) gdk_event_copy ((GdkEvent *) event);
      my_event->button = 2;

      retval = GTK_WIDGET_CLASS (parent_class)->button_press_event (widget,
                                                                    my_event);

      gdk_event_free ((GdkEvent *) my_event);

      return retval;
    }

  return GTK_WIDGET_CLASS (parent_class)->button_press_event (widget, event);
}

static gboolean
gimp_color_scale_button_release (GtkWidget      *widget,
                                 GdkEventButton *event)
{
  if (event->button == 1)
    {
      GdkEventButton *my_event;
      gboolean        retval;

      my_event = (GdkEventButton *) gdk_event_copy ((GdkEvent *) event);
      my_event->button = 2;

      retval = GTK_WIDGET_CLASS (parent_class)->button_release_event (widget,
                                                                      my_event);

      gdk_event_free ((GdkEvent *) my_event);

      return retval;
    }

  return GTK_WIDGET_CLASS (parent_class)->button_release_event (widget, event);
}

static gboolean
gimp_color_scale_scroll (GtkWidget      *widget,
                         GdkEventScroll *event)
{
  if (gtk_orientable_get_orientation (GTK_ORIENTABLE (widget)) ==
      GTK_ORIENTATION_HORIZONTAL)
    {
      GdkEventScroll *my_event;
      gboolean        retval;

      my_event = (GdkEventScroll *) gdk_event_copy ((GdkEvent *) event);

      switch (my_event->direction)
        {
        case GDK_SCROLL_UP:
          my_event->direction = GDK_SCROLL_RIGHT;
          break;

        case GDK_SCROLL_DOWN:
          my_event->direction = GDK_SCROLL_LEFT;
          break;

        default:
          break;
        }

      retval = GTK_WIDGET_CLASS (parent_class)->scroll_event (widget, my_event);

      gdk_event_free ((GdkEvent *) my_event);

      return retval;
    }

  return GTK_WIDGET_CLASS (parent_class)->scroll_event (widget, event);
}

static gboolean
gimp_color_scale_expose (GtkWidget      *widget,
                         GdkEventExpose *event)
{
  GimpColorScale        *scale     = GIMP_COLOR_SCALE (widget);
  GimpColorScalePrivate *priv      = GET_PRIVATE (widget);
  GtkRange              *range     = GTK_RANGE (widget);
  GtkStyle              *style     = gtk_widget_get_style (widget);
  GdkWindow             *window    = gtk_widget_get_window (widget);
  gboolean               sensitive = gtk_widget_is_sensitive (widget);
  GtkAllocation          allocation;
  GdkRectangle           range_rect;
  GdkRectangle           area      = { 0, };
  cairo_surface_t       *buffer;
  gint                   focus = 0;
  gint                   trough_border;
  gint                   slider_start;
  gint                   slider_size;
  gint                   x, y;
  gint                   w, h;
  cairo_t               *cr;

  if (! scale->buf || ! gtk_widget_is_drawable (widget))
    return FALSE;

  gtk_widget_get_allocation (widget, &allocation);

  cr = gdk_cairo_create (window);
  gdk_cairo_region (cr, event->region);
  cairo_translate (cr, allocation.x, allocation.y);
  cairo_clip (cr);

  gtk_widget_style_get (widget,
                        "trough-border", &trough_border,
                        NULL);

  if (gtk_widget_get_can_focus (widget))
    {
      gint focus_padding = 0;

      gtk_widget_style_get (widget,
                            "focus-line-width", &focus,
                            "focus-padding",    &focus_padding,
                            NULL);
      focus += focus_padding;
    }

  gtk_range_get_range_rect (range, &range_rect);
  gtk_range_get_slider_range (range, &slider_start, NULL);

  x = range_rect.x + focus;
  y = range_rect.y + focus;
  w = range_rect.width  - 2 * focus;
  h = range_rect.height - 2 * focus;

  slider_size = gtk_range_get_min_slider_size (range) / 2;

  if (scale->needs_render)
    {
      gimp_color_scale_render (scale);

      if (! sensitive)
        gimp_color_scale_render_stipple (scale);

      scale->needs_render = FALSE;
    }

  gtk_paint_box (style, window,
                 sensitive ? GTK_STATE_ACTIVE : GTK_STATE_INSENSITIVE,
                 GTK_SHADOW_IN,
                 &event->area, widget, "trough",
                 x + allocation.x,
                 y + allocation.y,
                 w, h);

  if (! priv->transform)
    gimp_color_scale_create_transform (scale);

  if (priv->transform)
    {
      const Babl *format = babl_format ("cairo-RGB24");
      guchar     *buf    = g_new (guchar, scale->rowstride * scale->height);
      guchar     *src    = scale->buf;
      guchar     *dest   = buf;
      gint        i;

      for (i = 0; i < scale->height; i++)
        {
          gimp_color_transform_process_pixels (priv->transform,
                                               format, src,
                                               format, dest,
                                               scale->width);

          src  += scale->rowstride;
          dest += scale->rowstride;
        }

      buffer = cairo_image_surface_create_for_data (buf,
                                                    CAIRO_FORMAT_RGB24,
                                                    scale->width,
                                                    scale->height,
                                                    scale->rowstride);
      cairo_surface_set_user_data (buffer, NULL,
                                   buf, (cairo_destroy_func_t) g_free);
    }
  else
    {
      buffer = cairo_image_surface_create_for_data (scale->buf,
                                                    CAIRO_FORMAT_RGB24,
                                                    scale->width,
                                                    scale->height,
                                                    scale->rowstride);
    }

  switch (gtk_orientable_get_orientation (GTK_ORIENTABLE (range)))
    {
    case GTK_ORIENTATION_HORIZONTAL:
      cairo_set_source_surface (cr, buffer,
                                x + trough_border + slider_size,
                                y + trough_border + 1);
      break;

    case GTK_ORIENTATION_VERTICAL:
      cairo_set_source_surface (cr, buffer,
                                x + trough_border + 1,
                                y + trough_border + slider_size);
      break;
    }

  cairo_surface_destroy (buffer);
  cairo_paint (cr);

  if (gtk_widget_has_focus (widget))
    gtk_paint_focus (style, window, gtk_widget_get_state (widget),
                     &event->area, widget, "trough",
                     range_rect.x + allocation.x,
                     range_rect.y + allocation.y,
                     range_rect.width,
                     range_rect.height);

  switch (gtk_orientable_get_orientation (GTK_ORIENTABLE (range)))
    {
    case GTK_ORIENTATION_HORIZONTAL:
      area.x      = slider_start;
      area.y      = y + trough_border;
      area.width  = 2 * slider_size + 1;
      area.height = h - 2 * trough_border;
      break;

    case GTK_ORIENTATION_VERTICAL:
      area.x      = x + trough_border;
      area.y      = slider_start;
      area.width  = w - 2 * trough_border;
      area.height = 2 * slider_size + 1;
      break;
    }

  if (gtk_widget_is_sensitive (widget))
    gdk_cairo_set_source_color (cr, &style->black);
  else
    gdk_cairo_set_source_color (cr, &style->dark[GTK_STATE_INSENSITIVE]);

  switch (gtk_orientable_get_orientation (GTK_ORIENTABLE (range)))
    {
    case GTK_ORIENTATION_HORIZONTAL:
      cairo_move_to (cr, area.x, area.y);
      cairo_line_to (cr, area.x + area.width, area.y);
      cairo_line_to (cr,
                     area.x + area.width / 2 + 0.5,
                     area.y + area.width / 2);
      break;

    case GTK_ORIENTATION_VERTICAL:
      cairo_move_to (cr, area.x, area.y);
      cairo_line_to (cr, area.x, area.y + area.height);
      cairo_line_to (cr,
                     area.x + area.height / 2,
                     area.y + area.height / 2 + 0.5);
      break;
    }

  cairo_close_path (cr);
  cairo_fill (cr);

  if (gtk_widget_is_sensitive (widget))
    gdk_cairo_set_source_color (cr, &style->white);
  else
    gdk_cairo_set_source_color (cr, &style->light[GTK_STATE_INSENSITIVE]);

  switch (gtk_orientable_get_orientation (GTK_ORIENTABLE (range)))
    {
    case GTK_ORIENTATION_HORIZONTAL:
      cairo_move_to (cr, area.x, area.y + area.height);
      cairo_line_to (cr, area.x + area.width, area.y + area.height);
      cairo_line_to (cr,
                     area.x + area.width / 2 + 0.5,
                     area.y + area.height - area.width / 2);
      break;

    case GTK_ORIENTATION_VERTICAL:
      cairo_move_to (cr, area.x + area.width, area.y);
      cairo_line_to (cr, area.x + area.width, area.y + area.height);
      cairo_line_to (cr,
                     area.x + area.width - area.height / 2,
                     area.y + area.height / 2 + 0.5);
      break;
    }

  cairo_close_path (cr);
  cairo_fill (cr);

  cairo_destroy (cr);

  return FALSE;
}

/**
 * gimp_color_scale_new:
 * @orientation: the scale's orientation (horizontal or vertical)
 * @channel: the scale's color channel
 *
 * Creates a new #GimpColorScale widget.
 *
 * Return value: a new #GimpColorScale widget
 **/
GtkWidget *
gimp_color_scale_new (GtkOrientation            orientation,
                      GimpColorSelectorChannel  channel)
{
  GimpColorScale *scale = g_object_new (GIMP_TYPE_COLOR_SCALE,
                                        "orientation", orientation,
                                        "channel",     channel,
                                        NULL);

  gtk_range_set_flippable (GTK_RANGE (scale),
                           orientation == GTK_ORIENTATION_HORIZONTAL);

  return GTK_WIDGET (scale);
}

/**
 * gimp_color_scale_set_channel:
 * @scale: a #GimpColorScale widget
 * @channel: the new color channel
 *
 * Changes the color channel displayed by the @scale.
 **/
void
gimp_color_scale_set_channel (GimpColorScale           *scale,
                              GimpColorSelectorChannel  channel)
{
  g_return_if_fail (GIMP_IS_COLOR_SCALE (scale));

  if (channel != scale->channel)
    {
      scale->channel = channel;

      scale->needs_render = TRUE;
      gtk_widget_queue_draw (GTK_WIDGET (scale));

      g_object_notify (G_OBJECT (scale), "channel");
    }
}

/**
 * gimp_color_scale_set_color:
 * @scale: a #GimpColorScale widget
 * @rgb: the new color as #GimpRGB
 * @hsv: the new color as #GimpHSV
 *
 * Changes the color value of the @scale.
 **/
void
gimp_color_scale_set_color (GimpColorScale *scale,
                            const GimpRGB  *rgb,
                            const GimpHSV  *hsv)
{
  g_return_if_fail (GIMP_IS_COLOR_SCALE (scale));
  g_return_if_fail (rgb != NULL);
  g_return_if_fail (hsv != NULL);

  scale->rgb = *rgb;
  scale->hsv = *hsv;

  scale->needs_render = TRUE;
  gtk_widget_queue_draw (GTK_WIDGET (scale));
}

/**
 * gimp_color_scale_set_color_config:
 * @scale:  a #GimpColorScale widget.
 * @config: a #GimpColorConfig object.
 *
 * Sets the color management configuration to use with this color scale.
 *
 * Since: 2.10
 */
void
gimp_color_scale_set_color_config (GimpColorScale  *scale,
                                   GimpColorConfig *config)
{
  GimpColorScalePrivate *priv;

  g_return_if_fail (GIMP_IS_COLOR_SCALE (scale));
  g_return_if_fail (config == NULL || GIMP_IS_COLOR_CONFIG (config));

  priv = GET_PRIVATE (scale);

  if (config != priv->config)
    {
      if (priv->config)
        {
          g_signal_handlers_disconnect_by_func (priv->config,
                                                gimp_color_scale_notify_config,
                                                scale);

          gimp_color_scale_destroy_transform (scale);
        }

      g_set_object (&priv->config, config);

      if (priv->config)
        {
          g_signal_connect (priv->config, "notify",
                            G_CALLBACK (gimp_color_scale_notify_config),
                            scale);

          gimp_color_scale_notify_config (priv->config, NULL, scale);
        }
    }
}


/* as in gtkrange.c */
static gboolean
should_invert (GtkRange *range)
{
  gboolean inverted  = gtk_range_get_inverted (range);
  gboolean flippable = gtk_range_get_flippable (range);

  if (gtk_orientable_get_orientation (GTK_ORIENTABLE (range)) ==
      GTK_ORIENTATION_HORIZONTAL)
    {
      return
        (inverted && !flippable) ||
        (inverted && flippable &&
         gtk_widget_get_direction (GTK_WIDGET (range)) == GTK_TEXT_DIR_LTR) ||
        (!inverted && flippable &&
         gtk_widget_get_direction (GTK_WIDGET (range)) == GTK_TEXT_DIR_RTL);
    }
  else
    {
      return inverted;
    }
}

static void
gimp_color_scale_render (GimpColorScale *scale)
{
  GimpColorScalePrivate *priv  = GET_PRIVATE (scale);
  GtkRange              *range = GTK_RANGE (scale);
  GimpRGB                rgb;
  GimpHSV                hsv;
  GimpLCH                lch;
  gint                   multiplier = 1;
  guint                  x, y;
  gdouble               *channel_value = NULL; /* shut up compiler */
  gboolean               from_hsv      = FALSE;
  gboolean               from_lch      = FALSE;
  gboolean               invert;
  guchar                *buf;
  guchar                *d;

  if ((buf = scale->buf) == NULL)
    return;

  if (scale->channel == GIMP_COLOR_SELECTOR_ALPHA)
    {
      gimp_color_scale_render_alpha (scale);
      return;
    }

  rgb = scale->rgb;
  hsv = scale->hsv;
  babl_process (fish_rgb_to_lch, &rgb, &lch, 1);

  switch (scale->channel)
    {
    case GIMP_COLOR_SELECTOR_HUE:        channel_value = &hsv.h; break;
    case GIMP_COLOR_SELECTOR_SATURATION: channel_value = &hsv.s; break;
    case GIMP_COLOR_SELECTOR_VALUE:      channel_value = &hsv.v; break;

    case GIMP_COLOR_SELECTOR_RED:        channel_value = &rgb.r; break;
    case GIMP_COLOR_SELECTOR_GREEN:      channel_value = &rgb.g; break;
    case GIMP_COLOR_SELECTOR_BLUE:       channel_value = &rgb.b; break;
    case GIMP_COLOR_SELECTOR_ALPHA:      channel_value = &rgb.a; break;

    case GIMP_COLOR_SELECTOR_LCH_LIGHTNESS: channel_value = &lch.l; break;
    case GIMP_COLOR_SELECTOR_LCH_CHROMA:    channel_value = &lch.c; break;
    case GIMP_COLOR_SELECTOR_LCH_HUE:       channel_value = &lch.h; break;
    }

  switch (scale->channel)
    {
    case GIMP_COLOR_SELECTOR_HUE:
    case GIMP_COLOR_SELECTOR_SATURATION:
    case GIMP_COLOR_SELECTOR_VALUE:
      from_hsv = TRUE;
      break;

    case GIMP_COLOR_SELECTOR_LCH_LIGHTNESS:
      multiplier = 100;
      from_lch = TRUE;
      break;
    case GIMP_COLOR_SELECTOR_LCH_CHROMA:
      multiplier = 200;
      from_lch = TRUE;
      break;
    case GIMP_COLOR_SELECTOR_LCH_HUE:
      multiplier = 360;
      from_lch = TRUE;
      break;

    default:
      break;
    }

  invert = should_invert (range);

  switch (gtk_orientable_get_orientation (GTK_ORIENTABLE (range)))
    {
    case GTK_ORIENTATION_HORIZONTAL:
      for (x = 0, d = buf; x < scale->width; x++, d += 4)
        {
          gdouble value = (gdouble) x * multiplier / (gdouble) (scale->width - 1);
          guchar  r, g, b;

          if (invert)
            value = multiplier - value;

          *channel_value = value;

          if (from_hsv)
            gimp_hsv_to_rgb (&hsv, &rgb);
          else if (from_lch)
            babl_process (fish_lch_to_rgb, &lch, &rgb, 1);

          if (rgb.r < 0.0 || rgb.r > 1.0 ||
              rgb.g < 0.0 || rgb.g > 1.0 ||
              rgb.b < 0.0 || rgb.b > 1.0)
            {
              r = priv->oog_color[0];
              g = priv->oog_color[1];
              b = priv->oog_color[2];
            }
          else
            {
              gimp_rgb_get_uchar (&rgb, &r, &g, &b);
            }

          GIMP_CAIRO_RGB24_SET_PIXEL (d, r, g, b);
        }

      d = buf + scale->rowstride;
      for (y = 1; y < scale->height; y++)
        {
          memcpy (d, buf, scale->rowstride);
          d += scale->rowstride;
        }
      break;

    case GTK_ORIENTATION_VERTICAL:
      for (y = 0; y < scale->height; y++)
        {
          gdouble value = (gdouble) y * multiplier / (gdouble) (scale->height - 1);
          guchar  r, g, b;

          if (invert)
            value = multiplier - value;

          *channel_value = value;

          if (from_hsv)
            gimp_hsv_to_rgb (&hsv, &rgb);
          else if (from_lch)
            babl_process (fish_lch_to_rgb, &lch, &rgb, 1);

          if (rgb.r < 0.0 || rgb.r > 1.0 ||
              rgb.g < 0.0 || rgb.g > 1.0 ||
              rgb.b < 0.0 || rgb.b > 1.0)
            {
              r = priv->oog_color[0];
              g = priv->oog_color[1];
              b = priv->oog_color[2];
            }
          else
            {
              gimp_rgb_get_uchar (&rgb, &r, &g, &b);
            }

          for (x = 0, d = buf; x < scale->width; x++, d += 4)
            {
              GIMP_CAIRO_RGB24_SET_PIXEL (d, r, g, b);
            }

          buf += scale->rowstride;
        }
      break;
    }
}

static void
gimp_color_scale_render_alpha (GimpColorScale *scale)
{
  GtkRange *range = GTK_RANGE (scale);
  GimpRGB   rgb;
  gboolean  invert;
  gdouble   a;
  guint     x, y;
  guchar   *buf;
  guchar   *d, *l;

  invert = should_invert (range);

  buf = scale->buf;
  rgb = scale->rgb;

  switch (gtk_orientable_get_orientation (GTK_ORIENTABLE (range)))
    {
    case GTK_ORIENTATION_HORIZONTAL:
      {
        guchar  *light;
        guchar  *dark;

        light = buf;
        /* this won't work correctly for very thin scales */
        dark  = (scale->height > GIMP_CHECK_SIZE_SM ?
                 buf + GIMP_CHECK_SIZE_SM * scale->rowstride : light);

        for (x = 0, d = light, l = dark; x < scale->width; x++)
          {
            if ((x % GIMP_CHECK_SIZE_SM) == 0)
              {
                guchar *t;

                t = d;
                d = l;
                l = t;
              }

            a = (gdouble) x / (gdouble) (scale->width - 1);

            if (invert)
              a = 1.0 - a;

            GIMP_CAIRO_RGB24_SET_PIXEL (l,
                                        (GIMP_CHECK_LIGHT +
                                         (rgb.r - GIMP_CHECK_LIGHT) * a) * 255.999,
                                        (GIMP_CHECK_LIGHT +
                                         (rgb.g - GIMP_CHECK_LIGHT) * a) * 255.999,
                                        (GIMP_CHECK_LIGHT +
                                         (rgb.b - GIMP_CHECK_LIGHT) * a) * 255.999);
            l += 4;

            GIMP_CAIRO_RGB24_SET_PIXEL (d,
                                        (GIMP_CHECK_DARK +
                                         (rgb.r - GIMP_CHECK_DARK) * a) * 255.999,
                                        (GIMP_CHECK_DARK +
                                         (rgb.g - GIMP_CHECK_DARK) * a) * 255.999,
                                        (GIMP_CHECK_DARK +
                                         (rgb.b - GIMP_CHECK_DARK) * a) * 255.999);
            d += 4;
          }

        for (y = 0, d = buf; y < scale->height; y++, d += scale->rowstride)
          {
            if (y == 0 || y == GIMP_CHECK_SIZE_SM)
              continue;

            if ((y / GIMP_CHECK_SIZE_SM) & 1)
              memcpy (d, dark, scale->rowstride);
            else
              memcpy (d, light, scale->rowstride);
          }
      }
      break;

    case GTK_ORIENTATION_VERTICAL:
      {
        guchar  light[4] = {0xff, 0xff, 0xff, 0xff};
        guchar  dark[4] = {0xff, 0xff, 0xff, 0xff};

        for (y = 0, d = buf; y < scale->height; y++, d += scale->rowstride)
          {
            a = (gdouble) y / (gdouble) (scale->height - 1);

            if (invert)
              a = 1.0 - a;

            GIMP_CAIRO_RGB24_SET_PIXEL (light,
                                        (GIMP_CHECK_LIGHT +
                                         (rgb.r - GIMP_CHECK_LIGHT) * a) * 255.999,
                                        (GIMP_CHECK_LIGHT +
                                         (rgb.g - GIMP_CHECK_LIGHT) * a) * 255.999,
                                        (GIMP_CHECK_LIGHT +
                                         (rgb.b - GIMP_CHECK_LIGHT) * a) * 255.999);

            GIMP_CAIRO_RGB24_SET_PIXEL (dark,
                                        (GIMP_CHECK_DARK +
                                         (rgb.r - GIMP_CHECK_DARK) * a) * 255.999,
                                        (GIMP_CHECK_DARK +
                                         (rgb.g - GIMP_CHECK_DARK) * a) * 255.999,
                                        (GIMP_CHECK_DARK +
                                         (rgb.b - GIMP_CHECK_DARK) * a) * 255.999);

            for (x = 0, l = d; x < scale->width; x++, l += 4)
              {
                if (((x / GIMP_CHECK_SIZE_SM) ^ (y / GIMP_CHECK_SIZE_SM)) & 1)
                  {
                    l[0] = light[0];
                    l[1] = light[1];
                    l[2] = light[2];
                    l[3] = light[3];
                  }
                else
                  {
                    l[0] = dark[0];
                    l[1] = dark[1];
                    l[2] = dark[2];
                    l[3] = dark[3];
                  }
              }
          }
      }
      break;
    }
}

/*
 * This could be integrated into the render functions which might be
 * slightly faster. But we trade speed for keeping the code simple.
 */
static void
gimp_color_scale_render_stipple (GimpColorScale *scale)
{
  GtkWidget *widget = GTK_WIDGET (scale);
  GtkStyle  *style  = gtk_widget_get_style (widget);
  guchar    *buf;
  guchar     insensitive[4] = {0xff, 0xff, 0xff, 0xff};
  guint      x, y;

  if ((buf = scale->buf) == NULL)
    return;

  GIMP_CAIRO_RGB24_SET_PIXEL (insensitive,
                              style->bg[GTK_STATE_INSENSITIVE].red   >> 8,
                              style->bg[GTK_STATE_INSENSITIVE].green >> 8,
                              style->bg[GTK_STATE_INSENSITIVE].blue  >> 8);

  for (y = 0; y < scale->height; y++, buf += scale->rowstride)
    {
      guchar *d = buf + 4 * (y % 2);

      for (x = 0; x < scale->width - (y % 2); x += 2, d += 8)
        {
          d[0] = insensitive[0];
          d[1] = insensitive[1];
          d[2] = insensitive[2];
          d[3] = insensitive[3];
        }
    }
}

static void
gimp_color_scale_create_transform (GimpColorScale *scale)
{
  GimpColorScalePrivate *priv = GET_PRIVATE (scale);

  if (priv->config)
    {
      static GimpColorProfile *profile = NULL;

      const Babl *format = babl_format ("cairo-RGB24");

      if (G_UNLIKELY (! profile))
        profile = gimp_color_profile_new_rgb_srgb ();

      priv->transform = gimp_widget_get_color_transform (GTK_WIDGET (scale),
                                                         priv->config,
                                                         profile,
                                                         format,
                                                         format);
    }
}

static void
gimp_color_scale_destroy_transform (GimpColorScale *scale)
{
  GimpColorScalePrivate *priv = GET_PRIVATE (scale);

  if (priv->transform)
    {
      g_object_unref (priv->transform);
      priv->transform = NULL;
    }

  gtk_widget_queue_draw (GTK_WIDGET (scale));
}

static void
gimp_color_scale_notify_config (GimpColorConfig  *config,
                                const GParamSpec *pspec,
                                GimpColorScale   *scale)
{
  GimpColorScalePrivate *priv = GET_PRIVATE (scale);

  gimp_color_scale_destroy_transform (scale);

  gimp_rgb_get_uchar (&config->out_of_gamut_color,
                      priv->oog_color,
                      priv->oog_color + 1,
                      priv->oog_color + 2);
  scale->needs_render = TRUE;
}
