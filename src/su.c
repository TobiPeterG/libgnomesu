/* libgnomesu - Library for providing superuser privileges to GNOME apps.
 * Copyright (C) 2003  Hongli Lai
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef _SU_C_
#define _SU_C_

#include <gtk/gtk.h>
#include <glade/glade.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>

#include <libintl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pwd.h>
#include <unistd.h>
#include <string.h>

#include "su.h"
#include "utils.h"
#include "libgnomesu.h"

G_BEGIN_DECLS


#undef _
#define _(x) dgettext (GETTEXT_PACKAGE, x)


typedef struct
{
	GladeXML *xml;
	GtkWidget *win;		/* the window */
	GtkWidget *pass;	/* the password entry */
	GtkWidget *command;	/* the 'Command to run' label */
	GtkWidget *verify;	/* the "verifying password..." label */
	GdkCursor *watch;	/* The watch busy cursor */
	guint tries;
} SuGUI;


static gboolean
pass_changed (GtkEntry *entry, GtkWidget *ok)
{
	gtk_widget_set_sensitive (ok, gtk_entry_get_text (entry) != NULL
		&& strlen (gtk_entry_get_text (entry)) > 0);
	return FALSE;
}


static SuGUI *
init_gui (gchar *command, gchar *user)
{
	SuGUI *gui;

	gui = (SuGUI *) g_new0 (SuGUI, 1);
	gui->xml = __libgnomesu_load_glade ("su.glade");
	glade_xml_signal_autoconnect (gui->xml);

	gui->win = glade_xml_get_widget (gui->xml, "SuDialog");
	gtk_widget_realize (gui->win);
	gui->pass = glade_xml_get_widget (gui->xml, "password");
	g_signal_connect (gui->pass, "changed", G_CALLBACK (pass_changed),
		glade_xml_get_widget (gui->xml, "ok"));
	gui->command = glade_xml_get_widget (gui->xml, "command");
	gtk_label_set_text (GTK_LABEL (gui->command), command);
	gui->verify = glade_xml_get_widget (gui->xml, "verifying");
	gui->watch = gdk_cursor_new (GDK_WATCH);

	if (strcmp (user, "root") != 0)
	{
		GtkWidget *passLabel, *info;
		gchar *tmp;

		passLabel = glade_xml_get_widget (gui->xml, "passLabel");
		info = glade_xml_get_widget (gui->xml, "info");

		tmp = g_strdup_printf (_("%s's _password:"), user);
		gtk_label_set_text_with_mnemonic (GTK_LABEL (passLabel), tmp);
		g_free (tmp);

		tmp = g_strdup_printf (_(
			"<span weight=\"bold\">The requested action needs further authentication.</span>\n"
			"Please enter %s's password and click Run to continue."),
			user);
		gtk_label_set_markup (GTK_LABEL (info), tmp);
		g_free (tmp);
	}

	gtk_widget_show (gui->win);

	return gui;
}


static void
clear_entry (GtkWidget *entry)
{
	gchar *blank;

	/* Make a pathetic stab at clearing the GtkEntry field memory */
	blank = (gchar *) gtk_entry_get_text (GTK_ENTRY (entry));
	if (blank && strlen (blank))
		memset (blank, ' ', strlen (blank));

	blank = g_strdup (blank);
	if (strlen (blank))
		memset (blank, ' ', strlen (blank));

	gtk_entry_set_text (GTK_ENTRY (entry), blank);
	gtk_entry_set_text (GTK_ENTRY (entry), "");
}


static void
fini_gui (SuGUI *gui)
{
	if (!gui) return;

	clear_entry (gui->pass);
	gtk_widget_destroy (gui->win);
	g_object_unref (gui->xml);
	gdk_cursor_unref (gui->watch);
	g_free (gui);
	while (gtk_events_pending ())
		gtk_main_iteration ();
}


static void
get_password (SuGUI *gui, gchar **password, gboolean previous_was_incorrect)
{
	gint response;

	if (previous_was_incorrect)
	{
		if (gui->tries >= 2)
			gtk_label_set_markup (GTK_LABEL (gui->verify),
				_("<span style=\"italic\" weight=\"bold\">Incorrect password, please try again. "
				  "You have one more chance.</span>"));
		else
			gtk_label_set_markup (GTK_LABEL (gui->verify),
				_("<span style=\"italic\" weight=\"bold\">Incorrect password, please try again.</span>"));
		gtk_widget_show (gui->verify);
	} else
		gtk_widget_hide (gui->verify);
	gtk_widget_set_sensitive (gui->win, TRUE);
	gdk_window_set_cursor (gui->win->window, NULL);
	gtk_widget_grab_focus (gui->pass);
	response = gtk_dialog_run (GTK_DIALOG (gui->win));
	
	if (response == GTK_RESPONSE_OK)
	{
		gui->tries++;
		gtk_label_set_markup (GTK_LABEL (gui->verify),
			_("<span style=\"italic\" weight=\"bold\">Please wait, verifying password...</span>"));
		gtk_widget_show (gui->verify);
		gdk_window_set_cursor (gui->win->window, gui->watch);
		gtk_widget_set_sensitive (gui->win, FALSE);

		*password = g_strdup (gtk_entry_get_text (GTK_ENTRY (gui->pass)));
	}

	clear_entry (gui->pass);
	while (gtk_events_pending ())
		gtk_main_iteration ();
}


/* Show an error message */
static void
bomb (SuGUI *gui, gchar *format, ...)
{
	GtkWidget *dialog, *win = NULL;
	va_list ap;
	gchar *msg;

	va_start (ap, format);
	msg = g_strdup_vprintf (format, ap);
	va_end (ap);

	if (gui)
		win = gui->win;
	dialog = gtk_message_dialog_new (GTK_WINDOW (win),
		GTK_DIALOG_MODAL,
		GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
		msg);
	gtk_window_set_resizable (GTK_WINDOW (dialog), FALSE);
	gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (dialog);
}


static gboolean
detect (gchar *exe, const gchar *user)
{
	struct stat buf;

	/* Check whether gnomesu-backend is present and setuid root */
	if (stat (LIBEXEC "/gnomesu-backend", &buf) == -1)
		return FALSE;
	else
		return (buf.st_uid == 0) && (buf.st_mode & S_ISUID);
}


static gboolean
spawn_async (gchar *user, gchar **argv, int *pid)
{
	int mypid, parent_pipe[2], child_pipe[2];

	g_return_val_if_fail (argv != NULL, FALSE);

	if (!user)
		user = "root";


	if (pipe (parent_pipe) == -1)
		return FALSE;
	if (pipe (child_pipe) == -1)
	{
		close (parent_pipe[0]);
		close (parent_pipe[1]);
		return FALSE;
	}


	mypid = fork ();
	switch (mypid)
	{
	case -1: /* error */
		close (parent_pipe[0]);
		close (parent_pipe[1]);
		close (child_pipe[0]);
		close (child_pipe[1]);
		return FALSE;
	case 0: /* child */
	    {
	        gchar **su_argv;
	        guint i, c;

		close (child_pipe[1]);
		close (parent_pipe[0]);

		c = __libgnomesu_count_args (argv);
		su_argv = g_new0 (gchar *, c + 5);
		su_argv[0] = LIBEXEC "/gnomesu-backend";
		su_argv[1] = g_strdup_printf ("%d", child_pipe[0]);
		su_argv[2] = g_strdup_printf ("%d", parent_pipe[1]);
		su_argv[3] = user;
		for (i = 0; i < c; i++)
			su_argv[i + 4] = argv[i];

		putenv ("_GNOMESU_BACKEND_START=1");
		execv (LIBEXEC "/gnomesu-backend", su_argv);
		_exit (1);
		break;
	    }
	default: /* parent */
	    {
		gchar buf[1024];
		FILE *f;
		int status;
		SuGUI *gui = NULL;
		gboolean previous_incorrect = FALSE;

		close (parent_pipe[1]);
		close (child_pipe[0]);
		f = fdopen (parent_pipe[0], "r");
		if (!f)
			return FALSE;

		while (!feof (f) && fgets (buf, sizeof (buf), f))
		{
			if (strcmp (buf, "DONE\n") == 0)
			{
				fini_gui (gui);
				fclose (f);
				close (parent_pipe[0]);
				close (child_pipe[1]);
				if (pid)
					*pid = mypid;
				return TRUE;
			}
			else if (strcmp (buf, "INCORRECT_PASSWORD\n") == 0)
			{
				previous_incorrect = TRUE;
			}
			else if (strcmp (buf, "ASK_PASS\n") == 0)
			{
				gchar *password = NULL, *commandline;

				if (!gui)
				{
					commandline = __libgnomesu_create_command (argv);
					gui = init_gui (commandline, user);
					g_free (commandline);
				}

				get_password (gui, &password, previous_incorrect);
				if (!password)
					break;

				write (child_pipe[1], password, strlen (password));
				memset (password, 0, strlen (password));
				g_free (password);
				write (child_pipe[1], "\n", 1);
			}

			/* These are all errors */
			else if (strcmp (buf, "PASSWORD_FAIL\n") == 0)
			{
				break;
			} else if (strcmp (buf, "NO_SUCH_USER\n") == 0)
			{
				bomb (gui, _("User '%s' doesn't exist."),
					user);
				break;
			} else if (strcmp (buf, "ERROR\n") == 0)
			{
				bomb (gui, _("An unknown error occured while authenticating."));
				break;
			} else if (strcmp (buf, "DENIED\n") == 0)
			{	
				bomb (gui, _("You do not have permission to authenticate."));
				break;
			} else
				break;
		}

		fini_gui (gui);
		fclose (f);
		close (child_pipe[1]);

		while (waitpid (mypid, &status, WNOHANG) == 0)
		{
			while (gtk_events_pending ())
				gtk_main_iteration ();
			usleep (100000);
		}

		return FALSE;
	    }
	}

	return FALSE;
}


GnomeSuService *
__gnomesu_su_service_new ()
{
	GnomeSuService *service;

	service = (GnomeSuService *) g_new0 (GnomeSuService, 1);
	service->detect = detect;
	service->spawn_async = spawn_async;
	return service;
}


G_END_DECLS

#endif /* _SU_C_ */