/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 William Jon McCann <mccann@jhu.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib-object.h>

#include "gdm-slave-proxy.h"

#define GDM_SLAVE_PROXY_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GDM_TYPE_SLAVE_PROXY, GdmSlaveProxyPrivate))

struct GdmSlaveProxyPrivate
{
	char    *command;
	GPid     pid;
	guint    child_watch_id;
};

enum {
	PROP_0,
	PROP_COMMAND,
};

enum {
	EXITED,
	DIED,
	LAST_SIGNAL
};

static guint signals [LAST_SIGNAL] = { 0, };

static void	gdm_slave_proxy_class_init	(GdmSlaveProxyClass *klass);
static void	gdm_slave_proxy_init	        (GdmSlaveProxy      *slave);
static void	gdm_slave_proxy_finalize	(GObject            *object);

G_DEFINE_TYPE (GdmSlaveProxy, gdm_slave_proxy, G_TYPE_OBJECT)

/* adapted from gspawn.c */
static int
wait_on_child (int pid)
{
	int status;

 wait_again:
	if (waitpid (pid, &status, 0) < 0) {
		if (errno == EINTR) {
			goto wait_again;
		} else if (errno == ECHILD) {
			; /* do nothing, child already reaped */
		} else {
			g_debug ("waitpid () should not fail in 'GdmSpawnProxy'");
		}
	}

	return status;
}

static void
slave_died (GdmSlaveProxy *slave)
{
	int exit_status;

	if (slave->priv->pid < 0) {
		return;
	}

	g_debug ("Waiting on process %d", slave->priv->pid);
	exit_status = wait_on_child (slave->priv->pid);

	if (WIFEXITED (exit_status) && (WEXITSTATUS (exit_status) != 0)) {
		g_debug ("Wait on child process failed");
	} else {
		/* exited normally */
	}

	g_spawn_close_pid (slave->priv->pid);
	slave->priv->pid = -1;

	g_debug ("Slave died");
}

static void
child_watch (GPid           pid,
	     int            status,
	     GdmSlaveProxy *slave)
{
        g_debug ("child (pid:%d) done (%s:%d)",
                 (int) pid,
                 WIFEXITED (status) ? "status"
                 : WIFSIGNALED (status) ? "signal"
                 : "unknown",
                 WIFEXITED (status) ? WEXITSTATUS (status)
                 : WIFSIGNALED (status) ? WTERMSIG (status)
                 : -1);
        if (WIFEXITED (status)) {
                int code = WEXITSTATUS (status);
                g_signal_emit (slave, signals [EXITED], 0, code);
        } else if (WIFSIGNALED (status)) {
                int num = WTERMSIG (status);
                g_signal_emit (slave, signals [DIED], 0, num);
        }

        g_spawn_close_pid (slave->priv->pid);
	slave->priv->pid = -1;
}

static gboolean
spawn_slave (GdmSlaveProxy *slave)
{
	char	  **argv;
	gboolean    result;
	GError	   *error = NULL;

	result = FALSE;

	if (! g_shell_parse_argv (slave->priv->command, NULL, &argv, &error)) {
		g_warning ("Could not parse command: %s", error->message);
		g_error_free (error);
		goto out;
	}

	g_debug ("Running command: %s", slave->priv->command);

	error = NULL;
	result = g_spawn_async_with_pipes (NULL,
					   argv,
					   NULL,
					   G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
					   NULL,
					   NULL,
					   &slave->priv->pid,
					   NULL,
					   NULL,
					   NULL,
					   &error);

	if (! result) {
		g_warning ("Could not start command '%s': %s", slave->priv->command, error->message);
		g_error_free (error);
		g_strfreev (argv);
		goto out;
	}

	g_strfreev (argv);

	slave->priv->child_watch_id = g_child_watch_add (slave->priv->pid,
							 (GChildWatchFunc)child_watch,
							 slave);

	result = TRUE;

 out:

	return result;
}

static int
signal_pid (int pid,
	    int signal)
{
	int status = -1;

	/* perhaps block sigchld */
	g_debug ("Sending signal %d to pid %d", signal, pid);

	status = kill (pid, signal);

	if (status < 0) {
		if (errno == ESRCH) {
			g_warning ("Child process %lu was already dead.",
				   (unsigned long) pid);
		} else {
			g_warning ("Couldn't kill child process %lu: %s",
				   (unsigned long) pid,
				   g_strerror (errno));
		}
	}

	/* perhaps unblock sigchld */

	return status;
}

static void
kill_slave (GdmSlaveProxy *slave)
{
	if (slave->priv->pid <= 1) {
		return;
	}

	signal_pid (slave->priv->pid, SIGTERM);
}

gboolean
gdm_slave_proxy_start (GdmSlaveProxy *slave)
{
	spawn_slave (slave);

	return TRUE;
}

gboolean
gdm_slave_proxy_stop (GdmSlaveProxy *slave)
{
	g_debug ("Killing slave");

	kill_slave (slave);

	if (slave->priv->child_watch_id > 0) {
		g_source_remove (slave->priv->child_watch_id);
	}

	return TRUE;
}

void
gdm_slave_proxy_set_command (GdmSlaveProxy *slave,
			     const char    *command)
{
        g_free (slave->priv->command);
        slave->priv->command = g_strdup (command);
}

static void
gdm_slave_proxy_set_property (GObject      *object,
			guint	       prop_id,
			const GValue *value,
			GParamSpec   *pspec)
{
	GdmSlaveProxy *self;

	self = GDM_SLAVE_PROXY (object);

	switch (prop_id) {
	case PROP_COMMAND:
		gdm_slave_proxy_set_command (self, g_value_get_string (value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gdm_slave_proxy_get_property (GObject    *object,
				 guint       prop_id,
				 GValue	    *value,
				 GParamSpec *pspec)
{
	GdmSlaveProxy *self;

	self = GDM_SLAVE_PROXY (object);

	switch (prop_id) {
	case PROP_COMMAND:
		g_value_set_string (value, self->priv->command);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gdm_slave_proxy_class_init (GdmSlaveProxyClass *klass)
{
	GObjectClass    *object_class = G_OBJECT_CLASS (klass);

	object_class->get_property = gdm_slave_proxy_get_property;
	object_class->set_property = gdm_slave_proxy_set_property;
	object_class->finalize = gdm_slave_proxy_finalize;

	g_type_class_add_private (klass, sizeof (GdmSlaveProxyPrivate));

	g_object_class_install_property (object_class,
					 PROP_COMMAND,
					 g_param_spec_string ("command",
							      "command",
							      "command",
							      NULL,
							      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
	signals [EXITED] =
		g_signal_new ("exited",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (GdmSlaveProxyClass, exited),
			      NULL,
			      NULL,
			      g_cclosure_marshal_VOID__INT,
			      G_TYPE_NONE,
			      1,
			      G_TYPE_INT);

	signals [DIED] =
		g_signal_new ("died",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (GdmSlaveProxyClass, died),
			      NULL,
			      NULL,
			      g_cclosure_marshal_VOID__INT,
			      G_TYPE_NONE,
			      1,
			      G_TYPE_INT);
}

static void
gdm_slave_proxy_init (GdmSlaveProxy *slave)
{

	slave->priv = GDM_SLAVE_PROXY_GET_PRIVATE (slave);

	slave->priv->pid = -1;
}

static void
gdm_slave_proxy_finalize (GObject *object)
{
	GdmSlaveProxy *slave;

	g_return_if_fail (object != NULL);
	g_return_if_fail (GDM_IS_SLAVE_PROXY (object));

	slave = GDM_SLAVE_PROXY (object);

	g_return_if_fail (slave->priv != NULL);

	G_OBJECT_CLASS (gdm_slave_proxy_parent_class)->finalize (object);
}

GdmSlaveProxy *
gdm_slave_proxy_new (void)
{
	GObject *object;

	object = g_object_new (GDM_TYPE_SLAVE_PROXY,
			       NULL);

	return GDM_SLAVE_PROXY (object);
}
