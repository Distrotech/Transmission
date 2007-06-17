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
#include "makemeta-ui.h"
#include "util.h"

typedef struct
{
    char torrent_name[2048];
    GtkWidget * size_lb;
    GtkWidget * announce_entry;
    GtkWidget * comment_entry;
    GtkWidget * progressbar;
    GtkWidget * private_check;
    GtkWidget * dialog;
    GtkWidget * progress_dialog;
    tr_metainfo_builder_t * builder;
    tr_handle_t * handle;
}
MakeMetaUI;

static void
freeMetaUI( gpointer p )
{
    MakeMetaUI * ui = (MakeMetaUI *) p;
    tr_metaInfoBuilderFree( ui->builder );
    memset( ui, ~0, sizeof(MakeMetaUI) );
    g_free( ui );
}

static void
progress_response_cb ( GtkDialog *d UNUSED, int response, gpointer user_data )
{
    MakeMetaUI * ui = (MakeMetaUI *) user_data;

    if( response == GTK_RESPONSE_CANCEL )
    {
        ui->builder->abortFlag = TRUE;
    }
    else
    {
        gtk_widget_destroy( ui->dialog );
    }
}

static gboolean
refresh_cb ( gpointer user_data )
{
    char buf[1024];
    double fraction;
    MakeMetaUI * ui = (MakeMetaUI *) user_data;
    GtkProgressBar * p = GTK_PROGRESS_BAR( ui->progressbar );

    fraction = (double)ui->builder->pieceIndex / ui->builder->pieceCount;
    gtk_progress_bar_set_fraction( p, fraction );
    g_snprintf( buf, sizeof(buf), "%s (%d%%)", ui->torrent_name, (int)(fraction*100 + 0.5));
    gtk_progress_bar_set_text( p, buf );

    if( ui->builder->isDone )
    {
        GtkWidget * w;

        if( ui->builder->failed )
        {
            w = gtk_message_dialog_new (GTK_WINDOW(ui->progress_dialog),
                                        GTK_DIALOG_DESTROY_WITH_PARENT,
                                        GTK_MESSAGE_ERROR,
                                        GTK_BUTTONS_CLOSE,
                                        _("Torrent creation failed.")); /* FIXME: tell why */
            gtk_dialog_run( GTK_DIALOG( w ) );
            gtk_widget_destroy( ui->progress_dialog );
        }
        else
        {
            GtkWidget * w = ui->progress_dialog;
            gtk_window_set_title (GTK_WINDOW(ui->progress_dialog), _("Torrent Created!"));
            gtk_dialog_set_response_sensitive (GTK_DIALOG(w), GTK_RESPONSE_CANCEL, FALSE);
            gtk_dialog_set_response_sensitive (GTK_DIALOG(w), GTK_RESPONSE_CLOSE, TRUE);
            gtk_progress_bar_set_text( p, buf );
        }
    }

    return !ui->builder->isDone;
}

static void
response_cb( GtkDialog* d, int response, gpointer user_data )
{
    MakeMetaUI * ui = (MakeMetaUI*) user_data;
    GtkWidget *w, *p, *fr;
    char *tmp;
    char buf[1024];

    if( response != GTK_RESPONSE_ACCEPT )
    {
        gtk_widget_destroy( GTK_WIDGET( d ) );
        return;
    }

    w = gtk_dialog_new_with_buttons( _("Making Torrent..."), 
                                     GTK_WINDOW(d),
                                     GTK_DIALOG_DESTROY_WITH_PARENT,
                                     GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                     GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE,
                                     NULL );
    g_signal_connect( w, "response", G_CALLBACK(progress_response_cb), ui );
    ui->progress_dialog = w;
    gtk_dialog_set_response_sensitive (GTK_DIALOG(w), GTK_RESPONSE_CLOSE, FALSE);

    tmp = g_path_get_basename (ui->builder->top);
    g_snprintf( ui->torrent_name, sizeof(ui->torrent_name), "%s.torrent", tmp );
    g_snprintf( buf, sizeof(buf), "%s (%d%%)", ui->torrent_name, 0);
    p = ui->progressbar = gtk_progress_bar_new ();
    gtk_progress_bar_set_text( GTK_PROGRESS_BAR(p), buf );
    fr = gtk_frame_new (NULL);
    gtk_frame_set_shadow_type (GTK_FRAME(fr), GTK_SHADOW_NONE);
    gtk_container_set_border_width( GTK_CONTAINER(fr), 20 );
    gtk_container_add (GTK_CONTAINER(fr), p);
    gtk_box_pack_start_defaults( GTK_BOX(GTK_DIALOG(w)->vbox), fr );
    gtk_widget_show_all ( w );
    g_free( tmp );

    tr_makeMetaInfo( ui->builder,
                     NULL, 
                     gtk_entry_get_text( GTK_ENTRY( ui->announce_entry ) ),
                     gtk_entry_get_text( GTK_ENTRY( ui->comment_entry ) ),
                     gtk_toggle_button_get_active( GTK_TOGGLE_BUTTON( ui->private_check ) ) );

    g_timeout_add ( 200, refresh_cb, ui );
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
    int fileCount = 0;
    uint64_t totalSize = 0;

    if( ui->builder ) {
        tr_metaInfoBuilderFree( ui->builder );
        ui->builder = NULL;
    }

    filename = gtk_file_chooser_get_filename( chooser );
    if( filename ) {
        ui->builder = tr_metaInfoBuilderCreate( ui->handle, filename );
        g_free( filename );
        fileCount = (int) ui->builder->fileCount;
        totalSize = ui->builder->totalSize;
    }

    pch = readablesize( totalSize );
    g_snprintf( buf, sizeof(buf), "<i>%s; %d %s</i>",
                pch, fileCount,
                ngettext("file", "files", fileCount));
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
    g_object_set_data_full( G_OBJECT(d), "ui", ui, freeMetaUI );
    ui->dialog = d;

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
