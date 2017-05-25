/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007-2017 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <locale.h>
#include <glib/gi18n.h>
#include <appstream-glib.h>

#include "gs-app-private.h"
#include "gs-app-list-private.h"
#include "gs-category-private.h"
#include "gs-plugin-loader.h"
#include "gs-plugin.h"
#include "gs-plugin-event.h"
#include "gs-plugin-job-private.h"
#include "gs-plugin-private.h"
#include "gs-utils.h"

#define GS_PLUGIN_LOADER_UPDATES_CHANGED_DELAY	3	/* s */
#define GS_PLUGIN_LOADER_RELOAD_DELAY		5	/* s */

typedef struct
{
	GPtrArray		*plugins;
	GPtrArray		*locations;
	gchar			*locale;
	gchar			*language;
	GsAppList		*global_cache;
	AsProfile		*profile;
	SoupSession		*soup_session;
	GPtrArray		*auth_array;
	GPtrArray		*file_monitors;
	GsPluginStatus		 global_status_last;

	GMutex			 pending_apps_mutex;
	GPtrArray		*pending_apps;

	GSettings		*settings;

	GMutex			 events_by_id_mutex;
	GHashTable		*events_by_id;		/* unique-id : GsPluginEvent */

	gchar			**compatible_projects;
	guint			 scale;

	guint			 updates_changed_id;
	guint			 reload_id;
	GHashTable		*disallow_updates;	/* GsPlugin : const char *name */

	GNetworkMonitor		*network_monitor;
	gulong			 network_changed_handler;
} GsPluginLoaderPrivate;

typedef struct {
	GsApp	*proxy;
	GsApp	*app;
	guint	total_apps;
	guint	app_index;
	gulong	progress_handler_id;
} GsProxyUpdateHelper;

static void gs_plugin_loader_monitor_network (GsPluginLoader *plugin_loader);

G_DEFINE_TYPE_WITH_PRIVATE (GsPluginLoader, gs_plugin_loader, G_TYPE_OBJECT)

enum {
	SIGNAL_STATUS_CHANGED,
	SIGNAL_PENDING_APPS_CHANGED,
	SIGNAL_UPDATES_CHANGED,
	SIGNAL_RELOAD,
	SIGNAL_LAST
};

enum {
	PROP_0,
	PROP_EVENTS,
	PROP_ALLOW_UPDATES,
	PROP_NETWORK_AVAILABLE,
	PROP_LAST
};

static guint signals [SIGNAL_LAST] = { 0 };

typedef void		 (*GsPluginFunc)		(GsPlugin	*plugin);
typedef gboolean	 (*GsPluginSetupFunc)		(GsPlugin	*plugin,
							 GCancellable	*cancellable,
							 GError		**error);
typedef gboolean	 (*GsPluginSearchFunc)		(GsPlugin	*plugin,
							 gchar		**value,
							 GsAppList	*list,
							 GCancellable	*cancellable,
							 GError		**error);
typedef gboolean	 (*GsPluginCategoryFunc)	(GsPlugin	*plugin,
							 GsCategory	*category,
							 GsAppList	*list,
							 GCancellable	*cancellable,
							 GError		**error);
typedef gboolean	 (*GsPluginGetRecentFunc)	(GsPlugin	*plugin,
							 GsAppList	*list,
							 guint64	 age,
							 GCancellable	*cancellable,
							 GError		**error);
typedef gboolean	 (*GsPluginResultsFunc)		(GsPlugin	*plugin,
							 GsAppList	*list,
							 GCancellable	*cancellable,
							 GError		**error);
typedef gboolean	 (*GsPluginCategoriesFunc)	(GsPlugin	*plugin,
							 GPtrArray	*list,
							 GCancellable	*cancellable,
							 GError		**error);
typedef gboolean	 (*GsPluginActionFunc)		(GsPlugin	*plugin,
							 GsApp		*app,
							 GCancellable	*cancellable,
							 GError		**error);
typedef gboolean	 (*GsPluginPurchaseFunc)	(GsPlugin	*plugin,
							 GsApp		*app,
							 GsPrice	*price,
							 GCancellable	*cancellable,
							 GError		**error);
typedef gboolean	 (*GsPluginReviewFunc)		(GsPlugin	*plugin,
							 GsApp		*app,
							 AsReview	*review,
							 GCancellable	*cancellable,
							 GError		**error);
typedef gboolean	 (*GsPluginAuthFunc)		(GsPlugin	*plugin,
							 GsAuth		*auth,
							 GCancellable	*cancellable,
							 GError		**error);
typedef gboolean	 (*GsPluginRefineFunc)		(GsPlugin	*plugin,
							 GsAppList	*list,
							 GsPluginRefineFlags refine_flags,
							 GCancellable	*cancellable,
							 GError		**error);
typedef gboolean	 (*GsPluginRefineAppFunc)	(GsPlugin	*plugin,
							 GsApp		*app,
							 GsPluginRefineFlags refine_flags,
							 GCancellable	*cancellable,
							 GError		**error);
typedef gboolean	 (*GsPluginRefineWildcardFunc)	(GsPlugin	*plugin,
							 GsApp		*app,
							 GsAppList	*list,
							 GsPluginRefineFlags refine_flags,
							 GCancellable	*cancellable,
							 GError		**error);
typedef gboolean	 (*GsPluginRefreshFunc)		(GsPlugin	*plugin,
							 guint		 cache_age,
							 GsPluginRefreshFlags refresh_flags,
							 GCancellable	*cancellable,
							 GError		**error);
typedef gboolean	 (*GsPluginFileToAppFunc)	(GsPlugin	*plugin,
							 GsAppList	*list,
							 GFile		*file,
							 GCancellable	*cancellable,
							 GError		**error);
typedef gboolean	 (*GsPluginUrlToAppFunc)	(GsPlugin	*plugin,
							 GsAppList	*list,
							 const gchar	*url,
							 GCancellable	*cancellable,
							 GError		**error);
typedef gboolean	 (*GsPluginUpdateFunc)		(GsPlugin	*plugin,
							 GsAppList	*apps,
							 GCancellable	*cancellable,
							 GError		**error);
typedef void		 (*GsPluginAdoptAppFunc)	(GsPlugin	*plugin,
							 GsApp		*app);

/* async helper */
typedef struct {
	GsPluginLoader			*plugin_loader;
	const gchar			*function_name;
	const gchar			*function_name_parent;
	GPtrArray			*catlist;
	GsPluginJob			*plugin_job;
	gboolean			 anything_ran;
} GsPluginLoaderHelper;

static GsPluginLoaderHelper *
gs_plugin_loader_helper_new (GsPluginLoader *plugin_loader, GsPluginJob *plugin_job)
{
	GsPluginLoaderHelper *helper = g_slice_new0 (GsPluginLoaderHelper);
	GsPluginAction action = gs_plugin_job_get_action (plugin_job);
	helper->plugin_loader = g_object_ref (plugin_loader);
	helper->plugin_job = g_object_ref (plugin_job);
	helper->function_name = gs_plugin_action_to_function_name (action);
	return helper;
}

static void
gs_plugin_loader_helper_free (GsPluginLoaderHelper *helper)
{
	g_object_unref (helper->plugin_loader);
	if (helper->plugin_job != NULL)
		g_object_unref (helper->plugin_job);
	if (helper->catlist != NULL)
		g_ptr_array_unref (helper->catlist);
	g_slice_free (GsPluginLoaderHelper, helper);
}

static void
gs_plugin_loader_job_debug (GsPluginLoaderHelper *helper)
{
	g_autofree gchar *str = gs_plugin_job_to_string (helper->plugin_job);
	g_info ("%s", str);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsPluginLoaderHelper, gs_plugin_loader_helper_free)

static gint
gs_plugin_loader_app_sort_name_cb (GsApp *app1, GsApp *app2, gpointer user_data)
{
	return g_strcmp0 (gs_app_get_name (app1),
			  gs_app_get_name (app2));
}

GsPlugin *
gs_plugin_loader_find_plugin (GsPluginLoader *plugin_loader,
			      const gchar *plugin_name)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);

	for (guint i = 0; i < priv->plugins->len; i++) {
		GsPlugin *plugin = g_ptr_array_index (priv->plugins, i);
		if (g_strcmp0 (gs_plugin_get_name (plugin), plugin_name) == 0)
			return plugin;
	}
	return NULL;
}

static void
gs_plugin_loader_action_start (GsPluginLoader *plugin_loader,
			       GsPlugin *plugin,
			       gboolean exclusive)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	guint i;

	/* set plugin as SELF and all plugins as OTHER */
	gs_plugin_action_start (plugin, exclusive);
	for (i = 0; i < priv->plugins->len; i++) {
		GsPlugin *plugin_tmp;
		plugin_tmp = g_ptr_array_index (priv->plugins, i);
		if (!gs_plugin_get_enabled (plugin_tmp))
			continue;
		gs_plugin_set_running_other (plugin_tmp, TRUE);
	}
}

static void
gs_plugin_loader_action_stop (GsPluginLoader *plugin_loader, GsPlugin *plugin)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	guint i;

	/* clear plugin as SELF and all plugins as OTHER */
	gs_plugin_action_stop (plugin);
	for (i = 0; i < priv->plugins->len; i++) {
		GsPlugin *plugin_tmp;
		plugin_tmp = g_ptr_array_index (priv->plugins, i);
		if (!gs_plugin_get_enabled (plugin_tmp))
			continue;
		gs_plugin_set_running_other (plugin_tmp, FALSE);
	}
}

static gboolean
gs_plugin_loader_notify_idle_cb (gpointer user_data)
{
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (user_data);
	g_object_notify (G_OBJECT (plugin_loader), "events");
	return FALSE;
}

static void
gs_plugin_loader_add_event (GsPluginLoader *plugin_loader, GsPluginEvent *event)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&priv->events_by_id_mutex);
	g_hash_table_insert (priv->events_by_id,
			     g_strdup (gs_plugin_event_get_unique_id (event)),
			     g_object_ref (event));
	g_idle_add (gs_plugin_loader_notify_idle_cb, plugin_loader);
}

/* if the error is worthy of notifying then create a plugin event */
static void
gs_plugin_loader_create_event_from_error (GsPluginLoader *plugin_loader,
					  GsPluginAction action,
					  GsPlugin *plugin,
					  GsApp *app,
					  const GError *error)
{
	guint i;
	g_autoptr(GsApp) origin = NULL;
	g_auto(GStrv) split = NULL;
	g_autoptr(GsPluginEvent) event = NULL;

	/* invalid */
	if (error == NULL)
		return;
	if (error->domain != GS_PLUGIN_ERROR) {
		g_critical ("not GsPlugin error from plugin %s %s:%i: %s",
			    gs_plugin_get_name (plugin),
			    g_quark_to_string (error->domain),
			    error->code,
			    error->message);
		return;
	}

	/* create plugin event */
	event = gs_plugin_event_new ();
	if (app != NULL)
		gs_plugin_event_set_app (event, app);
	gs_plugin_event_set_error (event, error);
	gs_plugin_event_set_action (event, action);
	gs_plugin_event_add_flag (event, GS_PLUGIN_EVENT_FLAG_WARNING);

	/* can we find a unique ID */
	split = g_strsplit_set (error->message, "[]: ", -1);
	for (i = 0; split[i] != NULL; i++) {
		if (as_utils_unique_id_valid (split[i])) {
			origin = gs_plugin_cache_lookup (plugin, split[i]);
			if (origin != NULL) {
				g_debug ("found origin %s in error",
					 gs_app_get_unique_id (origin));
				gs_plugin_event_set_origin (event, origin);
				break;
			} else {
				g_debug ("no unique ID found for %s", split[i]);
			}
		}
	}

	/* add event to queue */
	gs_plugin_loader_add_event (plugin_loader, event);
}

static gboolean
gs_plugin_loader_is_error_fatal (GsPluginFailureFlags failure_flags,
				 const GError *err)
{
	if (failure_flags & GS_PLUGIN_FAILURE_FLAGS_FATAL_ANY)
		return TRUE;
	if (failure_flags & GS_PLUGIN_FAILURE_FLAGS_FATAL_AUTH) {
		if (g_error_matches (err, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_AUTH_REQUIRED))
			return TRUE;
		if (g_error_matches (err, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_AUTH_INVALID))
			return TRUE;
	}
	return FALSE;
}

static gboolean
gs_plugin_error_handle_failure (GsPluginLoaderHelper *helper,
				GsPlugin *plugin,
				const GError *error_local,
				GError **error)
{
	GsPluginFailureFlags flags;

	/* badly behaved plugin */
	if (error_local == NULL) {
		g_critical ("%s did not set error for %s",
			    gs_plugin_get_name (plugin),
			    helper->function_name);
		return TRUE;
	}

	/* abort early to allow main thread to process */
	flags = gs_plugin_job_get_failure_flags (helper->plugin_job);
	if (gs_plugin_loader_is_error_fatal (flags, error_local)) {
		if (error != NULL)
			*error = g_error_copy (error_local);
		return FALSE;
	}

	/* create event which is handled by the GsShell */
	if (flags & GS_PLUGIN_FAILURE_FLAGS_USE_EVENTS) {
		gs_plugin_loader_create_event_from_error (helper->plugin_loader,
							  gs_plugin_job_get_action (helper->plugin_job),
							  plugin,
							  gs_plugin_job_get_app (helper->plugin_job),
							  error_local);
	}

	/* fallback to console warning */
	if ((flags & GS_PLUGIN_FAILURE_FLAGS_NO_CONSOLE) == 0) {
		if (!g_error_matches (error_local,
				      GS_PLUGIN_ERROR,
				      GS_PLUGIN_ERROR_CANCELLED)) {
			g_warning ("failed to call %s on %s: %s",
				   helper->function_name,
				   gs_plugin_get_name (plugin),
				   error_local->message);
		}
	}

	return TRUE;
}

static void
gs_plugin_loader_run_adopt (GsPluginLoader *plugin_loader, GsAppList *list)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	guint i;
	guint j;

	/* go through each plugin in order */
	for (i = 0; i < priv->plugins->len; i++) {
		GsPluginAdoptAppFunc adopt_app_func = NULL;
		GsPlugin *plugin = g_ptr_array_index (priv->plugins, i);
		adopt_app_func = gs_plugin_get_symbol (plugin, "gs_plugin_adopt_app");
		if (adopt_app_func == NULL)
			continue;
		for (j = 0; j < gs_app_list_length (list); j++) {
			GsApp *app = gs_app_list_index (list, j);
			if (gs_app_get_management_plugin (app) != NULL)
				continue;
			if (gs_app_has_quirk (app, AS_APP_QUIRK_MATCH_ANY_PREFIX))
				continue;
			gs_plugin_loader_action_start (plugin_loader, plugin, FALSE);
			adopt_app_func (plugin, app);
			gs_plugin_loader_action_stop (plugin_loader, plugin);
			if (gs_app_get_management_plugin (app) != NULL) {
				g_debug ("%s adopted %s",
					 gs_plugin_get_name (plugin),
					 gs_app_get_unique_id (app));
			}
		}
	}
	for (j = 0; j < gs_app_list_length (list); j++) {
		GsApp *app = gs_app_list_index (list, j);
		if (gs_app_get_management_plugin (app) != NULL)
			continue;
		if (gs_app_has_quirk (app, AS_APP_QUIRK_MATCH_ANY_PREFIX))
			continue;
		g_debug ("nothing adopted %s", gs_app_get_unique_id (app));
	}
}

static gint
gs_plugin_loader_review_score_sort_cb (gconstpointer a, gconstpointer b)
{
	AsReview *ra = *((AsReview **) a);
	AsReview *rb = *((AsReview **) b);
	if (as_review_get_priority (ra) < as_review_get_priority (rb))
		return 1;
	if (as_review_get_priority (ra) > as_review_get_priority (rb))
		return -1;
	return 0;
}

static gboolean
gs_plugin_loader_call_vfunc (GsPluginLoaderHelper *helper,
			     GsPlugin *plugin,
			     GsApp *app,
			     GsAppList *list,
			     GCancellable *cancellable,
			     GError **error)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (helper->plugin_loader);
	GsPluginAction action = gs_plugin_job_get_action (helper->plugin_job);
	gboolean ret = TRUE;
	gpointer func = NULL;
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GTimer) timer = g_timer_new ();
	g_autoptr(AsProfileTask) ptask = NULL;

	/* load the possible symbol */
	func = gs_plugin_get_symbol (plugin, helper->function_name);
	if (func == NULL)
		return TRUE;

	/* profile */
	if (g_strcmp0 (helper->function_name, "gs_plugin_refine_app") != 0) {
		if (helper->function_name_parent == NULL) {
			ptask = as_profile_start (priv->profile,
						  "GsPlugin::%s(%s)",
						  gs_plugin_get_name (plugin),
						  helper->function_name);
		} else {
			ptask = as_profile_start (priv->profile,
						  "GsPlugin::%s(%s;%s)",
						  gs_plugin_get_name (plugin),
						  helper->function_name_parent,
						  helper->function_name);
		}
		g_assert (ptask != NULL);
	}

	/* fallback if unset */
	if (app == NULL)
		app = gs_plugin_job_get_app (helper->plugin_job);
	if (list == NULL)
		list = gs_plugin_job_get_list (helper->plugin_job);

	/* run the correct vfunc */
	gs_plugin_loader_action_start (helper->plugin_loader, plugin, FALSE);
	switch (action) {
	case GS_PLUGIN_ACTION_INITIALIZE:
	case GS_PLUGIN_ACTION_DESTROY:
		{
			GsPluginFunc plugin_func = func;
			plugin_func (plugin);
		}
		break;
	case GS_PLUGIN_ACTION_SETUP:
		{
			GsPluginSetupFunc plugin_func = func;
			ret = plugin_func (plugin, cancellable, &error_local);
		}
		break;
	case GS_PLUGIN_ACTION_REFINE:
		if (g_strcmp0 (helper->function_name, "gs_plugin_refine_wildcard") == 0) {
			GsPluginRefineWildcardFunc plugin_func = func;
			ret = plugin_func (plugin, app, list,
					   gs_plugin_job_get_refine_flags (helper->plugin_job),
					   cancellable, &error_local);
		} else if (g_strcmp0 (helper->function_name, "gs_plugin_refine_app") == 0) {
			GsPluginRefineAppFunc plugin_func = func;
			ret = plugin_func (plugin, app,
					   gs_plugin_job_get_refine_flags (helper->plugin_job),
					   cancellable, &error_local);
		} else if (g_strcmp0 (helper->function_name, "gs_plugin_refine") == 0) {
			GsPluginRefineFunc plugin_func = func;
			ret = plugin_func (plugin, list,
					   gs_plugin_job_get_refine_flags (helper->plugin_job),
					   cancellable, &error_local);
		} else {
			g_critical ("function_name %s invalid for %s",
				    helper->function_name,
				    gs_plugin_action_to_string (action));
		}
		break;
	case GS_PLUGIN_ACTION_UPDATE:
		if (g_strcmp0 (helper->function_name, "gs_plugin_update_app") == 0) {
			GsPluginActionFunc plugin_func = func;
			ret = plugin_func (plugin, app, cancellable, &error_local);
		} else if (g_strcmp0 (helper->function_name, "gs_plugin_update") == 0) {
			GsPluginUpdateFunc plugin_func = func;
			ret = plugin_func (plugin, list, cancellable, &error_local);
		} else {
			g_critical ("function_name %s invalid for %s",
				    helper->function_name,
				    gs_plugin_action_to_string (action));
		}
		break;
	case GS_PLUGIN_ACTION_INSTALL:
	case GS_PLUGIN_ACTION_REMOVE:
	case GS_PLUGIN_ACTION_SET_RATING:
	case GS_PLUGIN_ACTION_UPGRADE_DOWNLOAD:
	case GS_PLUGIN_ACTION_UPGRADE_TRIGGER:
	case GS_PLUGIN_ACTION_LAUNCH:
	case GS_PLUGIN_ACTION_UPDATE_CANCEL:
	case GS_PLUGIN_ACTION_ADD_SHORTCUT:
	case GS_PLUGIN_ACTION_REMOVE_SHORTCUT:
		{
			GsPluginActionFunc plugin_func = func;
			ret = plugin_func (plugin, app, cancellable, &error_local);
		}
		break;
	case GS_PLUGIN_ACTION_PURCHASE:
		{
			GsPluginPurchaseFunc plugin_func = func;
			ret = plugin_func (plugin, app,
					   gs_plugin_job_get_price (helper->plugin_job),
					   cancellable, &error_local);
		}
		break;
	case GS_PLUGIN_ACTION_REVIEW_SUBMIT:
	case GS_PLUGIN_ACTION_REVIEW_UPVOTE:
	case GS_PLUGIN_ACTION_REVIEW_DOWNVOTE:
	case GS_PLUGIN_ACTION_REVIEW_REPORT:
	case GS_PLUGIN_ACTION_REVIEW_REMOVE:
	case GS_PLUGIN_ACTION_REVIEW_DISMISS:
		{
			GsPluginReviewFunc plugin_func = func;
			ret = plugin_func (plugin, app,
					   gs_plugin_job_get_review (helper->plugin_job),
					   cancellable, &error_local);
		}
		break;
	case GS_PLUGIN_ACTION_GET_RECENT:
		{
			GsPluginGetRecentFunc plugin_func = func;
			ret = plugin_func (plugin, list,
					   gs_plugin_job_get_age (helper->plugin_job),
					   cancellable, &error_local);
		}
		break;
	case GS_PLUGIN_ACTION_GET_UPDATES:
	case GS_PLUGIN_ACTION_GET_UPDATES_HISTORICAL:
	case GS_PLUGIN_ACTION_GET_DISTRO_UPDATES:
	case GS_PLUGIN_ACTION_GET_UNVOTED_REVIEWS:
	case GS_PLUGIN_ACTION_GET_SOURCES:
	case GS_PLUGIN_ACTION_GET_INSTALLED:
	case GS_PLUGIN_ACTION_GET_POPULAR:
	case GS_PLUGIN_ACTION_GET_FEATURED:
		{
			GsPluginResultsFunc plugin_func = func;
			ret = plugin_func (plugin, list, cancellable, &error_local);
		}
		break;
	case GS_PLUGIN_ACTION_SEARCH:
		{
			GsPluginSearchFunc plugin_func = func;
			g_auto(GStrv) tokens = NULL;
			tokens = as_utils_search_tokenize (gs_plugin_job_get_search (helper->plugin_job));
			ret = plugin_func (plugin, tokens, list,
					   cancellable, &error_local);
		}
		break;
	case GS_PLUGIN_ACTION_SEARCH_FILES:
	case GS_PLUGIN_ACTION_SEARCH_PROVIDES:
		{
			GsPluginSearchFunc plugin_func = func;
			gchar *search[2] = { gs_plugin_job_get_search (helper->plugin_job), NULL };
			ret = plugin_func (plugin, search, list,
					   cancellable, &error_local);
		}
		break;
	case GS_PLUGIN_ACTION_GET_CATEGORIES:
		{
			GsPluginCategoriesFunc plugin_func = func;
			ret = plugin_func (plugin, helper->catlist,
					   cancellable, &error_local);
		}
		break;
	case GS_PLUGIN_ACTION_GET_CATEGORY_APPS:
		{
			GsPluginCategoryFunc plugin_func = func;
			ret = plugin_func (plugin,
					   gs_plugin_job_get_category (helper->plugin_job),
					   list,
					   cancellable, &error_local);
		}
		break;
	case GS_PLUGIN_ACTION_REFRESH:
		{
			GsPluginRefreshFunc plugin_func = func;
			ret = plugin_func (plugin,
					   gs_plugin_job_get_age (helper->plugin_job),
					   gs_plugin_job_get_refresh_flags (helper->plugin_job),
					   cancellable, &error_local);
		}
		break;
	case GS_PLUGIN_ACTION_FILE_TO_APP:
		{
			GsPluginFileToAppFunc plugin_func = func;
			ret = plugin_func (plugin, list,
					   gs_plugin_job_get_file (helper->plugin_job),
					   cancellable, &error_local);
		}
		break;
	case GS_PLUGIN_ACTION_URL_TO_APP:
		{
			GsPluginUrlToAppFunc plugin_func = func;
			ret = plugin_func (plugin, list,
					   gs_plugin_job_get_search (helper->plugin_job),
					   cancellable, &error_local);
		}
		break;
	case GS_PLUGIN_ACTION_AUTH_LOGIN:
	case GS_PLUGIN_ACTION_AUTH_LOGOUT:
	case GS_PLUGIN_ACTION_AUTH_REGISTER:
	case GS_PLUGIN_ACTION_AUTH_LOST_PASSWORD:
		{
			GsPluginAuthFunc plugin_func = func;
			ret = plugin_func (plugin,
					   gs_plugin_job_get_auth (helper->plugin_job),
					   cancellable, &error_local);
		}
		break;
	default:
		g_critical ("no handler for %s", helper->function_name);
		break;
	}
	gs_plugin_loader_action_stop (helper->plugin_loader, plugin);
	if (!ret) {
		return gs_plugin_error_handle_failure (helper,
							plugin,
							error_local,
							error);
	}

	/* check the plugin didn't take too long */
	switch (action) {
	case GS_PLUGIN_ACTION_INITIALIZE:
	case GS_PLUGIN_ACTION_DESTROY:
	case GS_PLUGIN_ACTION_SETUP:
		if (g_timer_elapsed (timer, NULL) > 0.5f) {
			g_warning ("plugin %s took %.1f seconds to do %s",
				   gs_plugin_get_name (plugin),
				   g_timer_elapsed (timer, NULL),
				   gs_plugin_action_to_string (action));
		}
		break;
	default:
		if (g_timer_elapsed (timer, NULL) > 0.5f) {
			g_debug ("plugin %s took %.1f seconds to do %s",
				 gs_plugin_get_name (plugin),
				 g_timer_elapsed (timer, NULL),
				 gs_plugin_action_to_string (action));
			}
		break;
	}

	/* success */
	helper->anything_ran = TRUE;
	return TRUE;
}

static gboolean
gs_plugin_loader_run_refine_internal (GsPluginLoaderHelper *helper,
				      GsAppList *list,
				      GCancellable *cancellable,
				      GError **error)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (helper->plugin_loader);
	guint i;
	guint j;
	GPtrArray *addons;
	GPtrArray *related;
	GsApp *app;

	/* try to adopt each application with a plugin */
	gs_plugin_loader_run_adopt (helper->plugin_loader, list);

	/* run each plugin */
	for (i = 0; i < priv->plugins->len; i++) {
		g_autoptr(AsProfileTask) ptask = NULL;
		GsPlugin *plugin = g_ptr_array_index (priv->plugins, i);
		g_autoptr(GsAppList) app_list = NULL;

		/* run the batched plugin symbol then the per-app plugin */
		helper->function_name = "gs_plugin_refine";
		if (!gs_plugin_loader_call_vfunc (helper, plugin, NULL, list,
						  cancellable, error)) {
			return FALSE;
		}

		/* use a copy of the list for the loop because a function called
		 * on the plugin may affect the list which can lead to problems
		 * (e.g. inserting an app in the list on every call results in
		 * an infinite loop) */
		app_list = gs_app_list_copy (list);
		for (j = 0; j < gs_app_list_length (app_list); j++) {
			app = gs_app_list_index (app_list, j);
			if (!gs_app_has_quirk (app, AS_APP_QUIRK_MATCH_ANY_PREFIX)) {
				helper->function_name = "gs_plugin_refine_app";
			} else {
				helper->function_name = "gs_plugin_refine_wildcard";
			}
			if (!gs_plugin_loader_call_vfunc (helper, plugin, app, NULL,
							  cancellable, error)) {
				return FALSE;
			}
		}
		gs_plugin_status_update (plugin, NULL, GS_PLUGIN_STATUS_FINISHED);
	}

	/* ensure these are sorted by score */
	if (gs_plugin_job_has_refine_flags (helper->plugin_job,
						GS_PLUGIN_REFINE_FLAGS_REQUIRE_REVIEWS)) {
		GPtrArray *reviews;
		for (i = 0; i < gs_app_list_length (list); i++) {
			app = gs_app_list_index (list, i);
			reviews = gs_app_get_reviews (app);
			g_ptr_array_sort (reviews,
					  gs_plugin_loader_review_score_sort_cb);
		}
	}

	/* refine addons one layer deep */
	if (gs_plugin_job_has_refine_flags (helper->plugin_job,
						GS_PLUGIN_REFINE_FLAGS_REQUIRE_ADDONS)) {
		g_autoptr(GsAppList) addons_list = NULL;
		gs_plugin_job_remove_refine_flags (helper->plugin_job,
						   GS_PLUGIN_REFINE_FLAGS_REQUIRE_ADDONS |
						   GS_PLUGIN_REFINE_FLAGS_REQUIRE_REVIEWS |
						   GS_PLUGIN_REFINE_FLAGS_REQUIRE_REVIEW_RATINGS);
		addons_list = gs_app_list_new ();
		for (i = 0; i < gs_app_list_length (list); i++) {
			app = gs_app_list_index (list, i);
			addons = gs_app_get_addons (app);
			for (j = 0; j < addons->len; j++) {
				GsApp *addon = g_ptr_array_index (addons, j);
				g_debug ("refining app %s addon %s",
					 gs_app_get_id (app),
					 gs_app_get_id (addon));
				gs_app_list_add (addons_list, addon);
			}
		}
		if (gs_app_list_length (addons_list) > 0) {
			if (!gs_plugin_loader_run_refine_internal (helper,
								   addons_list,
								   cancellable,
								   error)) {
				return FALSE;
			}
		}
	}

	/* also do runtime */
	if (gs_plugin_job_has_refine_flags (helper->plugin_job,
						GS_PLUGIN_REFINE_FLAGS_REQUIRE_RUNTIME)) {
		g_autoptr(GsAppList) list2 = gs_app_list_new ();
		for (i = 0; i < gs_app_list_length (list); i++) {
			GsApp *runtime;
			app = gs_app_list_index (list, i);
			runtime = gs_app_get_runtime (app);
			if (runtime != NULL)
				gs_app_list_add (list2, runtime);
		}
		if (gs_app_list_length (list2) > 0) {
			if (!gs_plugin_loader_run_refine_internal (helper,
								   list2,
								   cancellable,
								   error)) {
				return FALSE;
			}
		}
	}

	/* also do related packages one layer deep */
	if (gs_plugin_job_has_refine_flags (helper->plugin_job,
						GS_PLUGIN_REFINE_FLAGS_REQUIRE_RELATED)) {
		g_autoptr(GsAppList) related_list = NULL;
		gs_plugin_job_remove_refine_flags (helper->plugin_job,
						   GS_PLUGIN_REFINE_FLAGS_REQUIRE_RELATED);
		related_list = gs_app_list_new ();
		for (i = 0; i < gs_app_list_length (list); i++) {
			app = gs_app_list_index (list, i);
			related = gs_app_get_related (app);
			for (j = 0; j < related->len; j++) {
				app = g_ptr_array_index (related, j);
				g_debug ("refining related: %s[%s]",
					 gs_app_get_id (app),
					 gs_app_get_source_default (app));
				gs_app_list_add (related_list, app);
			}
		}
		if (gs_app_list_length (related_list) > 0) {
			if (!gs_plugin_loader_run_refine_internal (helper,
								   related_list,
								   cancellable,
								   error)) {
				return FALSE;
			}
		}
	}

	/* success */
	return TRUE;
}

static gboolean
gs_plugin_loader_run_refine (GsPluginLoaderHelper *helper,
			     GsAppList *list,
			     GCancellable *cancellable,
			     GError **error)
{
	gboolean has_match_any_prefix = FALSE;
	gboolean ret;
	g_autoptr(GsAppList) freeze_list = NULL;
	g_autoptr(GsPluginLoaderHelper) helper2 = NULL;
	g_autoptr(GsPluginJob) plugin_job = NULL;

	/* nothing to do */
	if (gs_app_list_length (list) == 0)
		return TRUE;

	/* freeze all apps */
	freeze_list = gs_app_list_copy (list);
	for (guint i = 0; i < gs_app_list_length (freeze_list); i++) {
		GsApp *app = gs_app_list_index (freeze_list, i);
		g_object_freeze_notify (G_OBJECT (app));
	}

	/* first pass */
	plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_REFINE,
					 "list", list,
					 "refine-flags", gs_plugin_job_get_refine_flags (helper->plugin_job),
					 "failure-flags", gs_plugin_job_get_failure_flags (helper->plugin_job),
					 NULL);
	helper2 = gs_plugin_loader_helper_new (helper->plugin_loader, plugin_job);
	helper2->function_name_parent = helper->function_name;
	ret = gs_plugin_loader_run_refine_internal (helper2, list, cancellable, error);
	if (!ret)
		goto out;

	/* second pass for any unadopted apps */
	for (guint i = 0; i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);
		if (gs_app_has_quirk (app, AS_APP_QUIRK_MATCH_ANY_PREFIX)) {
			has_match_any_prefix = TRUE;
			break;
		}
	}
	if (has_match_any_prefix) {
		g_debug ("2nd resolve pass for unadopted wildcards");
		if (!gs_plugin_loader_run_refine_internal (helper2, list,
							   cancellable,
							   error))
			goto out;
	}

	/* remove any addons that have the same source as the parent app */
	for (guint i = 0; i < gs_app_list_length (list); i++) {
		g_autoptr(GPtrArray) to_remove = g_ptr_array_new ();
		GsApp *app = gs_app_list_index (list, i);
		GPtrArray *addons = gs_app_get_addons (app);

		/* find any apps with the same source */
		const gchar *pkgname_parent = gs_app_get_source_default (app);
		if (pkgname_parent == NULL)
			continue;
		for (guint j = 0; j < addons->len; j++) {
			GsApp *addon = g_ptr_array_index (addons, j);
			if (g_strcmp0 (gs_app_get_source_default (addon),
				       pkgname_parent) == 0) {
				g_debug ("%s has the same pkgname of %s as %s",
					 gs_app_get_unique_id (app),
					 pkgname_parent,
					 gs_app_get_unique_id (addon));
				g_ptr_array_add (to_remove, addon);
			}
		}

		/* remove any addons with the same source */
		for (guint j = 0; j < to_remove->len; j++) {
			GsApp *addon = g_ptr_array_index (to_remove, j);
			gs_app_remove_addon (app, addon);
		}
	}

out:
	/* now emit all the changed signals */
	for (guint i = 0; i < gs_app_list_length (freeze_list); i++) {
		GsApp *app = gs_app_list_index (freeze_list, i);
		g_object_thaw_notify (G_OBJECT (app));
	}
	return ret;
}

static void
gs_plugin_loader_job_sorted_truncation_again (GsPluginLoaderHelper *helper)
{
	GsAppListSortFunc sort_func;
	gpointer sort_func_data;

	/* not valid */
	if (gs_plugin_job_get_list (helper->plugin_job) == NULL)
		return;

	/* unset */
	sort_func = gs_plugin_job_get_sort_func (helper->plugin_job);
	if (sort_func == NULL)
		return;
	sort_func_data = gs_plugin_job_get_sort_func_data (helper->plugin_job);
	gs_app_list_sort (gs_plugin_job_get_list (helper->plugin_job), sort_func, sort_func_data);
}

static void
gs_plugin_loader_job_sorted_truncation (GsPluginLoaderHelper *helper)
{
	GsAppListSortFunc sort_func;
	guint max_results;
	GsAppList *list = gs_plugin_job_get_list (helper->plugin_job);

	/* not valid */
	if (list == NULL)
		return;

	/* unset */
	max_results = gs_plugin_job_get_max_results (helper->plugin_job);
	if (max_results == 0)
		return;

	/* already small enough */
	if (gs_app_list_length (list) <= max_results)
		return;

	/* nothing set */
	g_debug ("truncating results to %u from %u",
		 max_results, gs_app_list_length (list));
	sort_func = gs_plugin_job_get_sort_func (helper->plugin_job);
	if (sort_func == NULL) {
		g_warning ("no ->sort_func() set for %s, using random!",
			   gs_plugin_action_to_string (gs_plugin_job_get_action (helper->plugin_job)));
		gs_app_list_randomize (list);
	} else {
		gpointer sort_func_data;
		sort_func_data = gs_plugin_job_get_sort_func_data (helper->plugin_job);
		gs_app_list_sort (list, sort_func, sort_func_data);
	}
	gs_app_list_truncate (list, max_results);
}

static gboolean
gs_plugin_loader_run_results (GsPluginLoaderHelper *helper,
			      GCancellable *cancellable,
			      GError **error)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (helper->plugin_loader);
	g_autoptr(AsProfileTask) ptask = NULL;

	/* profile */
	ptask = as_profile_start (priv->profile, "GsPlugin::*(%s)",
				  helper->function_name);
	g_assert (ptask != NULL);

	/* run each plugin */
	for (guint i = 0; i < priv->plugins->len; i++) {
		GsPlugin *plugin = g_ptr_array_index (priv->plugins, i);
		if (g_cancellable_set_error_if_cancelled (cancellable, error)) {
			gs_utils_error_convert_gio (error);
			return FALSE;
		}
		if (!gs_plugin_loader_call_vfunc (helper, plugin, NULL, NULL,
						  cancellable, error)) {
			return FALSE;
		}
		gs_plugin_status_update (plugin, NULL, GS_PLUGIN_STATUS_FINISHED);
	}
	return TRUE;
}

static const gchar *
gs_plugin_loader_get_app_str (GsApp *app)
{
	const gchar *id;

	/* first try the actual id */
	id = gs_app_get_unique_id (app);
	if (id != NULL)
		return id;

	/* then try the source */
	id = gs_app_get_source_default (app);
	if (id != NULL)
		return id;

	/* lastly try the source id */
	id = gs_app_get_source_id_default (app);
	if (id != NULL)
		return id;

	/* urmmm */
	return "<invalid>";
}

static gboolean
gs_plugin_loader_app_set_prio (GsApp *app, gpointer user_data)
{
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (user_data);
	GsPlugin *plugin;
	const gchar *tmp;

	/* if set, copy the priority */
	tmp = gs_app_get_management_plugin (app);
	if (tmp == NULL)
		return TRUE;
	plugin = gs_plugin_loader_find_plugin (plugin_loader, tmp);
	if (plugin == NULL)
		return TRUE;
	gs_app_set_priority (app, gs_plugin_get_priority (plugin));
	return TRUE;
}

static gboolean
gs_plugin_loader_app_is_valid_installed (GsApp *app, gpointer user_data)
{
	/* even without AppData, show things in progress */
	switch (gs_app_get_state (app)) {
	case AS_APP_STATE_INSTALLING:
	case AS_APP_STATE_REMOVING:
	case AS_APP_STATE_PURCHASING:
		return TRUE;
		break;
	default:
		break;
	}

	switch (gs_app_get_kind (app)) {
	case AS_APP_KIND_OS_UPGRADE:
	case AS_APP_KIND_CODEC:
	case AS_APP_KIND_FONT:
		g_debug ("app invalid as %s: %s",
			 as_app_kind_to_string (gs_app_get_kind (app)),
			 gs_plugin_loader_get_app_str (app));
		return FALSE;
		break;
	default:
		break;
	}

	/* ignore this crazy application */
	if (g_strcmp0 (gs_app_get_id (app), "gnome-system-monitor-kde.desktop") == 0) {
		g_debug ("Ignoring KDE version of %s", gs_app_get_id (app));
		return FALSE;
	}

	/* sanity check */
	if (!gs_app_is_installed (app)) {
		g_autofree gchar *tmp = gs_app_to_string (app);
		g_warning ("ignoring non-installed app %s", tmp);
		return FALSE;
	}

	return TRUE;
}

static gboolean
gs_plugin_loader_app_is_valid (GsApp *app, gpointer user_data)
{
	GsPluginLoaderHelper *helper = (GsPluginLoaderHelper *) user_data;

	/* never show addons */
	if (gs_app_get_kind (app) == AS_APP_KIND_ADDON) {
		g_debug ("app invalid as addon %s",
			 gs_plugin_loader_get_app_str (app));
		return FALSE;
	}

	/* never show CLI apps */
	if (gs_app_get_kind (app) == AS_APP_KIND_CONSOLE) {
		g_debug ("app invalid as console %s",
			 gs_plugin_loader_get_app_str (app));
		return FALSE;
	}

	/* don't show unknown state */
	if (gs_app_get_state (app) == AS_APP_STATE_UNKNOWN) {
		g_debug ("app invalid as state unknown %s",
			 gs_plugin_loader_get_app_str (app));
		return FALSE;
	}

	/* don't show unconverted unavailables */
	if (gs_app_get_kind (app) == AS_APP_KIND_UNKNOWN &&
		gs_app_get_state (app) == AS_APP_STATE_UNAVAILABLE) {
		g_debug ("app invalid as unconverted unavailable %s",
			 gs_plugin_loader_get_app_str (app));
		return FALSE;
	}

	/* don't show blacklisted apps */
	if (gs_app_has_category (app, "Blacklisted")) {
		g_debug ("app invalid as blacklisted %s",
			 gs_plugin_loader_get_app_str (app));
		return FALSE;
	}

	/* don't show sources */
	if (gs_app_get_kind (app) == AS_APP_KIND_SOURCE) {
		g_debug ("app invalid as source %s",
			 gs_plugin_loader_get_app_str (app));
		return FALSE;
	}

	/* don't show unknown kind */
	if (gs_app_get_kind (app) == AS_APP_KIND_UNKNOWN) {
		g_debug ("app invalid as kind unknown %s",
			 gs_plugin_loader_get_app_str (app));
		return FALSE;
	}

	/* don't show unconverted packages in the application view */
	if (!gs_plugin_job_has_refine_flags (helper->plugin_job,
						 GS_PLUGIN_REFINE_FLAGS_ALLOW_PACKAGES) &&
	    (gs_app_get_kind (app) == AS_APP_KIND_GENERIC)) {
//		g_debug ("app invalid as only a %s: %s",
//			 as_app_kind_to_string (gs_app_get_kind (app)),
//			 gs_plugin_loader_get_app_str (app));
		return FALSE;
	}

	/* don't show apps that do not have the required details */
	if (gs_app_get_name (app) == NULL) {
		g_debug ("app invalid as no name %s",
			 gs_plugin_loader_get_app_str (app));
		return FALSE;
	}
	if (gs_app_get_summary (app) == NULL) {
		g_debug ("app invalid as no summary %s",
			 gs_plugin_loader_get_app_str (app));
		return FALSE;
	}
	if (gs_app_get_kind (app) == AS_APP_KIND_DESKTOP &&
	    gs_app_get_pixbuf (app) == NULL) {
		g_debug ("app invalid as no pixbuf %s",
			 gs_plugin_loader_get_app_str (app));
		return FALSE;
	}
	return TRUE;
}

static gboolean
gs_plugin_loader_app_is_valid_updatable (GsApp *app, gpointer user_data)
{
	return gs_plugin_loader_app_is_valid (app, user_data) &&
		gs_app_is_updatable (app);
}

static gboolean
gs_plugin_loader_filter_qt_for_gtk (GsApp *app, gpointer user_data)
{
	/* hide the QT versions in preference to the GTK ones */
	if (g_strcmp0 (gs_app_get_id (app), "transmission-qt.desktop") == 0 ||
	    g_strcmp0 (gs_app_get_id (app), "nntpgrab_qt.desktop") == 0 ||
	    g_strcmp0 (gs_app_get_id (app), "gimagereader-qt4.desktop") == 0 ||
	    g_strcmp0 (gs_app_get_id (app), "gimagereader-qt5.desktop") == 0 ||
	    g_strcmp0 (gs_app_get_id (app), "nntpgrab_server_qt.desktop") == 0 ||
	    g_strcmp0 (gs_app_get_id (app), "hotot-qt.desktop") == 0) {
		g_debug ("removing QT version of %s",
			 gs_plugin_loader_get_app_str (app));
		return FALSE;
	}

	/* hide the KDE version in preference to the GTK one */
	if (g_strcmp0 (gs_app_get_id (app), "qalculate_kde.desktop") == 0) {
		g_debug ("removing KDE version of %s",
			 gs_plugin_loader_get_app_str (app));
		return FALSE;
	}

	/* hide the KDE version in preference to the Qt one */
	if (g_strcmp0 (gs_app_get_id (app), "kid3.desktop") == 0 ||
	    g_strcmp0 (gs_app_get_id (app), "kchmviewer.desktop") == 0) {
		g_debug ("removing KDE version of %s",
			 gs_plugin_loader_get_app_str (app));
		return FALSE;
	}
	return TRUE;
}

static gboolean
gs_plugin_loader_app_is_non_compulsory (GsApp *app, gpointer user_data)
{
	return !gs_app_has_quirk (app, AS_APP_QUIRK_COMPULSORY);
}

static gboolean
gs_plugin_loader_get_app_is_compatible (GsApp *app, gpointer user_data)
{
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (user_data);
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	const gchar *tmp;
	guint i;

	/* search for any compatible projects */
	tmp = gs_app_get_project_group (app);
	if (tmp == NULL)
		return TRUE;
	for (i = 0; priv->compatible_projects[i] != NULL; i++) {
		if (g_strcmp0 (tmp,  priv->compatible_projects[i]) == 0)
			return TRUE;
	}
	g_debug ("removing incompatible %s from project group %s",
		 gs_app_get_id (app), gs_app_get_project_group (app));
	return FALSE;
}

/**
 * gs_plugin_loader_get_event_by_id:
 * @list: A #GsAppList
 * @unique_id: A unique_id
 *
 * Finds the first matching event in the list using the usual wildcard
 * rules allowed in unique_ids.
 *
 * Returns: (transfer none): a #GsPluginEvent, or %NULL if not found
 **/
GsPluginEvent *
gs_plugin_loader_get_event_by_id (GsPluginLoader *plugin_loader, const gchar *unique_id)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&priv->events_by_id_mutex);
	return g_hash_table_lookup (priv->events_by_id, unique_id);
}

GsPluginEvent *
gs_plugin_loader_get_event_by_error (GsPluginLoader *plugin_loader, GsPluginError error_code)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&priv->events_by_id_mutex);
	g_autoptr(GList) values = g_hash_table_get_values (priv->events_by_id);

	/* find the one that matches the error code */
	for (GList *l = values; l != NULL; l = l->next) {
		GsPluginEvent *event = GS_PLUGIN_EVENT (l->data);
		const GError *error = gs_plugin_event_get_error (event);
		if (g_error_matches (error, GS_PLUGIN_ERROR, error_code))
			return event;
	}
	return NULL;
}

/******************************************************************************/

static gboolean
gs_plugin_loader_featured_debug (GsApp *app, gpointer user_data)
{
	if (g_strcmp0 (gs_app_get_id (app),
	    g_getenv ("GNOME_SOFTWARE_FEATURED")) == 0)
		return TRUE;
	return FALSE;
}

static gint
gs_plugin_loader_app_sort_kind_cb (GsApp *app1, GsApp *app2, gpointer user_data)
{
	if (gs_app_get_kind (app1) == AS_APP_KIND_DESKTOP)
		return -1;
	if (gs_app_get_kind (app2) == AS_APP_KIND_DESKTOP)
		return 1;
	return 0;
}

static gint
gs_plugin_loader_app_sort_match_value_cb (GsApp *app1, GsApp *app2, gpointer user_data)
{
	if (gs_app_get_match_value (app1) > gs_app_get_match_value (app2))
		return -1;
	if (gs_app_get_match_value (app1) < gs_app_get_match_value (app2))
		return 1;
	return 0;
}

/******************************************************************************/

static gboolean
gs_plugin_loader_convert_unavailable_app (GsApp *app, const gchar *search)
{
	GPtrArray *keywords;
	const gchar *keyword;
	guint i;
	g_autoptr(GString) tmp = NULL;

	/* is the search string one of the codec keywords */
	keywords = gs_app_get_keywords (app);
	for (i = 0; i < keywords->len; i++) {
		keyword = g_ptr_array_index (keywords, i);
		if (g_ascii_strcasecmp (search, keyword) == 0) {
			search = keyword;
			break;
		}
	}

	tmp = g_string_new ("");
	/* TRANSLATORS: this is when we know about an application or
	 * addon, but it can't be listed for some reason */
	g_string_append_printf (tmp, _("No addon codecs are available "
				"for the %s format."), search);
	g_string_append (tmp, "\n");
	g_string_append_printf (tmp, _("Information about %s, as well as options "
				"for how to get a codec that can play this format "
				"can be found on the website."), search);
	gs_app_set_summary_missing (app, tmp->str);
	gs_app_set_kind (app, AS_APP_KIND_GENERIC);
	gs_app_set_state (app, AS_APP_STATE_UNAVAILABLE);
	gs_app_set_size_installed (app, GS_APP_SIZE_UNKNOWABLE);
	gs_app_set_size_download (app, GS_APP_SIZE_UNKNOWABLE);
	return TRUE;
}

static void
gs_plugin_loader_convert_unavailable (GsAppList *list, const gchar *search)
{
	guint i;
	GsApp *app;

	for (i = 0; i < gs_app_list_length (list); i++) {
		app = gs_app_list_index (list, i);
		if (gs_app_get_kind (app) != AS_APP_KIND_GENERIC)
			continue;
		if (gs_app_get_state (app) != AS_APP_STATE_UNAVAILABLE)
			continue;
		if (gs_app_get_kind (app) != AS_APP_KIND_CODEC)
			continue;
		if (gs_app_get_url (app, AS_URL_KIND_MISSING) == NULL)
			continue;

		/* only convert the first unavailable codec */
		if (gs_plugin_loader_convert_unavailable_app (app, search))
			break;
	}
}

/**
 * gs_plugin_loader_job_process_finish:
 *
 * Return value: (element-type GsApp) (transfer full): A list of applications
 **/
GsAppList *
gs_plugin_loader_job_process_finish (GsPluginLoader *plugin_loader,
				     GAsyncResult *res,
				     GError **error)
{
	g_return_val_if_fail (GS_IS_PLUGIN_LOADER (plugin_loader), NULL);
	g_return_val_if_fail (G_IS_TASK (res), NULL);
	g_return_val_if_fail (g_task_is_valid (res, plugin_loader), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	gs_utils_error_convert_gio (error);
	return g_task_propagate_pointer (G_TASK (res), error);
}

/**
 * gs_plugin_loader_job_action_finish:
 *
 * Return value: success
 **/
gboolean
gs_plugin_loader_job_action_finish (GsPluginLoader *plugin_loader,
				     GAsyncResult *res,
				     GError **error)
{
	g_return_val_if_fail (GS_IS_PLUGIN_LOADER (plugin_loader), FALSE);
	g_return_val_if_fail (G_IS_TASK (res), FALSE);
	g_return_val_if_fail (g_task_is_valid (res, plugin_loader), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	return g_task_propagate_pointer (G_TASK (res), error) != NULL;
}

/******************************************************************************/

static gint
gs_plugin_loader_category_sort_cb (gconstpointer a, gconstpointer b)
{
	GsCategory *cata = GS_CATEGORY (*(GsCategory **) a);
	GsCategory *catb = GS_CATEGORY (*(GsCategory **) b);
	if (gs_category_get_score (cata) < gs_category_get_score (catb))
		return 1;
	if (gs_category_get_score (cata) > gs_category_get_score (catb))
		return -1;
	return g_strcmp0 (gs_category_get_name (cata),
			  gs_category_get_name (catb));
}

static void
gs_plugin_loader_fix_category_all (GsCategory *category)
{
	GPtrArray *children;
	GsCategory *cat_all;
	guint i, j;

	/* set correct size */
	cat_all = gs_category_find_child (category, "all");
	if (cat_all == NULL)
		return;
	gs_category_set_size (cat_all, gs_category_get_size (category));

	/* add the desktop groups from all children */
	children = gs_category_get_children (category);
	for (i = 0; i < children->len; i++) {
		GPtrArray *desktop_groups;
		GsCategory *child;

		/* ignore the all category */
		child = g_ptr_array_index (children, i);
		if (g_strcmp0 (gs_category_get_id (child), "all") == 0)
			continue;

		/* add all desktop groups */
		desktop_groups = gs_category_get_desktop_groups (child);
		for (j = 0; j < desktop_groups->len; j++) {
			const gchar *tmp = g_ptr_array_index (desktop_groups, j);
			gs_category_add_desktop_group (cat_all, tmp);
		}
	}
}

static void
gs_plugin_loader_job_get_categories_thread_cb (GTask *task,
					      gpointer object,
					      gpointer task_data,
					      GCancellable *cancellable)
{
	GError *error = NULL;
	GsPluginLoaderHelper *helper = (GsPluginLoaderHelper *) task_data;

	/* run each plugin */
	if (!gs_plugin_loader_run_results (helper, cancellable, &error)) {
		g_task_return_error (task, error);
		return;
	}

	/* make sure 'All' has the right categories */
	for (guint i = 0; i < helper->catlist->len; i++) {
		GsCategory *cat = g_ptr_array_index (helper->catlist, i);
		gs_plugin_loader_fix_category_all (cat);
	}

	/* sort by name */
	g_ptr_array_sort (helper->catlist, gs_plugin_loader_category_sort_cb);
	for (guint i = 0; i < helper->catlist->len; i++) {
		GsCategory *cat = GS_CATEGORY (g_ptr_array_index (helper->catlist, i));
		gs_category_sort_children (cat);
	}

	/* success */
	if (helper->catlist->len == 0) {
		g_task_return_new_error (task,
					 GS_PLUGIN_ERROR,
					 GS_PLUGIN_ERROR_NOT_SUPPORTED,
					 "no categories to show");
		return;
	}

	/* success */
	g_task_return_pointer (task, g_ptr_array_ref (helper->catlist), (GDestroyNotify) g_ptr_array_unref);
}

/**
 * gs_plugin_loader_job_get_categories_async:
 *
 * This method calls all plugins that implement the gs_plugin_add_categories()
 * function. The plugins return #GsCategory objects.
 **/
void
gs_plugin_loader_job_get_categories_async (GsPluginLoader *plugin_loader,
				       GsPluginJob *plugin_job,
				       GCancellable *cancellable,
				       GAsyncReadyCallback callback,
				       gpointer user_data)
{
	GsPluginLoaderHelper *helper;
	g_autoptr(GTask) task = NULL;

	g_return_if_fail (GS_IS_PLUGIN_LOADER (plugin_loader));
	g_return_if_fail (GS_IS_PLUGIN_JOB (plugin_job));
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	/* save helper */
	helper = gs_plugin_loader_helper_new (plugin_loader, plugin_job);
	helper->catlist = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	gs_plugin_loader_job_debug (helper);

	/* run in a thread */
	task = g_task_new (plugin_loader, cancellable, callback, user_data);
	g_task_set_task_data (task, helper, (GDestroyNotify) gs_plugin_loader_helper_free);
	g_task_run_in_thread (task, gs_plugin_loader_job_get_categories_thread_cb);
}

/**
 * gs_plugin_loader_job_get_categories_finish:
 *
 * Return value: (element-type GsCategory) (transfer full): A list of applications
 **/
GPtrArray *
gs_plugin_loader_job_get_categories_finish (GsPluginLoader *plugin_loader,
					   GAsyncResult *res,
					   GError **error)
{
	g_return_val_if_fail (GS_IS_PLUGIN_LOADER (plugin_loader), NULL);
	g_return_val_if_fail (G_IS_TASK (res), NULL);
	g_return_val_if_fail (g_task_is_valid (res, plugin_loader), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	gs_utils_error_convert_gio (error);
	return g_task_propagate_pointer (G_TASK (res), error);
}

/******************************************************************************/

static gboolean
emit_pending_apps_idle (gpointer loader)
{
	g_signal_emit (loader, signals[SIGNAL_PENDING_APPS_CHANGED], 0);
	g_object_unref (loader);

	return G_SOURCE_REMOVE;
}

static void
gs_plugin_loader_pending_apps_add (GsPluginLoader *plugin_loader,
				   GsPluginLoaderHelper *helper)
{
	GsAppList *list = gs_plugin_job_get_list (helper->plugin_job);
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&priv->pending_apps_mutex);

	g_assert (gs_app_list_length (list) > 0);
	for (guint i = 0; i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);
		g_ptr_array_add (priv->pending_apps, g_object_ref (app));
		/* make sure the progress is properly initialized */
		gs_app_set_progress (app, 0);
	}
	g_idle_add (emit_pending_apps_idle, g_object_ref (plugin_loader));
}

static void
gs_plugin_loader_pending_apps_remove (GsPluginLoader *plugin_loader,
				      GsPluginLoaderHelper *helper)
{
	GsAppList *list = gs_plugin_job_get_list (helper->plugin_job);
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&priv->pending_apps_mutex);

	g_assert (gs_app_list_length (list) > 0);
	for (guint i = 0; i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);
		g_ptr_array_remove (priv->pending_apps, app);

		/* check the app is not still in an action helper */
		switch (gs_app_get_state (app)) {
		case AS_APP_STATE_INSTALLING:
		case AS_APP_STATE_REMOVING:
			g_warning ("application %s left in %s helper",
				   gs_app_get_unique_id (app),
				   as_app_state_to_string (gs_app_get_state (app)));
			gs_app_set_state (app, AS_APP_STATE_UNKNOWN);
			break;
		default:
			break;
		}

	}
	g_idle_add (emit_pending_apps_idle, g_object_ref (plugin_loader));
}

static gboolean
load_install_queue (GsPluginLoader *plugin_loader, GError **error)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	guint i;
	g_autofree gchar *contents = NULL;
	g_autofree gchar *file = NULL;
	g_auto(GStrv) names = NULL;
	g_autoptr(GsAppList) list = NULL;

	/* load from file */
	file = g_build_filename (g_get_user_data_dir (),
				 "gnome-software",
				 "install-queue",
				 NULL);
	if (!g_file_test (file, G_FILE_TEST_EXISTS))
		return TRUE;
	g_debug ("loading install queue from %s", file);
	if (!g_file_get_contents (file, &contents, NULL, error))
		return FALSE;

	/* add each app-id */
	list = gs_app_list_new ();
	names = g_strsplit (contents, "\n", 0);
	for (i = 0; names[i]; i++) {
		g_autoptr(GsApp) app = NULL;
		if (strlen (names[i]) == 0)
			continue;
		app = gs_app_new (names[i]);
		gs_app_set_state (app, AS_APP_STATE_QUEUED_FOR_INSTALL);

		g_mutex_lock (&priv->pending_apps_mutex);
		g_ptr_array_add (priv->pending_apps,
				 g_object_ref (app));
		g_mutex_unlock (&priv->pending_apps_mutex);

		g_debug ("adding pending app %s", gs_app_get_unique_id (app));
		gs_app_list_add (list, app);
	}

	/* refine */
	if (gs_app_list_length (list) > 0) {
		g_autoptr(GsPluginLoaderHelper) helper = NULL;
		g_autoptr(GsPluginJob) plugin_job = NULL;
		plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_REFINE, NULL);
		helper = gs_plugin_loader_helper_new (plugin_loader, plugin_job);
		gs_plugin_job_set_failure_flags (helper->plugin_job,
						 GS_PLUGIN_FAILURE_FLAGS_USE_EVENTS);
		if (!gs_plugin_loader_run_refine (helper, list, NULL, error))
			return FALSE;
	}
	return TRUE;
}

static void
save_install_queue (GsPluginLoader *plugin_loader)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	GPtrArray *pending_apps;
	gboolean ret;
	gint i;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) s = NULL;
	g_autofree gchar *file = NULL;

	s = g_string_new ("");
	pending_apps = priv->pending_apps;
	g_mutex_lock (&priv->pending_apps_mutex);
	for (i = (gint) pending_apps->len - 1; i >= 0; i--) {
		GsApp *app;
		app = g_ptr_array_index (pending_apps, i);
		if (gs_app_get_state (app) == AS_APP_STATE_QUEUED_FOR_INSTALL) {
			g_string_append (s, gs_app_get_id (app));
			g_string_append_c (s, '\n');
		}
	}
	g_mutex_unlock (&priv->pending_apps_mutex);

	/* save file */
	file = g_build_filename (g_get_user_data_dir (),
				 "gnome-software",
				 "install-queue",
				 NULL);
	if (!gs_mkdir_parent (file, &error)) {
		g_warning ("failed to create dir for %s: %s",
			   file, error->message);
		return;
	}
	g_debug ("saving install queue to %s", file);
	ret = g_file_set_contents (file, s->str, (gssize) s->len, &error);
	if (!ret)
		g_warning ("failed to save install queue: %s", error->message);
}

static void
add_app_to_install_queue (GsPluginLoader *plugin_loader, GsApp *app)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	GPtrArray *addons;
	guint i;
	guint id;

	/* queue the app itself */
	g_mutex_lock (&priv->pending_apps_mutex);
	g_ptr_array_add (priv->pending_apps, g_object_ref (app));
	g_mutex_unlock (&priv->pending_apps_mutex);

	gs_app_set_state (app, AS_APP_STATE_QUEUED_FOR_INSTALL);
	id = g_idle_add (emit_pending_apps_idle, g_object_ref (plugin_loader));
	g_source_set_name_by_id (id, "[gnome-software] emit_pending_apps_idle");
	save_install_queue (plugin_loader);

	/* recursively queue any addons */
	addons = gs_app_get_addons (app);
	for (i = 0; i < addons->len; i++) {
		GsApp *addon = g_ptr_array_index (addons, i);
		if (gs_app_get_to_be_installed (addon))
			add_app_to_install_queue (plugin_loader, addon);
	}
}

static gboolean
remove_app_from_install_queue (GsPluginLoader *plugin_loader, GsApp *app)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	GPtrArray *addons;
	gboolean ret;
	guint i;
	guint id;

	g_mutex_lock (&priv->pending_apps_mutex);
	ret = g_ptr_array_remove (priv->pending_apps, app);
	g_mutex_unlock (&priv->pending_apps_mutex);

	if (ret) {
		gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
		id = g_idle_add (emit_pending_apps_idle, g_object_ref (plugin_loader));
		g_source_set_name_by_id (id, "[gnome-software] emit_pending_apps_idle");
		save_install_queue (plugin_loader);

		/* recursively remove any queued addons */
		addons = gs_app_get_addons (app);
		for (i = 0; i < addons->len; i++) {
			GsApp *addon = g_ptr_array_index (addons, i);
			remove_app_from_install_queue (plugin_loader, addon);
		}
	}

	return ret;
}

/******************************************************************************/

gboolean
gs_plugin_loader_get_allow_updates (GsPluginLoader *plugin_loader)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	g_autoptr(GList) list = NULL;
	GList *l;

	/* nothing */
	if (g_hash_table_size (priv->disallow_updates) == 0)
		return TRUE;

	/* list */
	list = g_hash_table_get_values (priv->disallow_updates);
	for (l = list; l != NULL; l = l->next) {
		const gchar *reason = l->data;
		g_debug ("managed updates inhibited by %s", reason);
	}
	return FALSE;
}

GsAppList *
gs_plugin_loader_get_pending (GsPluginLoader *plugin_loader)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	GsAppList *array;
	guint i;

	array = gs_app_list_new ();
	g_mutex_lock (&priv->pending_apps_mutex);
	for (i = 0; i < priv->pending_apps->len; i++) {
		GsApp *app = g_ptr_array_index (priv->pending_apps, i);
		gs_app_list_add (array, app);
	}
	g_mutex_unlock (&priv->pending_apps_mutex);

	return array;
}

gboolean
gs_plugin_loader_get_enabled (GsPluginLoader *plugin_loader,
			      const gchar *plugin_name)
{
	GsPlugin *plugin;
	plugin = gs_plugin_loader_find_plugin (plugin_loader, plugin_name);
	if (plugin == NULL)
		return FALSE;
	return gs_plugin_get_enabled (plugin);
}

/**
 * gs_plugin_loader_get_events:
 * @plugin_loader: A #GsPluginLoader
 *
 * Gets all plugin events, even ones that are not active or visible anymore.
 *
 * Returns: (transfer container) (element-type GsPluginEvent): events
 **/
GPtrArray *
gs_plugin_loader_get_events (GsPluginLoader *plugin_loader)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	GPtrArray *events = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	GList *l;
	g_autoptr(GList) keys = NULL;
	g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&priv->events_by_id_mutex);

	/* just add everything */
	keys = g_hash_table_get_keys (priv->events_by_id);
	for (l = keys; l != NULL; l = l->next) {
		const gchar *key = l->data;
		GsPluginEvent *event = g_hash_table_lookup (priv->events_by_id, key);
		if (event == NULL) {
			g_warning ("failed to get event for '%s'", key);
			continue;
		}
		g_ptr_array_add (events, g_object_ref (event));
	}
	return events;
}

/**
 * gs_plugin_loader_get_event_default:
 * @plugin_loader: A #GsPluginLoader
 *
 * Gets an active plugin event where active means that it was not been
 * already dismissed by the user.
 *
 * Returns: a #GsPluginEvent, or %NULL for none
 **/
GsPluginEvent *
gs_plugin_loader_get_event_default (GsPluginLoader *plugin_loader)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	GList *l;
	g_autoptr(GList) keys = NULL;
	g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&priv->events_by_id_mutex);

	/* just add everything */
	keys = g_hash_table_get_keys (priv->events_by_id);
	for (l = keys; l != NULL; l = l->next) {
		const gchar *key = l->data;
		GsPluginEvent *event = g_hash_table_lookup (priv->events_by_id, key);
		if (event == NULL) {
			g_warning ("failed to get event for '%s'", key);
			continue;
		}
		if (!gs_plugin_event_has_flag (event, GS_PLUGIN_EVENT_FLAG_INVALID))
			return event;
	}
	return NULL;
}

/**
 * gs_plugin_loader_remove_events:
 * @plugin_loader: A #GsPluginLoader
 *
 * Removes all plugin events from the loader. This function should only be
 * called from the self tests.
 **/
void
gs_plugin_loader_remove_events (GsPluginLoader *plugin_loader)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&priv->events_by_id_mutex);
	g_hash_table_remove_all (priv->events_by_id);
}

static void
gs_plugin_loader_report_event_cb (GsPlugin *plugin,
				  GsPluginEvent *event,
				  GsPluginLoader *plugin_loader)
{
	gs_plugin_loader_add_event (plugin_loader, event);
}

static void
gs_plugin_loader_allow_updates_cb (GsPlugin *plugin,
				   gboolean allow_updates,
				   GsPluginLoader *plugin_loader)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	gpointer exists;

	/* plugin now allowing gnome-software to show updates panel */
	exists = g_hash_table_lookup (priv->disallow_updates, plugin);
	if (allow_updates) {
		if (exists == NULL)
			return;
		g_debug ("plugin %s no longer inhibited managed updates",
			 gs_plugin_get_name (plugin));
		g_hash_table_remove (priv->disallow_updates, plugin);

	/* plugin preventing the updates panel from being shown */
	} else {
		if (exists != NULL)
			return;
		g_debug ("plugin %s inhibited managed updates",
			 gs_plugin_get_name (plugin));
		g_hash_table_insert (priv->disallow_updates,
				     (gpointer) plugin,
				     (gpointer) gs_plugin_get_name (plugin));
	}

	/* something possibly changed, so notify display layer */
	g_object_notify (G_OBJECT (plugin_loader), "allow-updates");
}

static void
gs_plugin_loader_status_changed_cb (GsPlugin *plugin,
				    GsApp *app,
				    GsPluginStatus status,
				    GsPluginLoader *plugin_loader)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);

	/* nothing specific */
	if (app == NULL || gs_app_get_id (app) == NULL) {
		if (priv->global_status_last != status) {
			g_debug ("emitting global %s",
				 gs_plugin_status_to_string (status));
			g_signal_emit (plugin_loader,
				       signals[SIGNAL_STATUS_CHANGED],
				       0, app, status);
			priv->global_status_last = status;
		}
		return;
	}

	/* a specific app */
	g_debug ("emitting %s(%s)",
		 gs_plugin_status_to_string (status),
		 gs_app_get_id (app));
	g_signal_emit (plugin_loader,
		       signals[SIGNAL_STATUS_CHANGED],
		       0, app, status);
}

static gboolean
gs_plugin_loader_job_actions_changed_delay_cb (gpointer user_data)
{
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (user_data);
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);

	/* notify shells */
	g_debug ("updates-changed");
	g_signal_emit (plugin_loader, signals[SIGNAL_UPDATES_CHANGED], 0);
	priv->updates_changed_id = 0;

	g_object_unref (plugin_loader);
	return FALSE;
}

static void
gs_plugin_loader_job_actions_changed_cb (GsPlugin *plugin,
				     GsPluginLoader *plugin_loader)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	if (priv->updates_changed_id != 0)
		return;
	priv->updates_changed_id =
		g_timeout_add_seconds (GS_PLUGIN_LOADER_UPDATES_CHANGED_DELAY,
				       gs_plugin_loader_job_actions_changed_delay_cb,
				       g_object_ref (plugin_loader));
}

static gboolean
gs_plugin_loader_reload_delay_cb (gpointer user_data)
{
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (user_data);
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);

	/* notify shells */
	g_debug ("emitting ::reload");
	g_signal_emit (plugin_loader, signals[SIGNAL_RELOAD], 0);
	priv->reload_id = 0;

	g_object_unref (plugin_loader);
	return FALSE;
}

static void
gs_plugin_loader_reload_cb (GsPlugin *plugin,
			    GsPluginLoader *plugin_loader)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	if (priv->reload_id != 0)
		return;
	priv->reload_id =
		g_timeout_add_seconds (GS_PLUGIN_LOADER_RELOAD_DELAY,
				       gs_plugin_loader_reload_delay_cb,
				       g_object_ref (plugin_loader));
}

static void
gs_plugin_loader_open_plugin (GsPluginLoader *plugin_loader,
			      const gchar *filename)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	GsPlugin *plugin;
	g_autoptr(GError) error = NULL;

	/* create plugin from file */
	plugin = gs_plugin_create (filename, &error);
	if (plugin == NULL) {
		g_warning ("Failed to load %s: %s", filename, error->message);
		return;
	}
	g_signal_connect (plugin, "updates-changed",
			  G_CALLBACK (gs_plugin_loader_job_actions_changed_cb),
			  plugin_loader);
	g_signal_connect (plugin, "reload",
			  G_CALLBACK (gs_plugin_loader_reload_cb),
			  plugin_loader);
	g_signal_connect (plugin, "status-changed",
			  G_CALLBACK (gs_plugin_loader_status_changed_cb),
			  plugin_loader);
	g_signal_connect (plugin, "report-event",
			  G_CALLBACK (gs_plugin_loader_report_event_cb),
			  plugin_loader);
	g_signal_connect (plugin, "allow-updates",
			  G_CALLBACK (gs_plugin_loader_allow_updates_cb),
			  plugin_loader);
	gs_plugin_set_soup_session (plugin, priv->soup_session);
	gs_plugin_set_auth_array (plugin, priv->auth_array);
	gs_plugin_set_profile (plugin, priv->profile);
	gs_plugin_set_locale (plugin, priv->locale);
	gs_plugin_set_language (plugin, priv->language);
	gs_plugin_set_scale (plugin, gs_plugin_loader_get_scale (plugin_loader));
	gs_plugin_set_global_cache (plugin, priv->global_cache);
	g_debug ("opened plugin %s: %s", filename, gs_plugin_get_name (plugin));

	/* add to array */
	g_ptr_array_add (priv->plugins, plugin);
}

void
gs_plugin_loader_set_scale (GsPluginLoader *plugin_loader, guint scale)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);

	/* save globally, and update each plugin */
	priv->scale = scale;
	for (guint i = 0; i < priv->plugins->len; i++) {
		GsPlugin *plugin = g_ptr_array_index (priv->plugins, i);
		gs_plugin_set_scale (plugin, scale);
	}
}

guint
gs_plugin_loader_get_scale (GsPluginLoader *plugin_loader)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	return priv->scale;
}

GsAuth *
gs_plugin_loader_get_auth_by_id (GsPluginLoader *plugin_loader,
				 const gchar *provider_id)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	guint i;

	/* match on ID */
	for (i = 0; i < priv->auth_array->len; i++) {
		GsAuth *auth = g_ptr_array_index (priv->auth_array, i);
		if (g_strcmp0 (gs_auth_get_provider_id (auth), provider_id) == 0)
			return auth;
	}
	return NULL;
}

void
gs_plugin_loader_add_location (GsPluginLoader *plugin_loader, const gchar *location)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	for (guint i = 0; i < priv->locations->len; i++) {
		const gchar *location_tmp = g_ptr_array_index (priv->locations, i);
		if (g_strcmp0 (location_tmp, location) == 0)
			return;
	}
	g_info ("adding plugin location %s", location);
	g_ptr_array_add (priv->locations, g_strdup (location));
}

static gint
gs_plugin_loader_plugin_sort_fn (gconstpointer a, gconstpointer b)
{
	GsPlugin **pa = (GsPlugin **) a;
	GsPlugin **pb = (GsPlugin **) b;
	if (gs_plugin_get_order (*pa) < gs_plugin_get_order (*pb))
		return -1;
	if (gs_plugin_get_order (*pa) > gs_plugin_get_order (*pb))
		return 1;
	return 0;
}

static void
gs_plugin_loader_plugin_dir_changed_cb (GFileMonitor *monitor,
					GFile *file,
					GFile *other_file,
					GFileMonitorEvent event_type,
					GsPluginLoader *plugin_loader)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	GsApp *app;
	g_autoptr(GsPluginEvent) event = gs_plugin_event_new ();
	g_autoptr(GError) error = NULL;

	/* add app */
	gs_plugin_event_set_action (event, GS_PLUGIN_ACTION_SETUP);
	app = gs_app_list_lookup (priv->global_cache,
		"system/*/*/*/org.gnome.Software.desktop/*");
	if (app != NULL)
		gs_plugin_event_set_app (event, app);

	/* add error */
	g_set_error_literal (&error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_RESTART_REQUIRED,
			     "A restart is required");
	gs_plugin_event_set_error (event, error);
	gs_plugin_loader_add_event (plugin_loader, event);
}

void
gs_plugin_loader_clear_caches (GsPluginLoader *plugin_loader)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	for (guint i = 0; i < priv->plugins->len; i++) {
		GsPlugin *plugin = g_ptr_array_index (priv->plugins, i);
		gs_plugin_cache_invalidate (plugin);
	}
	gs_app_list_remove_all (priv->global_cache);
}

/**
 * gs_plugin_loader_setup_again:
 * @plugin_loader: a #GsPluginLoader
 *
 * Calls setup on each plugin. This should only be used from the self tests
 * and in a controlled way.
 */
void
gs_plugin_loader_setup_again (GsPluginLoader *plugin_loader)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	GsPluginAction actions[] = {
		GS_PLUGIN_ACTION_DESTROY,
		GS_PLUGIN_ACTION_INITIALIZE,
		GS_PLUGIN_ACTION_SETUP,
		GS_PLUGIN_ACTION_UNKNOWN };

	/* clear global cache */
	gs_plugin_loader_clear_caches (plugin_loader);

	/* remove any events */
	gs_plugin_loader_remove_events (plugin_loader);

	/* call in order */
	for (guint j = 0; actions[j] != GS_PLUGIN_ACTION_UNKNOWN; j++) {
		for (guint i = 0; i < priv->plugins->len; i++) {
			g_autoptr(GError) error_local = NULL;
			g_autoptr(GsPluginLoaderHelper) helper = NULL;
			g_autoptr(GsPluginJob) plugin_job = NULL;
			GsPlugin *plugin = g_ptr_array_index (priv->plugins, i);
			if (!gs_plugin_get_enabled (plugin))
				continue;

			plugin_job = gs_plugin_job_newv (actions[j],
							 "failure-flags", GS_PLUGIN_FAILURE_FLAGS_NO_CONSOLE,
							 NULL);
			helper = gs_plugin_loader_helper_new (plugin_loader, plugin_job);
			if (!gs_plugin_loader_call_vfunc (helper, plugin, NULL, NULL,
							  NULL, &error_local)) {
				g_warning ("resetup of %s failed: %s",
					   gs_plugin_get_name (plugin),
					   error_local->message);
				break;
			}
			if (actions[j] == GS_PLUGIN_ACTION_DESTROY)
				gs_plugin_clear_data (plugin);
		}
	}
}

/**
 * gs_plugin_loader_setup:
 * @plugin_loader: a #GsPluginLoader
 * @whitelist: list of plugin names, or %NULL
 * @blacklist: list of plugin names, or %NULL
 * @cancellable: A #GCancellable, or %NULL
 * @error: A #GError, or %NULL
 *
 * Sets up the plugin loader ready for use.
 *
 * Returns: %TRUE for success
 */
gboolean
gs_plugin_loader_setup (GsPluginLoader *plugin_loader,
			gchar **whitelist,
			gchar **blacklist,
			GsPluginFailureFlags failure_flags,
			GCancellable *cancellable,
			GError **error)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	const gchar *filename_tmp;
	const gchar *plugin_name;
	gboolean changes;
	GPtrArray *deps;
	GsPlugin *dep;
	GsPlugin *plugin;
	guint dep_loop_check = 0;
	guint i;
	guint j;
	g_autoptr(AsProfileTask) ptask = NULL;
	g_autoptr(GsPluginLoaderHelper) helper = NULL;
	g_autoptr(GsPluginJob) plugin_job = NULL;

	/* use the default, but this requires a 'make install' */
	if (priv->locations->len == 0) {
		g_autofree gchar *filename = NULL;
		filename = g_strdup_printf ("gs-plugins-%s", GS_PLUGIN_API_VERSION);
		g_ptr_array_add (priv->locations, g_build_filename (LIBDIR, filename, NULL));
	}

	for (i = 0; i < priv->locations->len; i++) {
		GFileMonitor *monitor;
		const gchar *location = g_ptr_array_index (priv->locations, i);
		g_autoptr(GFile) plugin_dir = g_file_new_for_path (location);
		monitor = g_file_monitor_directory (plugin_dir,
						    G_FILE_MONITOR_NONE,
						    cancellable,
						    error);
		if (monitor == NULL)
			return FALSE;
		g_signal_connect (monitor, "changed",
				  G_CALLBACK (gs_plugin_loader_plugin_dir_changed_cb), plugin_loader);
		g_ptr_array_add (priv->file_monitors, monitor);
	}

	/* search for plugins */
	ptask = as_profile_start_literal (priv->profile, "GsPlugin::setup");
	g_assert (ptask != NULL);
	for (i = 0; i < priv->locations->len; i++) {
		const gchar *location = g_ptr_array_index (priv->locations, i);
		g_autoptr(GDir) dir = NULL;

		/* search in the plugin directory for plugins */
		dir = g_dir_open (location, 0, error);
		if (dir == NULL)
			return FALSE;

		/* try to open each plugin */
		g_debug ("searching for plugins in %s", location);
		do {
			g_autofree gchar *filename_plugin = NULL;
			filename_tmp = g_dir_read_name (dir);
			if (filename_tmp == NULL)
				break;
			if (!g_str_has_suffix (filename_tmp, ".so"))
				continue;
			filename_plugin = g_build_filename (location,
							    filename_tmp,
							    NULL);
			gs_plugin_loader_open_plugin (plugin_loader, filename_plugin);
		} while (TRUE);
	}

	/* optional whitelist */
	if (whitelist != NULL) {
		for (i = 0; i < priv->plugins->len; i++) {
			gboolean ret;
			plugin = g_ptr_array_index (priv->plugins, i);
			if (!gs_plugin_get_enabled (plugin))
				continue;
			ret = g_strv_contains ((const gchar * const *) whitelist,
					       gs_plugin_get_name (plugin));
			if (!ret) {
				g_debug ("%s not in whitelist, disabling",
					 gs_plugin_get_name (plugin));
			}
			gs_plugin_set_enabled (plugin, ret);
		}
	}

	/* optional blacklist */
	if (blacklist != NULL) {
		for (i = 0; i < priv->plugins->len; i++) {
			gboolean ret;
			plugin = g_ptr_array_index (priv->plugins, i);
			if (!gs_plugin_get_enabled (plugin))
				continue;
			ret = g_strv_contains ((const gchar * const *) blacklist,
					       gs_plugin_get_name (plugin));
			if (ret)
				gs_plugin_set_enabled (plugin, FALSE);
		}
	}

	/* run the plugins */
	plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_INITIALIZE, NULL);
	helper = gs_plugin_loader_helper_new (plugin_loader, plugin_job);
	gs_plugin_job_set_failure_flags (helper->plugin_job,
					 failure_flags |
					 GS_PLUGIN_FAILURE_FLAGS_NO_CONSOLE);
	if (!gs_plugin_loader_run_results (helper, cancellable, error))
		return FALSE;

	/* order by deps */
	do {
		changes = FALSE;
		for (i = 0; i < priv->plugins->len; i++) {
			plugin = g_ptr_array_index (priv->plugins, i);
			deps = gs_plugin_get_rules (plugin, GS_PLUGIN_RULE_RUN_AFTER);
			for (j = 0; j < deps->len && !changes; j++) {
				plugin_name = g_ptr_array_index (deps, j);
				dep = gs_plugin_loader_find_plugin (plugin_loader,
								    plugin_name);
				if (dep == NULL) {
					g_debug ("cannot find plugin '%s' "
						 "requested by '%s'",
						 plugin_name,
						 gs_plugin_get_name (plugin));
					continue;
				}
				if (!gs_plugin_get_enabled (dep))
					continue;
				if (gs_plugin_get_order (plugin) <= gs_plugin_get_order (dep)) {
					g_debug ("%s [%u] to be ordered after %s [%u] "
						 "so promoting to [%u]",
						 gs_plugin_get_name (plugin),
						 gs_plugin_get_order (plugin),
						 gs_plugin_get_name (dep),
						 gs_plugin_get_order (dep),
						 gs_plugin_get_order (dep) + 1);
					gs_plugin_set_order (plugin, gs_plugin_get_order (dep) + 1);
					changes = TRUE;
				}
			}
		}
		for (i = 0; i < priv->plugins->len; i++) {
			plugin = g_ptr_array_index (priv->plugins, i);
			deps = gs_plugin_get_rules (plugin, GS_PLUGIN_RULE_RUN_BEFORE);
			for (j = 0; j < deps->len && !changes; j++) {
				plugin_name = g_ptr_array_index (deps, j);
				dep = gs_plugin_loader_find_plugin (plugin_loader,
								    plugin_name);
				if (dep == NULL) {
					g_debug ("cannot find plugin '%s' "
						 "requested by '%s'",
						 plugin_name,
						 gs_plugin_get_name (plugin));
					continue;
				}
				if (!gs_plugin_get_enabled (dep))
					continue;
				if (gs_plugin_get_order (plugin) >= gs_plugin_get_order (dep)) {
					g_debug ("%s [%u] to be ordered before %s [%u] "
						 "so promoting to [%u]",
						 gs_plugin_get_name (plugin),
						 gs_plugin_get_order (plugin),
						 gs_plugin_get_name (dep),
						 gs_plugin_get_order (dep),
						 gs_plugin_get_order (dep) + 1);
					gs_plugin_set_order (dep, gs_plugin_get_order (plugin) + 1);
					changes = TRUE;
				}
			}
		}

		/* check we're not stuck */
		if (dep_loop_check++ > 100) {
			g_set_error (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_PLUGIN_DEPSOLVE_FAILED,
				     "got stuck in dep loop");
			return FALSE;
		}
	} while (changes);

	/* check for conflicts */
	for (i = 0; i < priv->plugins->len; i++) {
		plugin = g_ptr_array_index (priv->plugins, i);
		if (!gs_plugin_get_enabled (plugin))
			continue;
		deps = gs_plugin_get_rules (plugin, GS_PLUGIN_RULE_CONFLICTS);
		for (j = 0; j < deps->len && !changes; j++) {
			plugin_name = g_ptr_array_index (deps, j);
			dep = gs_plugin_loader_find_plugin (plugin_loader,
							    plugin_name);
			if (dep == NULL)
				continue;
			if (!gs_plugin_get_enabled (dep))
				continue;
			g_debug ("disabling %s as conflicts with %s",
				 gs_plugin_get_name (dep),
				 gs_plugin_get_name (plugin));
			gs_plugin_set_enabled (dep, FALSE);
		}
	}

	/* sort by order */
	g_ptr_array_sort (priv->plugins,
			  gs_plugin_loader_plugin_sort_fn);

	/* assign priority values */
	do {
		changes = FALSE;
		for (i = 0; i < priv->plugins->len; i++) {
			plugin = g_ptr_array_index (priv->plugins, i);
			deps = gs_plugin_get_rules (plugin, GS_PLUGIN_RULE_BETTER_THAN);
			for (j = 0; j < deps->len && !changes; j++) {
				plugin_name = g_ptr_array_index (deps, j);
				dep = gs_plugin_loader_find_plugin (plugin_loader,
								    plugin_name);
				if (dep == NULL) {
					g_debug ("cannot find plugin '%s' "
						 "requested by '%s'",
						 plugin_name,
						 gs_plugin_get_name (plugin));
					continue;
				}
				if (!gs_plugin_get_enabled (dep))
					continue;
				if (gs_plugin_get_priority (plugin) <= gs_plugin_get_priority (dep)) {
					g_debug ("%s [%u] is better than %s [%u] "
						 "so promoting to [%u]",
						 gs_plugin_get_name (plugin),
						 gs_plugin_get_priority (plugin),
						 gs_plugin_get_name (dep),
						 gs_plugin_get_priority (dep),
						 gs_plugin_get_priority (dep) + 1);
					gs_plugin_set_priority (plugin, gs_plugin_get_priority (dep) + 1);
					changes = TRUE;
				}
			}
		}

		/* check we're not stuck */
		if (dep_loop_check++ > 100) {
			g_set_error (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_PLUGIN_DEPSOLVE_FAILED,
				     "got stuck in priority loop");
			return FALSE;
		}
	} while (changes);

	/* run setup */
	gs_plugin_job_set_action (helper->plugin_job, GS_PLUGIN_ACTION_SETUP);
	helper->function_name = "gs_plugin_setup";
	for (i = 0; i < priv->plugins->len; i++) {
		g_autoptr(GError) error_local = NULL;
		plugin = g_ptr_array_index (priv->plugins, i);
		if (!gs_plugin_loader_call_vfunc (helper, plugin, NULL, NULL,
						  cancellable, &error_local)) {
			g_debug ("disabling %s as setup failed: %s",
				 gs_plugin_get_name (plugin),
				 error_local->message);
			gs_plugin_set_enabled (plugin, FALSE);
		}
	}

	/* now we can load the install-queue */
	if (!load_install_queue (plugin_loader, error))
		return FALSE;
	return TRUE;
}

void
gs_plugin_loader_dump_state (GsPluginLoader *plugin_loader)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	g_autoptr(GString) str_enabled = g_string_new (NULL);
	g_autoptr(GString) str_disabled = g_string_new (NULL);

	/* print what the priorities are if verbose */
	for (guint i = 0; i < priv->plugins->len; i++) {
		GsPlugin *plugin = g_ptr_array_index (priv->plugins, i);
		GString *str = gs_plugin_get_enabled (plugin) ? str_enabled : str_disabled;
		g_string_append_printf (str, "%s, ", gs_plugin_get_name (plugin));
		g_debug ("[%s]\t%u\t->\t%s",
			 gs_plugin_get_enabled (plugin) ? "enabled" : "disabld",
			 gs_plugin_get_order (plugin),
			 gs_plugin_get_name (plugin));
	}
	if (str_enabled->len > 2)
		g_string_truncate (str_enabled, str_enabled->len - 2);
	if (str_disabled->len > 2)
		g_string_truncate (str_disabled, str_disabled->len - 2);
	g_info ("enabled plugins: %s", str_enabled->str);
	g_info ("disabled plugins: %s", str_disabled->str);
}

static void
gs_plugin_loader_get_property (GObject *object, guint prop_id,
			       GValue *value, GParamSpec *pspec)
{
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (object);
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);

	switch (prop_id) {
	case PROP_EVENTS:
		g_value_set_pointer (value, priv->events_by_id);
		break;
	case PROP_ALLOW_UPDATES:
		g_value_set_boolean (value, gs_plugin_loader_get_allow_updates (plugin_loader));
		break;
	case PROP_NETWORK_AVAILABLE:
		g_value_set_boolean (value, gs_plugin_loader_get_network_available (plugin_loader));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gs_plugin_loader_set_property (GObject *object, guint prop_id,
			       const GValue *value, GParamSpec *pspec)
{
	switch (prop_id) {
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gs_plugin_loader_dispose (GObject *object)
{
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (object);
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);

	if (priv->plugins != NULL) {
		g_autoptr(GsPluginLoaderHelper) helper = NULL;
		g_autoptr(GsPluginJob) plugin_job = NULL;
		plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_DESTROY, NULL);
		helper = gs_plugin_loader_helper_new (plugin_loader, plugin_job);
		gs_plugin_loader_run_results (helper, NULL, NULL);
		g_clear_pointer (&priv->plugins, g_ptr_array_unref);
	}
	if (priv->updates_changed_id != 0) {
		g_source_remove (priv->updates_changed_id);
		priv->updates_changed_id = 0;
	}
	if (priv->network_changed_handler != 0) {
		g_signal_handler_disconnect (priv->network_monitor,
					     priv->network_changed_handler);
		priv->network_changed_handler = 0;
	}
	g_clear_object (&priv->network_monitor);
	g_clear_object (&priv->soup_session);
	g_clear_object (&priv->profile);
	g_clear_object (&priv->settings);
	g_clear_pointer (&priv->auth_array, g_ptr_array_unref);
	g_clear_pointer (&priv->pending_apps, g_ptr_array_unref);

	G_OBJECT_CLASS (gs_plugin_loader_parent_class)->dispose (object);
}

static void
gs_plugin_loader_finalize (GObject *object)
{
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (object);
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);

	g_strfreev (priv->compatible_projects);
	g_ptr_array_unref (priv->locations);
	g_free (priv->locale);
	g_free (priv->language);
	g_object_unref (priv->global_cache);
	g_ptr_array_unref (priv->file_monitors);
	g_hash_table_unref (priv->events_by_id);
	g_hash_table_unref (priv->disallow_updates);

	g_mutex_clear (&priv->pending_apps_mutex);
	g_mutex_clear (&priv->events_by_id_mutex);

	G_OBJECT_CLASS (gs_plugin_loader_parent_class)->finalize (object);
}

static void
gs_plugin_loader_class_init (GsPluginLoaderClass *klass)
{
	GParamSpec *pspec;
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->get_property = gs_plugin_loader_get_property;
	object_class->set_property = gs_plugin_loader_set_property;
	object_class->dispose = gs_plugin_loader_dispose;
	object_class->finalize = gs_plugin_loader_finalize;

	pspec = g_param_spec_string ("events", NULL, NULL,
				     NULL,
				     G_PARAM_READABLE);
	g_object_class_install_property (object_class, PROP_EVENTS, pspec);

	pspec = g_param_spec_boolean ("allow-updates", NULL, NULL,
				      TRUE,
				      G_PARAM_READABLE);
	g_object_class_install_property (object_class, PROP_ALLOW_UPDATES, pspec);

	pspec = g_param_spec_boolean ("network-available", NULL, NULL,
				      FALSE,
				      G_PARAM_READABLE);
	g_object_class_install_property (object_class, PROP_NETWORK_AVAILABLE, pspec);

	signals [SIGNAL_STATUS_CHANGED] =
		g_signal_new ("status-changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (GsPluginLoaderClass, status_changed),
			      NULL, NULL, g_cclosure_marshal_generic,
			      G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_UINT);
	signals [SIGNAL_PENDING_APPS_CHANGED] =
		g_signal_new ("pending-apps-changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (GsPluginLoaderClass, pending_apps_changed),
			      NULL, NULL, g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
	signals [SIGNAL_UPDATES_CHANGED] =
		g_signal_new ("updates-changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (GsPluginLoaderClass, updates_changed),
			      NULL, NULL, g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
	signals [SIGNAL_RELOAD] =
		g_signal_new ("reload",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (GsPluginLoaderClass, reload),
			      NULL, NULL, g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
}

static void
gs_plugin_loader_allow_updates_recheck (GsPluginLoader *plugin_loader)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	if (g_settings_get_boolean (priv->settings, "allow-updates")) {
		g_hash_table_remove (priv->disallow_updates, plugin_loader);
	} else {
		g_hash_table_insert (priv->disallow_updates,
				     (gpointer) plugin_loader,
				     (gpointer) "GSettings");
	}
}

static void
gs_plugin_loader_settings_changed_cb (GSettings *settings,
				      const gchar *key,
				      GsPluginLoader *plugin_loader)
{
	if (g_strcmp0 (key, "allow-updates") == 0)
		gs_plugin_loader_allow_updates_recheck (plugin_loader);
}

static void
gs_plugin_loader_init (GsPluginLoader *plugin_loader)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	const gchar *tmp;
	gchar *match;
	gchar **projects;
	guint i;

	priv->scale = 1;
	priv->global_cache = gs_app_list_new ();
	priv->plugins = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	priv->pending_apps = g_ptr_array_new_with_free_func ((GFreeFunc) g_object_unref);
	priv->auth_array = g_ptr_array_new_with_free_func ((GFreeFunc) g_object_unref);
	priv->file_monitors = g_ptr_array_new_with_free_func ((GFreeFunc) g_object_unref);
	priv->locations = g_ptr_array_new_with_free_func (g_free);
	priv->profile = as_profile_new ();
	priv->settings = g_settings_new ("org.gnome.software");
	g_signal_connect (priv->settings, "changed",
			  G_CALLBACK (gs_plugin_loader_settings_changed_cb), plugin_loader);
	priv->events_by_id = g_hash_table_new_full ((GHashFunc) as_utils_unique_id_hash,
					            (GEqualFunc) as_utils_unique_id_equal,
						    g_free,
						    (GDestroyNotify) g_object_unref);

	/* share a soup session (also disable the double-compression) */
	priv->soup_session = soup_session_new_with_options (SOUP_SESSION_USER_AGENT, gs_user_agent (),
							    SOUP_SESSION_TIMEOUT, 10,
							    NULL);
	soup_session_remove_feature_by_type (priv->soup_session,
					     SOUP_TYPE_CONTENT_DECODER);

	/* get the locale without the various UTF-8 suffixes */
	tmp = g_getenv ("GS_SELF_TEST_LOCALE");
	if (tmp != NULL) {
		g_debug ("using self test locale of %s", tmp);
		priv->locale = g_strdup (tmp);
	} else {
		priv->locale = g_strdup (setlocale (LC_MESSAGES, NULL));
		match = g_strstr_len (priv->locale, -1, ".UTF-8");
		if (match != NULL)
			*match = '\0';
		match = g_strstr_len (priv->locale, -1, ".utf8");
		if (match != NULL)
			*match = '\0';
	}

	/* the settings key sets the initial override */
	priv->disallow_updates = g_hash_table_new (g_direct_hash, g_direct_equal);
	gs_plugin_loader_allow_updates_recheck (plugin_loader);

	/* get the language from the locale */
	priv->language = g_strdup (priv->locale);
	match = g_strrstr (priv->language, "_");
	if (match != NULL)
		*match = '\0';

	g_mutex_init (&priv->pending_apps_mutex);
	g_mutex_init (&priv->events_by_id_mutex);

	/* monitor the network as the many UI operations need the network */
	gs_plugin_loader_monitor_network (plugin_loader);

	/* by default we only show project-less apps or compatible projects */
	tmp = g_getenv ("GNOME_SOFTWARE_COMPATIBLE_PROJECTS");
	if (tmp == NULL) {
		projects = g_settings_get_strv (priv->settings,
						"compatible-projects");
	} else {
		projects = g_strsplit (tmp, ",", -1);
	}
	for (i = 0; projects[i] != NULL; i++)
		g_debug ("compatible-project: %s", projects[i]);
	priv->compatible_projects = projects;
}

/**
 * gs_plugin_loader_new:
 *
 * Return value: a new GsPluginLoader object.
 **/
GsPluginLoader *
gs_plugin_loader_new (void)
{
	GsPluginLoader *plugin_loader;
	plugin_loader = g_object_new (GS_TYPE_PLUGIN_LOADER, NULL);
	return GS_PLUGIN_LOADER (plugin_loader);
}

static void
gs_plugin_loader_app_installed_cb (GObject *source,
				   GAsyncResult *res,
				   gpointer user_data)
{
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source);
	gboolean ret;
	g_autoptr(GError) error = NULL;
	g_autoptr(GsApp) app = GS_APP (user_data);

	ret = gs_plugin_loader_job_action_finish (plugin_loader,
						   res,
						   &error);
	if (!ret) {
		remove_app_from_install_queue (plugin_loader, app);
		g_warning ("failed to install %s: %s",
			   gs_app_get_unique_id (app), error->message);
	}
}

gboolean
gs_plugin_loader_get_network_available (GsPluginLoader *plugin_loader)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	if (priv->network_monitor == NULL) {
		g_debug ("no network monitor, so returning network-available=TRUE");
		return TRUE;
	}
	return g_network_monitor_get_network_available (priv->network_monitor);
}

gboolean
gs_plugin_loader_get_network_metered (GsPluginLoader *plugin_loader)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	if (priv->network_monitor == NULL) {
		g_debug ("no network monitor, so returning network-metered=FALSE");
		return FALSE;
	}
	return g_network_monitor_get_network_metered (priv->network_monitor);
}

static void
gs_plugin_loader_network_changed_cb (GNetworkMonitor *monitor,
				     gboolean available,
				     GsPluginLoader *plugin_loader)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);

	g_debug ("network status change: %s [%s]",
		 available ? "online" : "offline",
		 g_network_monitor_get_network_metered (priv->network_monitor) ? "metered" : "unmetered");

	g_object_notify (G_OBJECT (plugin_loader), "network-available");

	if (available) {
		g_autoptr(GsAppList) queue = NULL;
		g_mutex_lock (&priv->pending_apps_mutex);
		queue = gs_app_list_new ();
		for (guint i = 0; i < priv->pending_apps->len; i++) {
			GsApp *app = g_ptr_array_index (priv->pending_apps, i);
			if (gs_app_get_state (app) == AS_APP_STATE_QUEUED_FOR_INSTALL)
				gs_app_list_add (queue, app);
		}
		g_mutex_unlock (&priv->pending_apps_mutex);
		for (guint i = 0; i < gs_app_list_length (queue); i++) {
			GsApp *app = gs_app_list_index (queue, i);
			g_autoptr(GsPluginJob) plugin_job = NULL;
			plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_INSTALL,
							 "app", app,
							 "failure-flags", GS_PLUGIN_FAILURE_FLAGS_USE_EVENTS,
							 NULL);
			gs_plugin_loader_job_process_async (plugin_loader, plugin_job,
							    NULL,
							    gs_plugin_loader_app_installed_cb,
							    g_object_ref (app));
		}
	}
}

static void
gs_plugin_loader_monitor_network (GsPluginLoader *plugin_loader)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	GNetworkMonitor *network_monitor;

	network_monitor = g_network_monitor_get_default ();
	if (network_monitor == NULL || priv->network_changed_handler != 0)
		return;
	priv->network_monitor = g_object_ref (network_monitor);

	priv->network_changed_handler =
		g_signal_connect (priv->network_monitor, "network-changed",
				  G_CALLBACK (gs_plugin_loader_network_changed_cb), plugin_loader);

	gs_plugin_loader_network_changed_cb (priv->network_monitor,
			    g_network_monitor_get_network_available (priv->network_monitor),
			    plugin_loader);
}

/******************************************************************************/

static AsIcon *
_gs_app_get_icon_by_kind (GsApp *app, AsIconKind kind)
{
	GPtrArray *icons = gs_app_get_icons (app);
	guint i;
	for (i = 0; i < icons->len; i++) {
		AsIcon *ic = g_ptr_array_index (icons, i);
		if (as_icon_get_kind (ic) == kind)
			return ic;
	}
	return NULL;
}

static GPtrArray *
get_updatable_apps (GPtrArray *apps)
{
	GPtrArray *updatables = g_ptr_array_sized_new (apps->len);

	for (guint i = 0; i < apps->len; ++i) {
		GsApp *app = g_ptr_array_index (apps, i);
		if (gs_app_is_updatable (app))
			g_ptr_array_add (updatables, app);
	}

	return updatables;
}

static void
related_app_progress_notify_cb (GsApp *app,
				GParamSpec *pspec,
				GsProxyUpdateHelper *proxy_helper)
{
	GsApp *proxy = proxy_helper->proxy;
	guint progress = gs_app_get_progress (app);
	guint progress_fraction = progress / proxy_helper->total_apps;
	guint progress_step = proxy_helper->app_index * (100 / proxy_helper->total_apps);

	/* assign the updating app's progress to its corresponding fraction of
	 * the proxy app's progress */
	gs_app_set_progress (proxy, MIN (progress_step + progress_fraction, 100));
}

static GsProxyUpdateHelper *
gs_proxy_update_helper_new (GsApp *proxy,
			    GsApp *related_app,
			    guint total_apps,
			    guint app_index)
{
	GsProxyUpdateHelper *proxy_helper = g_slice_new0 (GsProxyUpdateHelper);
	proxy_helper->proxy = g_object_ref (proxy);
	proxy_helper->app = g_object_ref (related_app);
	proxy_helper->total_apps = total_apps;
	proxy_helper->app_index = app_index;
	proxy_helper->progress_handler_id =
		g_signal_connect (proxy_helper->app, "notify::progress",
				  G_CALLBACK (related_app_progress_notify_cb),
				  proxy_helper);
	return proxy_helper;
}

static void
gs_proxy_update_helper_free (GsProxyUpdateHelper *proxy_helper)
{
	g_signal_handler_disconnect (proxy_helper->app, proxy_helper->progress_handler_id);
	g_object_unref (proxy_helper->app);
	g_object_unref (proxy_helper->proxy);
	g_slice_free (GsProxyUpdateHelper, proxy_helper);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GsProxyUpdateHelper, gs_proxy_update_helper_free)

static gboolean
gs_plugin_loader_generic_update (GsPluginLoader *plugin_loader,
				 GsPluginLoaderHelper *helper,
				 GCancellable *cancellable,
				 GError **error)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	GsAppList *list;

	/* run each plugin, per-app version */
	list = gs_plugin_job_get_list (helper->plugin_job);
	for (guint i = 0; i < priv->plugins->len; i++) {
		GsPluginActionFunc plugin_app_func = NULL;

		GsPlugin *plugin = g_ptr_array_index (priv->plugins, i);
		if (g_cancellable_set_error_if_cancelled (cancellable, error)) {
			gs_utils_error_convert_gio (error);
			return FALSE;
		}
		plugin_app_func = gs_plugin_get_symbol (plugin, helper->function_name);
		if (plugin_app_func == NULL)
			continue;

		/* for each app */
		for (guint j = 0; j < gs_app_list_length (list); j++) {
			GsApp *app_tmp = gs_app_list_index (list, j);
			g_autoptr(GPtrArray) apps = NULL;
			gboolean is_proxy_update;

			/* operate on the parent app or the related apps */
			is_proxy_update = gs_app_has_quirk (app_tmp, AS_APP_QUIRK_IS_PROXY);
			if (is_proxy_update) {
				apps = get_updatable_apps (gs_app_get_related (app_tmp));
				if (apps->len > 0) {
					/* ensure that the proxy app is updatable */
					if (!gs_app_is_updatable (app_tmp))
						gs_app_set_state (app_tmp, AS_APP_STATE_UPDATABLE_LIVE);
					gs_app_set_state (app_tmp, AS_APP_STATE_INSTALLING);
				}
			} else {
				apps = g_ptr_array_new ();
				g_ptr_array_add (apps, app_tmp);
			}
			for (guint k = 0; k < apps->len; k++) {
				GsApp *app = g_ptr_array_index (apps, k);
				gboolean ret;
				g_autoptr(AsProfileTask) ptask = NULL;
				g_autoptr(GError) error_local = NULL;
				g_autoptr(GsProxyUpdateHelper) proxy_helper = NULL;

				gs_plugin_job_set_app (helper->plugin_job, app);

				if (is_proxy_update) {
					proxy_helper = gs_proxy_update_helper_new (app_tmp,
										   app,
										   apps->len,
										   k);
					g_assert (proxy_helper != NULL);
				}

				ptask = as_profile_start (priv->profile,
							  "GsPlugin::%s(%s){%s}",
							  gs_plugin_get_name (plugin),
							  helper->function_name,
							  gs_app_get_id (app));
				g_assert (ptask != NULL);
				gs_plugin_loader_action_start (plugin_loader, plugin, FALSE);
				ret = plugin_app_func (plugin, app,
						       cancellable,
						       &error_local);
				gs_plugin_loader_action_stop (plugin_loader, plugin);
				if (!ret) {
					if (!gs_plugin_error_handle_failure (helper,
									     plugin,
									     error_local,
									     error)) {
						return FALSE;
					}
				}
			}
			if (is_proxy_update)
				gs_app_set_state (app_tmp, AS_APP_STATE_INSTALLED);
		}
		helper->anything_ran = TRUE;
		gs_plugin_status_update (plugin, NULL, GS_PLUGIN_STATUS_FINISHED);
	}
	return TRUE;
}

static void
gs_plugin_loader_process_thread_cb (GTask *task,
				    gpointer object,
				    gpointer task_data,
				    GCancellable *cancellable)
{
	GError *error = NULL;
	GsPluginLoaderHelper *helper = (GsPluginLoaderHelper *) task_data;
	GsAppList *list = gs_plugin_job_get_list (helper->plugin_job);
	GsPluginAction action = gs_plugin_job_get_action (helper->plugin_job);
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (object);
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	gboolean add_to_pending_array = FALSE;

	/* these change the pending count on the installed panel */
	switch (action) {
	case GS_PLUGIN_ACTION_INSTALL:
	case GS_PLUGIN_ACTION_REMOVE:
		add_to_pending_array = TRUE;
		break;
	default:
		break;
	}

	/* add to pending list */
	if (add_to_pending_array)
		gs_plugin_loader_pending_apps_add (plugin_loader, helper);

	/* run each plugin */
	if (action != GS_PLUGIN_ACTION_REFINE) {
		if (!gs_plugin_loader_run_results (helper, cancellable, &error)) {
			if (add_to_pending_array) {
				gs_app_set_state_recover (gs_plugin_job_get_app (helper->plugin_job));
				gs_plugin_loader_pending_apps_remove (plugin_loader, helper);
			}
			g_task_return_error (task, error);
			return;
		}
	}

	/* run per-app version */
	if (action == GS_PLUGIN_ACTION_UPDATE) {
		helper->function_name = "gs_plugin_update_app";
		if (!gs_plugin_loader_generic_update (plugin_loader, helper,
						      cancellable, &error)) {
			g_task_return_error (task, error);
			return;
		}
	}

	/* remove from pending list */
	if (add_to_pending_array)
		gs_plugin_loader_pending_apps_remove (plugin_loader, helper);

	/* append extra things when we want the list of pending updates */
	if (action == GS_PLUGIN_ACTION_GET_UPDATES &&
	    !g_settings_get_boolean (priv->settings, "download-updates")) {
		helper->function_name = "gs_plugin_add_updates_pending";
		if (!gs_plugin_loader_run_results (helper, cancellable, &error)) {
			g_task_return_error (task, error);
			return;
		}
	}

	/* some functions are really required for proper operation */
	switch (action) {
	case GS_PLUGIN_ACTION_DESTROY:
	case GS_PLUGIN_ACTION_GET_INSTALLED:
	case GS_PLUGIN_ACTION_GET_UPDATES:
	case GS_PLUGIN_ACTION_INITIALIZE:
	case GS_PLUGIN_ACTION_INSTALL:
	case GS_PLUGIN_ACTION_LAUNCH:
	case GS_PLUGIN_ACTION_REFRESH:
	case GS_PLUGIN_ACTION_REMOVE:
	case GS_PLUGIN_ACTION_SEARCH:
	case GS_PLUGIN_ACTION_SETUP:
	case GS_PLUGIN_ACTION_UPDATE:
		if (!helper->anything_ran) {
			g_set_error (&error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_NOT_SUPPORTED,
				     "no plugin could handle %s",
				     gs_plugin_action_to_string (action));
			g_task_return_error (task, error);
			return;
		}
		break;
	default:
		if (!helper->anything_ran) {
			g_debug ("no plugin could handle %s",
				 gs_plugin_action_to_string (action));
		}
		break;
	}

	/* unstage addons */
	if (add_to_pending_array) {
		GPtrArray *addons;
		addons = gs_app_get_addons (gs_plugin_job_get_app (helper->plugin_job));
		for (guint i = 0; i < addons->len; i++) {
			GsApp *addon = g_ptr_array_index (addons, i);
			if (gs_app_get_to_be_installed (addon))
				gs_app_set_to_be_installed (addon, FALSE);
		}
	}

	/* modify the local app */
	switch (action) {
	case GS_PLUGIN_ACTION_REVIEW_SUBMIT:
		gs_app_add_review (gs_plugin_job_get_app (helper->plugin_job), gs_plugin_job_get_review (helper->plugin_job));
		break;
	case GS_PLUGIN_ACTION_REVIEW_REMOVE:
		gs_app_remove_review (gs_plugin_job_get_app (helper->plugin_job), gs_plugin_job_get_review (helper->plugin_job));
		break;
	default:
		break;
	}

	/* filter to reduce to a sane set */
	gs_plugin_loader_job_sorted_truncation (helper);

	/* set the local file on any of the returned results */
	switch (action) {
	case GS_PLUGIN_ACTION_FILE_TO_APP:
		for (guint j = 0; j < gs_app_list_length (list); j++) {
			GsApp *app = gs_app_list_index (list, j);
			if (gs_app_get_local_file (app) == NULL)
				gs_app_set_local_file (app, gs_plugin_job_get_file (helper->plugin_job));
		}
	default:
		break;
	}

	/* pick up new source id */
	if (add_to_pending_array) {
		gs_plugin_job_add_refine_flags (helper->plugin_job,
						GS_PLUGIN_REFINE_FLAGS_REQUIRE_ORIGIN);
	}

	/* run refine() on each one if required */
	if (gs_plugin_job_get_refine_flags (helper->plugin_job) != 0) {
		if (!gs_plugin_loader_run_refine (helper, list, cancellable, &error)) {
			g_task_return_error (task, error);
			return;
		}
	}

	/* convert any unavailable codecs */
	switch (action) {
	case GS_PLUGIN_ACTION_SEARCH:
	case GS_PLUGIN_ACTION_SEARCH_FILES:
	case GS_PLUGIN_ACTION_SEARCH_PROVIDES:
		gs_plugin_loader_convert_unavailable (list, gs_plugin_job_get_search (helper->plugin_job));
		break;
	default:
		break;
	}

	/* check the local files have an icon set */
	switch (action) {
	case GS_PLUGIN_ACTION_URL_TO_APP:
	case GS_PLUGIN_ACTION_FILE_TO_APP:
		for (guint j = 0; j < gs_app_list_length (list); j++) {
			GsApp *app = gs_app_list_index (list, j);
			if (_gs_app_get_icon_by_kind (app, AS_ICON_KIND_STOCK) == NULL &&
			    _gs_app_get_icon_by_kind (app, AS_ICON_KIND_LOCAL) == NULL &&
			    _gs_app_get_icon_by_kind (app, AS_ICON_KIND_CACHED) == NULL) {
				g_autoptr(AsIcon) ic = as_icon_new ();
				as_icon_set_kind (ic, AS_ICON_KIND_STOCK);
				if (gs_app_get_kind (app) == AS_APP_KIND_SOURCE)
					as_icon_set_name (ic, "x-package-repository");
				else
					as_icon_set_name (ic, "application-x-executable");
				gs_app_add_icon (app, ic);
			}
		}

		/* run refine() on each one again to pick up any icons */
		gs_plugin_job_set_refine_flags (helper->plugin_job,
						GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON);
		if (!gs_plugin_loader_run_refine (helper, list, cancellable, &error)) {
			g_task_return_error (task, error);
			return;
		}
		break;
	default:
		break;
	}

	/* filter package list */
	switch (action) {
	case GS_PLUGIN_ACTION_URL_TO_APP:
		gs_app_list_filter (list, gs_plugin_loader_app_is_valid, helper);
		break;
	case GS_PLUGIN_ACTION_SEARCH:
	case GS_PLUGIN_ACTION_SEARCH_FILES:
	case GS_PLUGIN_ACTION_SEARCH_PROVIDES:
		gs_app_list_filter (list, gs_plugin_loader_app_is_valid, helper);
		gs_app_list_filter (list, gs_plugin_loader_filter_qt_for_gtk, NULL);
		gs_app_list_filter (list, gs_plugin_loader_get_app_is_compatible, plugin_loader);
		break;
	case GS_PLUGIN_ACTION_GET_CATEGORY_APPS:
		gs_app_list_filter (list, gs_plugin_loader_app_is_non_compulsory, NULL);
		gs_app_list_filter (list, gs_plugin_loader_app_is_valid, helper);
		gs_app_list_filter (list, gs_plugin_loader_filter_qt_for_gtk, NULL);
		gs_app_list_filter (list, gs_plugin_loader_get_app_is_compatible, plugin_loader);
		break;
	case GS_PLUGIN_ACTION_GET_INSTALLED:
		gs_app_list_filter (list, gs_plugin_loader_app_is_valid, helper);
		gs_app_list_filter (list, gs_plugin_loader_app_is_valid_installed, helper);
		break;
	case GS_PLUGIN_ACTION_GET_FEATURED:
		if (g_getenv ("GNOME_SOFTWARE_FEATURED") != NULL) {
			gs_app_list_filter (list, gs_plugin_loader_featured_debug, NULL);
		} else {
			gs_app_list_filter (list, gs_plugin_loader_app_is_valid, helper);
			gs_app_list_filter (list, gs_plugin_loader_get_app_is_compatible, plugin_loader);
		}
		break;
	case GS_PLUGIN_ACTION_GET_UPDATES:
		gs_app_list_filter (list, gs_plugin_loader_app_is_valid_updatable, helper);
		break;
	case GS_PLUGIN_ACTION_GET_RECENT:
		gs_app_list_filter (list, gs_plugin_loader_app_is_non_compulsory, NULL);
		gs_app_list_filter (list, gs_plugin_loader_app_is_valid, helper);
		gs_app_list_filter (list, gs_plugin_loader_filter_qt_for_gtk, NULL);
		gs_app_list_filter (list, gs_plugin_loader_get_app_is_compatible, plugin_loader);
		break;
	case GS_PLUGIN_ACTION_GET_POPULAR:
		gs_app_list_filter (list, gs_plugin_loader_app_is_valid, helper);
		gs_app_list_filter (list, gs_plugin_loader_filter_qt_for_gtk, NULL);
		gs_app_list_filter (list, gs_plugin_loader_get_app_is_compatible, plugin_loader);
		break;
	default:
		break;
	}

	/* only allow one result */
	if (action == GS_PLUGIN_ACTION_URL_TO_APP ||
	    action == GS_PLUGIN_ACTION_FILE_TO_APP) {
		if (gs_app_list_length (list) == 0) {
			g_autofree gchar *str = gs_plugin_job_to_string (helper->plugin_job);
			g_task_return_new_error (task,
						 GS_PLUGIN_ERROR,
						 GS_PLUGIN_ERROR_NOT_SUPPORTED,
						 "no application was created for %s",
						 str);
			return;
		}
		if (gs_app_list_length (list) > 1) {
			g_autofree gchar *str = gs_plugin_job_to_string (helper->plugin_job);
			g_task_return_new_error (task,
						 GS_PLUGIN_ERROR,
						 GS_PLUGIN_ERROR_NOT_SUPPORTED,
						 "more than one application was created for %s",
						 str);
			return;
		}
	}

	/* too many */
	if (gs_app_list_length (list) > 500) {
		g_task_return_new_error (task,
					 GS_PLUGIN_ERROR,
					 GS_PLUGIN_ERROR_NOT_SUPPORTED,
					 "too many results returned");
		return;
	}

	/* filter duplicates with priority, taking into account the source name
	 * & version, so we combine available updates with the installed app */
	gs_app_list_filter (list, gs_plugin_loader_app_set_prio, plugin_loader);
	gs_app_list_filter_duplicates (list,
				       GS_APP_LIST_FILTER_FLAG_KEY_ID |
				       GS_APP_LIST_FILTER_FLAG_KEY_SOURCE |
				       GS_APP_LIST_FILTER_FLAG_KEY_VERSION);

	/* sort these again as the refine may have added useful metadata */
	gs_plugin_loader_job_sorted_truncation_again (helper);

	/* success */
	g_task_return_pointer (task, g_object_ref (list), (GDestroyNotify) g_object_unref);
}

/**
 * gs_plugin_loader_job_process_async:
 *
 * This method calls all plugins.
 **/
void
gs_plugin_loader_job_process_async (GsPluginLoader *plugin_loader,
				    GsPluginJob *plugin_job,
				    GCancellable *cancellable,
				    GAsyncReadyCallback callback,
				    gpointer user_data)
{
	GsPluginAction action;
	GsPluginLoaderHelper *helper;
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	g_autoptr(GTask) task = NULL;

	g_return_if_fail (GS_IS_PLUGIN_LOADER (plugin_loader));
	g_return_if_fail (GS_IS_PLUGIN_JOB (plugin_job));
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	/* deal with the install queue */
	action = gs_plugin_job_get_action (plugin_job);
	if (action == GS_PLUGIN_ACTION_REMOVE) {
		if (remove_app_from_install_queue (plugin_loader, gs_plugin_job_get_app (plugin_job))) {
			GsAppList *list = gs_plugin_job_get_list (plugin_job);
			task = g_task_new (plugin_loader, cancellable, callback, user_data);
			g_task_return_pointer (task, g_object_ref (list), (GDestroyNotify) g_object_unref);
			return;
		}
	}
	if (action == GS_PLUGIN_ACTION_INSTALL &&
	    !gs_plugin_loader_get_network_available (plugin_loader)) {
		GsAppList *list = gs_plugin_job_get_list (plugin_job);
		add_app_to_install_queue (plugin_loader, gs_plugin_job_get_app (plugin_job));
		task = g_task_new (plugin_loader, cancellable, callback, user_data);
		g_task_return_pointer (task, g_object_ref (list), (GDestroyNotify) g_object_unref);
		return;
	}

	/* hardcoded, so resolve a set list */
	if (action == GS_PLUGIN_ACTION_GET_POPULAR) {
		g_auto(GStrv) apps = NULL;
		if (g_getenv ("GNOME_SOFTWARE_POPULAR") != NULL) {
			apps = g_strsplit (g_getenv ("GNOME_SOFTWARE_POPULAR"), ",", 0);
		} else {
			apps = g_settings_get_strv (priv->settings, "popular-overrides");
		}
		if (apps != NULL && g_strv_length (apps) > 0) {
			GsAppList *list = gs_plugin_job_get_list (plugin_job);
			for (guint i = 0; apps[i] != NULL; i++) {
				g_autoptr(GsApp) app = gs_app_new (apps[i]);
				gs_app_add_quirk (app, AS_APP_QUIRK_MATCH_ANY_PREFIX);
				gs_app_list_add (list, app);
			}
			gs_plugin_job_set_action (plugin_job, GS_PLUGIN_ACTION_REFINE);
		}
	}

	/* FIXME: the plugins should specify this, rather than hardcoding */
	if (gs_plugin_job_has_refine_flags (plugin_job,
					    GS_PLUGIN_REFINE_FLAGS_REQUIRE_KEY_COLORS)) {
		gs_plugin_job_add_refine_flags (plugin_job,
						GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON);
	}
	if (gs_plugin_job_has_refine_flags (plugin_job,
					    GS_PLUGIN_REFINE_FLAGS_REQUIRE_ORIGIN_UI)) {
		gs_plugin_job_add_refine_flags (plugin_job,
						GS_PLUGIN_REFINE_FLAGS_REQUIRE_ORIGIN);
	}
	if (gs_plugin_job_has_refine_flags (plugin_job,
					    GS_PLUGIN_REFINE_FLAGS_REQUIRE_ORIGIN_HOSTNAME)) {
		gs_plugin_job_add_refine_flags (plugin_job,
						GS_PLUGIN_REFINE_FLAGS_REQUIRE_ORIGIN);
	}
	if (gs_plugin_job_has_refine_flags (plugin_job,
					    GS_PLUGIN_REFINE_FLAGS_REQUIRE_SIZE)) {
		gs_plugin_job_add_refine_flags (plugin_job,
						GS_PLUGIN_REFINE_FLAGS_REQUIRE_RUNTIME);
	}

	/* FIXME: this is probably a bug */
	if (action == GS_PLUGIN_ACTION_GET_DISTRO_UPDATES ||
	    action == GS_PLUGIN_ACTION_GET_SOURCES) {
		gs_plugin_job_add_refine_flags (plugin_job,
						GS_PLUGIN_REFINE_FLAGS_REQUIRE_SETUP_ACTION);
	}

	/* check required args */
	task = g_task_new (plugin_loader, cancellable, callback, user_data);
	switch (action) {
	case GS_PLUGIN_ACTION_SEARCH:
	case GS_PLUGIN_ACTION_SEARCH_FILES:
	case GS_PLUGIN_ACTION_SEARCH_PROVIDES:
	case GS_PLUGIN_ACTION_URL_TO_APP:
		if (gs_plugin_job_get_search (plugin_job) == NULL) {
			g_task_return_new_error (task,
						 GS_PLUGIN_ERROR,
						 GS_PLUGIN_ERROR_NOT_SUPPORTED,
						 "no valid search terms");
			return;
		}
		break;
	case GS_PLUGIN_ACTION_REVIEW_SUBMIT:
	case GS_PLUGIN_ACTION_REVIEW_UPVOTE:
	case GS_PLUGIN_ACTION_REVIEW_DOWNVOTE:
	case GS_PLUGIN_ACTION_REVIEW_REPORT:
	case GS_PLUGIN_ACTION_REVIEW_REMOVE:
	case GS_PLUGIN_ACTION_REVIEW_DISMISS:
		if (gs_plugin_job_get_review (plugin_job) == NULL) {
			g_task_return_new_error (task,
						 GS_PLUGIN_ERROR,
						 GS_PLUGIN_ERROR_NOT_SUPPORTED,
						 "no valid review object");
			return;
		}
		break;
	default:
		break;
	}

	/* sorting fallbacks */
	switch (action) {
	case GS_PLUGIN_ACTION_SEARCH:
		if (gs_plugin_job_get_sort_func (plugin_job) == NULL) {
			gs_plugin_job_set_sort_func (plugin_job,
						     gs_plugin_loader_app_sort_match_value_cb);
		}
		break;
	case GS_PLUGIN_ACTION_GET_RECENT:
		if (gs_plugin_job_get_sort_func (plugin_job) == NULL) {
			gs_plugin_job_set_sort_func (plugin_job,
						     gs_plugin_loader_app_sort_kind_cb);
		}
		break;
	case GS_PLUGIN_ACTION_GET_CATEGORY_APPS:
		if (gs_plugin_job_get_sort_func (plugin_job) == NULL) {
			gs_plugin_job_set_sort_func (plugin_job,
						     gs_plugin_loader_app_sort_name_cb);
		}
		break;
	default:
		break;
	}

	/* save helper */
	helper = gs_plugin_loader_helper_new (plugin_loader, plugin_job);
	g_task_set_task_data (task, helper, (GDestroyNotify) gs_plugin_loader_helper_free);
	gs_plugin_loader_job_debug (helper);

	/* run in a thread */
	g_task_run_in_thread (task, gs_plugin_loader_process_thread_cb);
}

/******************************************************************************/

/**
 * gs_plugin_loader_get_plugin_supported:
 *
 * This function returns TRUE if the symbol is found in any enabled plugin.
 */
gboolean
gs_plugin_loader_get_plugin_supported (GsPluginLoader *plugin_loader,
				       const gchar *function_name)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	for (guint i = 0; i < priv->plugins->len; i++) {
		GsPlugin *plugin = g_ptr_array_index (priv->plugins, i);
		if (gs_plugin_get_symbol (plugin, function_name) != NULL)
			return TRUE;
	}
	return FALSE;
}

/**
 * gs_plugin_loader_app_create:
 * @plugin_loader: a #GsPluginLoader
 * @unique_id: a unique_id
 *
 * Returns an application from the global cache, creating if required.
 *
 * Returns: (transfer full): a #GsApp
 **/
GsApp *
gs_plugin_loader_app_create (GsPluginLoader *plugin_loader, const gchar *unique_id)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	GsApp *app;

	/* already exists */
	app = gs_app_list_lookup (priv->global_cache, unique_id);
	if (app != NULL)
		return g_object_ref (app);

	/* create and add */
	app = gs_app_new_from_unique_id (unique_id);
	gs_app_list_add (priv->global_cache, app);
	return app;
}

/**
 * gs_plugin_loader_get_system_app:
 * @plugin_loader: a #GsPluginLoader
 *
 * Returns the application that represents the currently installed OS.
 *
 * Returns: (transfer full): a #GsApp
 **/
GsApp *
gs_plugin_loader_get_system_app (GsPluginLoader *plugin_loader)
{
	return gs_plugin_loader_app_create (plugin_loader, "*/*/*/*/system/*");
}

AsProfile *
gs_plugin_loader_get_profile (GsPluginLoader *plugin_loader)
{
	GsPluginLoaderPrivate *priv = gs_plugin_loader_get_instance_private (plugin_loader);
	return priv->profile;
}

/* vim: set noexpandtab: */