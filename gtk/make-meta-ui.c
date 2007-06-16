/******************************************************************************
 * $Id:$
 *
 * Copyright (c) 2005-2007 Transmission authors and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#include <string.h>

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "transmission.h"
#include "makemeta.h"

#include "hig.h"
#include "make-meta-ui.h"
#include "util.h"

typedef struct
{
    GtkWidget * size_lb;
    GtkWidget * announce_entry;
    GtkWidget * comment_entry;
    GtkWidget * private_check;
    tr_metainfo_builder_t * builder;
    tr_handle_t * handle;
}
MakeMetaUI;

static void
cancel_cb( GtkDialog *d UNUSED, int response UNUSED, gpointer user_data )
{
    MakeMetaUI * ui = (MakeMetaUI *) user_data;
    ui->builder->abortFlag = TRUE;
}

static gboolean
refresh_cb ( gpointer user_data )
{
    MakeMetaUI * ui = (MakeMetaUI *) user_data;

    g_message ("refresh");

    return !ui->builder->isDone;
}

static void
response_cb( GtkDialog* d, int response, gpointer user_data )
{
    MakeMetaUI * ui = (MakeMetaUI*) user_data;
    GtkWidget *w, *l, *p;
    char *tmp, *name;

    if( response != GTK_RESPONSE_ACCEPT )
    {
        gtk_widget_destroy( GTK_WIDGET( d ) );
        return;
    }

    w = gtk_dialog_new_with_buttons( _("Making Torrent..."), 
                                     GTK_WINDOW(d),
                                     GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
                                     GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                     NULL );
    g_signal_connect( w, "response", G_CALLBACK(cancel_cb), ui );

    tmp = g_path_get_basename (ui->builder->top);
    name = g_strdup_printf ( "%s.torrent", tmp );
    l = gtk_label_new( name );
    gtk_box_pack_start_defaults ( GTK_BOX(GTK_DIALOG(w)->vbox), l );
    p = gtk_progress_bar_new ();
    gtk_box_pack_start_defaults ( GTK_BOX(GTK_DIALOG(w)->vbox), p );
    gtk_widget_show_all ( w );
    g_free( name );
    g_free( tmp );

    tr_makeMetaInfo( ui->builder,
                     NULL, 
                     gtk_entry_get_text( GTK_ENTRY( ui->announce_entry ) ),
                     gtk_entry_get_text( GTK_ENTRY( ui->comment_entry ) ),
                     gtk_toggle_button_get_active( GTK_TOGGLE_BUTTON( ui->private_check ) ) );

    g_timeout_add ( 1000, refresh_cb, ui );
}

static void
file_mode_toggled_cb (GtkToggleButton *togglebutton, gpointer user_data)
{
    if( gtk_toggle_button_get_active( togglebutton ) )
    {
        GtkFileChooserButton * w = GTK_FILE_CHOOSER_BUTTON(user_data);
        gtk_file_chooser_set_action( GTK_FILE_CHOOSER( w ), GTK_FILE_CHOOSER_ACTION_OPEN );
    }
}

static void
dir_mode_toggled_cb (GtkToggleButton *togglebutton, gpointer user_data)
{
    if( gtk_toggle_button_get_active( togglebutton ) )
    {
        GtkFileChooserButton * w = GTK_FILE_CHOOSER_BUTTON(user_data);
        gtk_file_chooser_set_action( GTK_FILE_CHOOSER( w ), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER );
    }
}

/***
****
***/

static void
file_selection_changed_cb( GtkFileChooser *chooser, gpointer user_data )
{
    MakeMetaUI * ui = (MakeMetaUI *) user_data;
    char * pch;
    char * filename;
    char buf[512];

    if( ui->builder ) {
        tr_metaInfoBuilderFree( ui->builder );
        ui->builder = NULL;
    }

    filename = gtk_file_chooser_get_filename( chooser );
    ui->builder = tr_metaInfoBuilderCreate( ui->handle, filename );
    g_free( filename );

    pch = readablesize( ui->builder->totalSize );
    g_snprintf( buf, sizeof(buf), "<i>%s; %lu %s</i>",
                pch, ui->builder->fileCount,
                ngettext("file", "files", (int)ui->builder->fileCount));
    gtk_label_set_markup ( GTK_LABEL(ui->size_lb), buf );
    g_free( pch );
}

GtkWidget*
make_meta_ui( GtkWindow * parent, tr_handle_t * handle )
{
    int row = 0;
    GtkWidget *d, *t, *w, *h, *rb_file, *rb_dir;
    char name[256];
    MakeMetaUI * ui = g_new0 ( MakeMetaUI, 1 );
    ui->handle = handle;

    d = gtk_dialog_new_with_buttons( _("Make a New Torrent"),
                                     parent,
                                     GTK_DIALOG_DESTROY_WITH_PARENT,
                                     GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                     GTK_STOCK_NEW, GTK_RESPONSE_ACCEPT,
                                     NULL );
    g_signal_connect( d, "response", G_CALLBACK(response_cb), ui );
    g_object_set_data_full( G_OBJECT(d), "ui", ui, g_free );

    t = hig_workarea_create ();

    hig_workarea_add_section_title (t, &row, _("Files"));
    hig_workarea_add_section_spacer (t, row, 3);

        g_snprintf( name, sizeof(name), "%s:", _("File _Type"));
        h = gtk_hbox_new( FALSE, GUI_PAD_SMALL );
        w = rb_dir = gtk_radio_button_new_with_mnemonic( NULL, _("Directory"));
        gtk_box_pack_start ( GTK_BOX(h), w, FALSE, FALSE, 0 );
        w = rb_file = gtk_radio_button_new_with_mnemonic_from_widget( GTK_RADIO_BUTTON(w), _("Single File") );
        gtk_box_pack_start ( GTK_BOX(h), w, FALSE, FALSE, 0 );
        hig_workarea_add_row (t, &row, name, h, NULL);

        g_snprintf( name, sizeof(name), "%s:", _("_File"));
        w = gtk_file_chooser_button_new( _("File or Directory to Add to the New Torrent"),
                                         GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER );
        g_signal_connect( w, "selection-changed", G_CALLBACK(file_selection_changed_cb), ui );
        g_signal_connect( rb_file, "toggled", G_CALLBACK(file_mode_toggled_cb), w );
        g_signal_connect( rb_dir, "toggled", G_CALLBACK(dir_mode_toggled_cb), w );
        hig_workarea_add_row (t, &row, name, w, NULL);

        g_snprintf( name, sizeof(name), "<i>%s</i>", _("No Files Selected"));
        w = ui->size_lb = gtk_label_new (NULL);
        gtk_label_set_markup ( GTK_LABEL(w), name );
        gtk_misc_set_alignment( GTK_MISC(w), 0.0f, 0.5f );
        hig_workarea_add_row (t, &row, "", w, NULL);
        

    hig_workarea_add_section_divider( t, &row );
    hig_workarea_add_section_title (t, &row, _("Torrent"));
    hig_workarea_add_section_spacer (t, row, 3);

        g_snprintf( name, sizeof(name), _("Private to this Tracker") );
        w = ui->private_check = hig_workarea_add_wide_checkbutton( t, &row, name, FALSE );

        g_snprintf( name, sizeof(name), "%s:", _("Announce _URL"));
        w = ui->announce_entry = gtk_entry_new( );
        gtk_entry_set_text(GTK_ENTRY(w), "http://");
        hig_workarea_add_row (t, &row, name, w, NULL );

        g_snprintf( name, sizeof(name), "%s:", _("Commen_t"));
        w = ui->comment_entry = gtk_entry_new( );
        hig_workarea_add_row (t, &row, name, w, NULL );


    gtk_window_set_default_size( GTK_WINDOW(d), 400u, 0u );
    gtk_box_pack_start_defaults( GTK_BOX(GTK_DIALOG(d)->vbox), t );
    gtk_widget_show_all( GTK_DIALOG(d)->vbox );
    return d;
}
