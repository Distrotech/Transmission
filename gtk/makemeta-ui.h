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
 *
 * To be blunt, I'm happy to let the Transmission project use my
 * code because Transmission is free software.  Closed-source and/or
 * commercial projects may not use this file.
 */

#ifndef MAKE_META_UI__H
#define MAKE_META_UI__H

#include <gtk/gtk.h>
#include "transmission.h"

GtkWidget* make_meta_ui( GtkWindow * parent, tr_handle_t * handle );

#endif
