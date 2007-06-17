/*
 * This file Copyright (C) 2007 Charles Kerr <charles@rebelbase.com>
 *
 * This file is available under the
 * Attribution-Noncommercial-Share Alike 3.0 License.
 * Overview:  http://creativecommons.org/licenses/by-nc-sa/3.0/
 * Full text: http://creativecommons.org/licenses/by-nc-sa/3.0/legalcode
 *
 * Loosely speaking, you're free to use and adapt this work
 * provided that it's for noncommercial uses and that you share your
 * changes under a compatible license.
 */

#ifndef TR_ACTIONS_H
#define TR_ACTIONS_H

#include <gtk/gtk.h>

void actions_init ( GtkUIManager * ui_manager, gpointer callback_user_data );

void action_activate ( const char * name );

void action_sensitize ( const char * name, gboolean b );

void action_toggle ( const char * name, gboolean b );

GtkWidget* action_get_widget ( const char * path );

#endif
