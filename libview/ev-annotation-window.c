/* ev-annotation-window.c
 *  this file is part of evince, a gnome document viewer
 *
 * Copyright (C) 2009 Carlos Garcia Campos <carlosgc@gnome.org>
 * Copyright (C) 2007 Iñigo Martinez <inigomartinez@gmail.com>
 *
 * Evince is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Evince is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "config.h"

#include <string.h>
#include <math.h>

#include "ev-annotation-window.h"
#include "ev-stock-icons.h"
#include "ev-view-marshal.h"
#include "ev-document-misc.h"

#if WITH_GSPELL
#include <glib/gi18n.h>
#include <gspell/gspell.h>
#endif

enum {
	PROP_0,
	PROP_ANNOTATION,
	PROP_PARENT
};

enum {
	CLOSED,
	MOVED,
	N_SIGNALS
};

struct _EvAnnotationWindow {
	GtkWindow     base_instance;

	EvAnnotation *annotation;
	GtkWindow    *parent;

	GtkWidget    *title;
	GtkWidget    *close_button;
	GtkWidget    *text_view;
	GtkWidget    *resize_se;
	GtkWidget    *resize_sw;

	gboolean      is_open;
	EvRectangle   rect;

	gboolean      in_move;
	gint          x;
	gint          y;
	gint          orig_x;
	gint          orig_y;

#if WITH_GSPELL
	GspellTextView *spellcheck_view;
	gboolean      enable_spellchecking;
#endif
};

struct _EvAnnotationWindowClass {
	GtkWindowClass base_class;

	void (* closed) (EvAnnotationWindow *window);
	void (* moved)  (EvAnnotationWindow *window,
			 gint                x,
			 gint                y);
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE (EvAnnotationWindow, ev_annotation_window, GTK_TYPE_WINDOW)

/* Cut and paste from gtkwindow.c */
static void
send_focus_change (GtkWidget *widget,
		   gboolean   in)
{
	GdkEvent *fevent = gdk_event_new (GDK_FOCUS_CHANGE);

	fevent->focus_change.type = GDK_FOCUS_CHANGE;
	fevent->focus_change.window = gtk_widget_get_window (widget);
	fevent->focus_change.in = in;
	if (fevent->focus_change.window)
		g_object_ref (fevent->focus_change.window);

	gtk_widget_send_focus_change (widget, fevent);

	gdk_event_free (fevent);
}

static void
ev_annotation_window_sync_contents (EvAnnotationWindow *window)
{
	gchar         *contents;
	GtkTextIter    start, end;
	GtkTextBuffer *buffer;
	EvAnnotation  *annot = window->annotation;

	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (window->text_view));
	gtk_text_buffer_get_bounds (buffer, &start, &end);
	contents = gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
	ev_annotation_set_contents (annot, contents);
	g_free (contents);
}

static double
get_srgb (const double color_component)
{
	/* calculation of sRGB color is based on note 1 of https://www.w3.org/TR/2008/REC-WCAG20-20081211/#relativeluminancedef */
	if (color_component <= 0.03928)
		return color_component / 12.92;
	else
		return powf (((color_component + 0.055) / 1.055), 2.4);
}

static double
get_relative_luminance (const GdkRGBA *color)
{
	/* calculation of relative luminance is based on note 1 of https://www.w3.org/TR/2008/REC-WCAG20-20081211/#relativeluminancedef */
	return get_srgb (color->red) * 0.2126 + get_srgb (color->blue) * 0.0722 + get_srgb (color->green) * 0.7152;
}

static double
get_contrast_level (const GdkRGBA *bg_color,
		    const GdkRGBA *fg_color)
{
	/* the contrast level calculus is based on WCAG 2.0 guideline 1.4  */
	/* https://www.w3.org/WAI/GL/UNDERSTANDING-WCAG20/visual-audio-contrast7.html#key-terms */
	const double bg_luminance = get_relative_luminance (bg_color);
	const double fg_luminance = get_relative_luminance (fg_color);
	return (fmax (bg_luminance, fg_luminance) + 0.05) / (fmin (bg_luminance, fg_luminance) + 0.05);
}

/**
 * get_most_readable_color:
 *
 * Returns: (transfer none): the most readable color on bg_color between first_color and second_color
 */
static GdkRGBA *
get_most_readable_color (const GdkRGBA *bg_color,
			 GdkRGBA *first_color,
			 GdkRGBA *second_color)
{
	const double first_contrast = get_contrast_level (bg_color, first_color);
	const double second_contrast = get_contrast_level (bg_color, second_color);
	/* higher is more readable (more contrast) */
	return first_contrast > second_contrast ? first_color : second_color;
}

/**
 * get_best_foreground_color:
 *
 * Returns: (transfer full): the most readable foreground color on bg_color between black #000000 and white #FFFFFF
 */
static GdkRGBA *
get_best_foreground_color (const GdkRGBA *bg_color)
{
	GdkRGBA black, white;
	gdk_rgba_parse (&black, "#000000");
	gdk_rgba_parse (&white, "#FFFFFF");
	return gdk_rgba_copy (get_most_readable_color (bg_color, &black, &white));
}

static void
ev_annotation_window_set_color (EvAnnotationWindow *window,
				GdkRGBA            *color)
{
	GtkCssProvider     *css_provider = gtk_css_provider_new ();
	g_autofree char    *rgba_str = gdk_rgba_to_string (color);
	g_autofree char    *css_data = NULL;
	g_autoptr (GError)  error = NULL;
	g_autoptr (GdkRGBA) icon_color = get_best_foreground_color (color);
	g_autofree char    *icon_color_str = gdk_rgba_to_string (icon_color);
	css_data = g_strdup_printf ("button {border-color: %1$s; color: %2$s; -gtk-icon-shadow:0 0; box-shadow:0 0;}\n\
				     button:hover {background: lighter(%1$s); border-color: darker(%1$s);}\n\
				     button:active {background: darker(%1$s);}\n\
				     evannotationwindow.background, button {background: %1$s}",
				    rgba_str, icon_color_str);

	gtk_css_provider_load_from_data (css_provider, css_data, strlen (css_data), &error);
	if (error != NULL)
		g_error ("%s", error->message);

	gtk_style_context_add_provider (gtk_widget_get_style_context (GTK_WIDGET (window)),
					GTK_STYLE_PROVIDER (css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	gtk_style_context_add_provider (gtk_widget_get_style_context (window->close_button),
					GTK_STYLE_PROVIDER (css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	gtk_style_context_add_class (gtk_widget_get_style_context (window->close_button), "circular");
}

static void
ev_annotation_window_set_opacity (EvAnnotationWindow *window,
		                  gdouble             opacity)
{
	gtk_widget_set_opacity (GTK_WIDGET (window), opacity);
	gtk_widget_set_opacity (GTK_WIDGET (window->text_view), opacity);
}

static void
ev_annotation_window_label_changed (EvAnnotationMarkup *annot,
				    GParamSpec         *pspec,
				    EvAnnotationWindow *window)
{
	const gchar *label = ev_annotation_markup_get_label (annot);

	gtk_window_set_title (GTK_WINDOW (window), label);
	gtk_label_set_text (GTK_LABEL (window->title), label);
}

static void
ev_annotation_window_color_changed (EvAnnotation       *annot,
				    GParamSpec         *pspec,
				    EvAnnotationWindow *window)
{
	GdkRGBA rgba;

	ev_annotation_get_rgba (annot, &rgba);
	ev_annotation_window_set_color (window, &rgba);
}

static void
ev_annotation_window_opacity_changed (EvAnnotation       *annot,
		                      GParamSpec         *pspec,
				      EvAnnotationWindow *window)
{
	gdouble opacity;

	opacity = ev_annotation_markup_get_opacity (EV_ANNOTATION_MARKUP (annot));
	ev_annotation_window_set_opacity (window, opacity);
}

static void
ev_annotation_window_dispose (GObject *object)
{
	EvAnnotationWindow *window = EV_ANNOTATION_WINDOW (object);

	if (window->annotation) {
		ev_annotation_window_sync_contents (window);
		g_object_unref (window->annotation);
		window->annotation = NULL;
	}

	(* G_OBJECT_CLASS (ev_annotation_window_parent_class)->dispose) (object);
}

static void
ev_annotation_window_set_property (GObject      *object,
				   guint         prop_id,
				   const GValue *value,
				   GParamSpec   *pspec)
{
	EvAnnotationWindow *window = EV_ANNOTATION_WINDOW (object);

	switch (prop_id) {
	case PROP_ANNOTATION:
		window->annotation = g_value_dup_object (value);
		break;
	case PROP_PARENT:
		window->parent = g_value_get_object (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
	}
}

static gboolean
ev_annotation_window_resize (EvAnnotationWindow *window,
			     GdkEventButton     *event,
			     GtkWidget          *ebox)
{
	if (event->type == GDK_BUTTON_PRESS && event->button == 1) {
		gtk_window_begin_resize_drag (GTK_WINDOW (window),
					      window->resize_sw == ebox ?
					      GDK_WINDOW_EDGE_SOUTH_WEST :
					      GDK_WINDOW_EDGE_SOUTH_EAST,
					      event->button, event->x_root,
					      event->y_root, event->time);
		return TRUE;
	}

	return FALSE;
}

static void
ev_annotation_window_set_resize_cursor (GtkWidget          *widget,
					EvAnnotationWindow *window)
{
	GdkWindow *gdk_window = gtk_widget_get_window (widget);

	if (!gdk_window)
		return;

	if (gtk_widget_is_sensitive (widget)) {
		GdkDisplay *display = gtk_widget_get_display (widget);
		GdkCursor  *cursor;

		cursor = gdk_cursor_new_for_display (display,
						     widget == window->resize_sw ?
						     GDK_BOTTOM_LEFT_CORNER :
						     GDK_BOTTOM_RIGHT_CORNER);
		gdk_window_set_cursor (gdk_window, cursor);
		g_object_unref (cursor);
	} else {
		gdk_window_set_cursor (gdk_window, NULL);
	}
}

static void
text_view_state_flags_changed (GtkWidget     *widget,
			       GtkStateFlags  previous_flags)
{
	GtkStateFlags current_flags = gtk_widget_get_state_flags (widget);

	if (current_flags & GTK_STATE_FLAG_BACKDROP)
		gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (widget), FALSE);
}

static void
ev_annotation_window_close (EvAnnotationWindow *window)
{
	gtk_widget_hide (GTK_WIDGET (window));
	g_signal_emit (window, signals[CLOSED], 0);
}

static gboolean
ev_annotation_window_button_press_event (GtkWidget      *widget,
					 GdkEventButton *event)
{
	EvAnnotationWindow *window = EV_ANNOTATION_WINDOW (widget);

	if (event->type == GDK_BUTTON_PRESS && event->button == 1) {
		window->in_move = TRUE;
		window->x = event->x_root - event->x;
		window->y = event->y_root - event->y;
		gtk_window_begin_move_drag (GTK_WINDOW (widget),
					    event->button,
					    event->x_root,
					    event->y_root,
					    event->time);
		return TRUE;
	}

	return FALSE;
}

static void
ev_annotation_window_init (EvAnnotationWindow *window)
{
	GtkWidget    *vbox, *hbox;
	GtkWidget    *icon;
	GtkWidget    *swindow;
	GtkWidget    *header;
	GtkIconTheme *icon_theme;
	GdkPixbuf    *pixbuf;

	icon_theme = gtk_icon_theme_get_default ();

	gtk_widget_set_can_focus (GTK_WIDGET (window), TRUE);

	vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

	/* Title bar */
	hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

	icon = gtk_image_new (); /* FIXME: use the annot icon */
	gtk_box_pack_start (GTK_BOX (hbox), icon, FALSE, FALSE, 0);
	gtk_widget_show (icon);

	header = gtk_event_box_new ();
	gtk_widget_add_events (header, GDK_BUTTON_PRESS_MASK);
	g_signal_connect_swapped (header, "button-press-event",
				  G_CALLBACK (ev_annotation_window_button_press_event),
			          window);

	window->title = gtk_label_new (NULL);
	gtk_container_add (GTK_CONTAINER (header), window->title);
	gtk_widget_show (window->title);

	gtk_box_pack_start (GTK_BOX (hbox), header, TRUE, TRUE, 0);
	gtk_widget_show (header);

	window->close_button = gtk_button_new_from_icon_name ("window-close-symbolic", GTK_ICON_SIZE_BUTTON);
	g_signal_connect_swapped (window->close_button, "clicked",
				  G_CALLBACK (ev_annotation_window_close),
				  window);

	gtk_box_pack_start (GTK_BOX (hbox), window->close_button, FALSE, FALSE, 0);
	gtk_widget_show (window->close_button);

	gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
	gtk_widget_show (hbox);

	/* Contents */
	swindow = gtk_scrolled_window_new (NULL, NULL);
	window->text_view = gtk_text_view_new ();

#if WITH_GSPELL
	window->spellcheck_view = NULL;
	window->spellcheck_view = gspell_text_view_get_from_gtk_text_view (GTK_TEXT_VIEW (window->text_view));
	gspell_text_view_basic_setup (window->spellcheck_view);
#endif

	gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (window->text_view), GTK_WRAP_WORD);
	g_signal_connect (window->text_view, "state-flags-changed",
			  G_CALLBACK (text_view_state_flags_changed),
			  window);
	gtk_container_add (GTK_CONTAINER (swindow), window->text_view);
	gtk_widget_show (window->text_view);

	gtk_box_pack_start (GTK_BOX (vbox), swindow, TRUE, TRUE, 0);
	gtk_widget_show (swindow);

	/* Resize bar */
	hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

	window->resize_sw = gtk_event_box_new ();
	gtk_widget_add_events (window->resize_sw, GDK_BUTTON_PRESS_MASK);
	g_signal_connect_swapped (window->resize_sw, "button-press-event",
				  G_CALLBACK (ev_annotation_window_resize),
				  window);
	g_signal_connect (window->resize_sw, "realize",
			  G_CALLBACK (ev_annotation_window_set_resize_cursor),
			  window);

	pixbuf = gtk_icon_theme_load_icon (icon_theme, EV_STOCK_RESIZE_SW, 8,
					   GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
	icon = gtk_image_new_from_pixbuf (pixbuf);
	g_object_unref (pixbuf);
	gtk_container_add (GTK_CONTAINER (window->resize_sw), icon);
	gtk_widget_show (icon);
	gtk_box_pack_start (GTK_BOX (hbox), window->resize_sw, FALSE, FALSE, 0);
	gtk_widget_show (window->resize_sw);

	window->resize_se = gtk_event_box_new ();
	gtk_widget_add_events (window->resize_se, GDK_BUTTON_PRESS_MASK);
	g_signal_connect_swapped (window->resize_se, "button-press-event",
				  G_CALLBACK (ev_annotation_window_resize),
				  window);
	g_signal_connect (window->resize_se, "realize",
			  G_CALLBACK (ev_annotation_window_set_resize_cursor),
			  window);

	pixbuf = gtk_icon_theme_load_icon (icon_theme, EV_STOCK_RESIZE_SE, 8,
					   GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
	icon = gtk_image_new_from_pixbuf (pixbuf);
	g_object_unref (pixbuf);
	gtk_container_add (GTK_CONTAINER (window->resize_se), icon);
	gtk_widget_show (icon);
	gtk_box_pack_end (GTK_BOX (hbox), window->resize_se, FALSE, FALSE, 0);
	gtk_widget_show (window->resize_se);

	gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
	gtk_widget_show (hbox);

	gtk_container_add (GTK_CONTAINER (window), vbox);
	gtk_widget_show (vbox);

	gtk_widget_add_events (GTK_WIDGET (window),
			       GDK_BUTTON_PRESS_MASK |
			       GDK_KEY_PRESS_MASK);

	gtk_container_set_border_width (GTK_CONTAINER (window), 2);

	gtk_window_set_decorated (GTK_WINDOW (window), FALSE);
	gtk_window_set_skip_taskbar_hint (GTK_WINDOW (window), TRUE);
	gtk_window_set_skip_pager_hint (GTK_WINDOW (window), TRUE);
	gtk_window_set_resizable (GTK_WINDOW (window), TRUE);
}

static GObject *
ev_annotation_window_constructor (GType                  type,
				  guint                  n_construct_properties,
				  GObjectConstructParam *construct_params)
{
	GObject            *object;
	EvAnnotationWindow *window;
	EvAnnotation       *annot;
	EvAnnotationMarkup *markup;
	const gchar        *contents;
	const gchar        *label;
	GdkRGBA             color;
	EvRectangle        *rect;
	gdouble             scale;
	gdouble             opacity;

	object = G_OBJECT_CLASS (ev_annotation_window_parent_class)->constructor (type,
										  n_construct_properties,
										  construct_params);
	window = EV_ANNOTATION_WINDOW (object);
	annot = window->annotation;
	markup = EV_ANNOTATION_MARKUP (annot);

	gtk_window_set_transient_for (GTK_WINDOW (window), window->parent);
	gtk_window_set_destroy_with_parent (GTK_WINDOW (window), FALSE);

	label = ev_annotation_markup_get_label (markup);
	window->is_open = ev_annotation_markup_get_popup_is_open (markup);
	ev_annotation_markup_get_rectangle (markup, &window->rect);

	rect = &window->rect;

	/* Rectangle is at doc resolution (72.0) */
	scale = ev_document_misc_get_widget_dpi (GTK_WIDGET (window)) / 72.0;
	gtk_window_resize (GTK_WINDOW (window),
			   (gint)((rect->x2 - rect->x1) * scale),
			   (gint)((rect->y2 - rect->y1) * scale));

	ev_annotation_get_rgba (annot, &color);
	ev_annotation_window_set_color (window, &color);

	opacity = ev_annotation_markup_get_opacity (markup);
	ev_annotation_window_set_opacity (window, opacity);

	gtk_widget_set_name (GTK_WIDGET (window), ev_annotation_get_name (annot));
	gtk_window_set_title (GTK_WINDOW (window), label);
	gtk_label_set_text (GTK_LABEL (window->title), label);

	contents = ev_annotation_get_contents (annot);
	if (contents) {
		GtkTextBuffer *buffer;

		buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (window->text_view));
		gtk_text_buffer_set_text (buffer, contents, -1);
	}

	g_signal_connect (annot, "notify::label",
			  G_CALLBACK (ev_annotation_window_label_changed),
			  window);
	g_signal_connect (annot, "notify::rgba",
			  G_CALLBACK (ev_annotation_window_color_changed),
			  window);
	g_signal_connect (annot, "notify::opacity",
			  G_CALLBACK (ev_annotation_window_opacity_changed),
			  window);

#if WITH_GSPELL
        gspell_text_view_set_inline_spell_checking (window->spellcheck_view, ev_annotation_window_get_enable_spellchecking (window));
#endif
	return object;
}

static gboolean
ev_annotation_window_configure_event (GtkWidget         *widget,
				      GdkEventConfigure *event)
{
	EvAnnotationWindow *window = EV_ANNOTATION_WINDOW (widget);

	if (window->in_move &&
	    (window->x != event->x || window->y != event->y)) {
		window->x = event->x;
		window->y = event->y;
	}

	return GTK_WIDGET_CLASS (ev_annotation_window_parent_class)->configure_event (widget, event);
}

static gboolean
ev_annotation_window_focus_in_event (GtkWidget     *widget,
				     GdkEventFocus *event)
{
	EvAnnotationWindow *window = EV_ANNOTATION_WINDOW (widget);

	if (window->in_move) {
		if (window->orig_x != window->x || window->orig_y != window->y) {
			window->orig_x = window->x;
			window->orig_y = window->y;
			g_signal_emit (window, signals[MOVED], 0, window->x, window->y);
		}
		window->in_move = FALSE;
	}

	gtk_widget_grab_focus (window->text_view);
	send_focus_change (window->text_view, TRUE);
	gtk_text_view_set_cursor_visible (GTK_TEXT_VIEW (window->text_view), TRUE);

	return FALSE;
}

static gboolean
ev_annotation_window_focus_out_event (GtkWidget     *widget,
				      GdkEventFocus *event)
{
	EvAnnotationWindow *window = EV_ANNOTATION_WINDOW (widget);

	ev_annotation_window_sync_contents (window);

	return FALSE;
}

static gboolean
ev_annotation_window_key_press_event (GtkWidget   *widget,
                                      GdkEventKey *event)
{
        if (event->keyval == GDK_KEY_Escape) {
                ev_annotation_window_close (EV_ANNOTATION_WINDOW (widget));
                return TRUE;
        }

        return GTK_WIDGET_CLASS (ev_annotation_window_parent_class)->key_press_event (widget, event);
}

static void
ev_annotation_window_class_init (EvAnnotationWindowClass *klass)
{
	GObjectClass   *g_object_class = G_OBJECT_CLASS (klass);
	GtkWidgetClass *gtk_widget_class = GTK_WIDGET_CLASS (klass);

	g_object_class->constructor = ev_annotation_window_constructor;
	g_object_class->set_property = ev_annotation_window_set_property;
	g_object_class->dispose = ev_annotation_window_dispose;

	gtk_widget_class->configure_event = ev_annotation_window_configure_event;
	gtk_widget_class->focus_in_event = ev_annotation_window_focus_in_event;
	gtk_widget_class->focus_out_event = ev_annotation_window_focus_out_event;
        gtk_widget_class->key_press_event = ev_annotation_window_key_press_event;

#if GTK_CHECK_VERSION(3, 20, 0)
	gtk_widget_class_set_css_name (gtk_widget_class, "evannotationwindow");
#endif
	g_object_class_install_property (g_object_class,
					 PROP_ANNOTATION,
					 g_param_spec_object ("annotation",
							      "Annotation",
							      "The annotation associated to the window",
							      EV_TYPE_ANNOTATION_MARKUP,
							      G_PARAM_WRITABLE |
							      G_PARAM_CONSTRUCT_ONLY |
                                                              G_PARAM_STATIC_STRINGS));
	g_object_class_install_property (g_object_class,
					 PROP_PARENT,
					 g_param_spec_object ("parent",
							      "Parent",
							      "The parent window",
							      GTK_TYPE_WINDOW,
							      G_PARAM_WRITABLE |
							      G_PARAM_CONSTRUCT_ONLY |
                                                              G_PARAM_STATIC_STRINGS));
	signals[CLOSED] =
		g_signal_new ("closed",
			      G_TYPE_FROM_CLASS (g_object_class),
			      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
			      G_STRUCT_OFFSET (EvAnnotationWindowClass, closed),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0, G_TYPE_NONE);
	signals[MOVED] =
		g_signal_new ("moved",
			      G_TYPE_FROM_CLASS (g_object_class),
			      G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
			      G_STRUCT_OFFSET (EvAnnotationWindowClass, moved),
			      NULL, NULL,
			      ev_view_marshal_VOID__INT_INT,
			      G_TYPE_NONE, 2,
			      G_TYPE_INT, G_TYPE_INT);
}

/* Public methods */
GtkWidget *
ev_annotation_window_new (EvAnnotation *annot,
			  GtkWindow    *parent)
{
	GtkWidget *window;

	g_return_val_if_fail (EV_IS_ANNOTATION_MARKUP (annot), NULL);
	g_return_val_if_fail (GTK_IS_WINDOW (parent), NULL);

	window = g_object_new (EV_TYPE_ANNOTATION_WINDOW,
			       "annotation", annot,
			       "parent", parent,
			       NULL);
	return window;
}

EvAnnotation *
ev_annotation_window_get_annotation (EvAnnotationWindow *window)
{
	g_return_val_if_fail (EV_IS_ANNOTATION_WINDOW (window), NULL);

	return window->annotation;
}

void
ev_annotation_window_set_annotation (EvAnnotationWindow *window,
				     EvAnnotation       *annot)
{
	g_return_if_fail (EV_IS_ANNOTATION_WINDOW (window));
	g_return_if_fail (EV_IS_ANNOTATION (annot));

	if (annot == window->annotation)
		return;

	g_object_unref (window->annotation);
	window->annotation = g_object_ref (annot);
	ev_annotation_window_sync_contents (window);
	g_object_notify (G_OBJECT (window), "annotation");
}

gboolean
ev_annotation_window_is_open (EvAnnotationWindow *window)
{
	g_return_val_if_fail (EV_IS_ANNOTATION_WINDOW (window), FALSE);

	return window->is_open;
}

void
ev_annotation_window_get_rectangle (EvAnnotationWindow *window,
				    EvRectangle        *rect)
{
	g_return_if_fail (EV_IS_ANNOTATION_WINDOW (window));
	g_return_if_fail (rect != NULL);

	*rect = window->rect;
}

void
ev_annotation_window_set_rectangle (EvAnnotationWindow *window,
				    const EvRectangle  *rect)
{
	g_return_if_fail (EV_IS_ANNOTATION_WINDOW (window));
	g_return_if_fail (rect != NULL);

	window->rect = *rect;
}

void
ev_annotation_window_grab_focus (EvAnnotationWindow *window)
{
	g_return_if_fail (EV_IS_ANNOTATION_WINDOW (window));

	if (!gtk_widget_has_focus (window->text_view)) {
		gtk_widget_grab_focus (GTK_WIDGET (window));
		send_focus_change (window->text_view, TRUE);
	}
}

void
ev_annotation_window_ungrab_focus (EvAnnotationWindow *window)
{
	g_return_if_fail (EV_IS_ANNOTATION_WINDOW (window));

	if (gtk_widget_has_focus (window->text_view)) {
		send_focus_change (window->text_view, FALSE);
	}

	ev_annotation_window_sync_contents (window);
}

void
ev_annotation_window_set_enable_spellchecking (EvAnnotationWindow *window,
                                               gboolean enable_spellchecking)
{
        g_return_if_fail (EV_IS_ANNOTATION_WINDOW (window));

#if WITH_GSPELL
        if (enable_spellchecking == ev_annotation_window_get_enable_spellchecking (window))
                return;

        window->enable_spellchecking = enable_spellchecking;
        gspell_text_view_set_inline_spell_checking (window->spellcheck_view, enable_spellchecking);
#endif
}

gboolean
ev_annotation_window_get_enable_spellchecking (EvAnnotationWindow *window)
{
        g_return_val_if_fail (EV_IS_ANNOTATION_WINDOW (window), FALSE);
#if WITH_GSPELL
        return window->enable_spellchecking;
#else
        return FALSE;
#endif
}
