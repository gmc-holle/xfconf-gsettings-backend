/*
 * Xfconf GSettings backend
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the licence, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Stephan Haller <nomad@froevel.de>
 */

// TODO: #include "config.h"

#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>
#include <gio/gio.h>

#include <xfconf/xfconf.h>

#include <string.h>

#define XFCONF_SETTINGS_CHANNEL		"xfconf-gsettings"

/* Define this class in GObject system */
typedef GSettingsBackendClass		XfconfSettingsBackendClass;

typedef struct
{
	GSettingsBackend	backend;

	XfconfChannel		*channel;
} XfconfSettingsBackend;

static GType xfconf_settings_backend_get_type(void);

G_DEFINE_TYPE(XfconfSettingsBackend,
				xfconf_settings_backend,
				G_TYPE_SETTINGS_BACKEND)

/* IMPLEMENTATION: Private variables and methods */
typedef struct _XfconfSettingsBackendTreeWriteData			XfconfSettingsBackendTreeWriteData;
struct _XfconfSettingsBackendTreeWriteData
{
	XfconfSettingsBackend		*backend;
	gpointer					originTag;
	GHashTable					*writtenKeys;
	gboolean					success;
};

typedef struct _XfconfSettingsBackendTreeCollectKeysData	XfconfSettingsBackendTreeCollectKeysData;
struct _XfconfSettingsBackendTreeCollectKeysData
{
	gchar						**keysList;
	guint						index;
};


/* Forward declarations */
static void _xfconf_settings_backend_reset(GSettingsBackend *inBackend,
											const gchar *inKey,
											gpointer inOriginTag);

/* Find matching GType for a GVariant type */
static GType _xfconf_settings_backend_gtype_from_gvariant_type(const GVariantType *inVariantType)
{
	g_return_val_if_fail(inVariantType, G_TYPE_INVALID);

	switch(g_variant_type_peek_string(inVariantType)[0])
	{
		case G_VARIANT_CLASS_BOOLEAN:
			return(G_TYPE_BOOLEAN);

		case G_VARIANT_CLASS_BYTE:
			return(G_TYPE_UCHAR);

		case G_VARIANT_CLASS_INT16:
			return(XFCONF_TYPE_INT16);

		case G_VARIANT_CLASS_UINT16:
			return(XFCONF_TYPE_UINT16);

		case G_VARIANT_CLASS_INT32:
			return(G_TYPE_INT);

		case G_VARIANT_CLASS_UINT32:
			return(G_TYPE_UINT);

		case G_VARIANT_CLASS_INT64:
			return(G_TYPE_INT64);

		case G_VARIANT_CLASS_UINT64:
			return(G_TYPE_INT64);

		case G_VARIANT_CLASS_DOUBLE:
			return(G_TYPE_DOUBLE);

		case G_VARIANT_CLASS_STRING:
		case G_VARIANT_CLASS_OBJECT_PATH:
		case G_VARIANT_CLASS_SIGNATURE:
			return(G_TYPE_STRING);

		/* The following type cannot be mapped to a GValue type
		 * processable by xfconf.
		 */
		case G_VARIANT_CLASS_HANDLE:
		case G_VARIANT_CLASS_VARIANT:
		case G_VARIANT_CLASS_MAYBE:
		case G_VARIANT_CLASS_ARRAY:
		case G_VARIANT_CLASS_TUPLE:
		case G_VARIANT_CLASS_DICT_ENTRY:
		default:
			break;
	}

	return(G_TYPE_INVALID);
}

/* Store a value in xfconf */
static gboolean _xfconf_settings_backend_write_internal(GSettingsBackend *inBackend,
														const gchar *inKey,
														GVariant *inValue,
														gpointer inOriginTag)
{
	XfconfSettingsBackend		*self=(XfconfSettingsBackend*)inBackend;
	GValue						xfconfValue=G_VALUE_INIT;
	GType						xfconfValueType;
	gboolean					success;

g_message("%s: Writing key '%s'", __func__, inKey);

	/* Get GType of property value for variant */
	xfconfValueType=_xfconf_settings_backend_gtype_from_gvariant_type(g_variant_get_type(inValue));

	/* If variant type could not be mapped to a GType than get a string
	 * representation of variant which will be store instead ...
	 */
	if(xfconfValueType==G_TYPE_INVALID)
	{
		gchar					*variantString;

		/* Get string representation of variant */
		variantString=g_variant_print(inValue, FALSE);

		/* Set up property value */
		g_value_init(&xfconfValue, G_TYPE_STRING);
		g_value_set_string(&xfconfValue, variantString);
		g_message("%s: Writing key '%s' with string representation '%s'", __func__, inKey, variantString);

		/* Release allocated resources */
		g_free(variantString);
	}
		/* ... otherwise the variant can be simply converted */
		else
		{
			/* Convert variant to GValue */
			g_dbus_gvariant_to_gvalue(inValue, &xfconfValue);
			g_message("%s: Writing key '%s' with converted value", __func__, inKey);
		}

	/* Store value in xfconf */
	success=xfconf_channel_set_property(self->channel, inKey, &xfconfValue);
	{
		gchar					*valueStr;

		valueStr=g_strdup_value_contents(&xfconfValue);
		g_message("%s: Key '%s' with value '%s' -> %s", __func__, inKey, valueStr, success ? "success" : "failed");
		g_free(valueStr);
	}

	/* Release allocated resources */
	g_value_unset(&xfconfValue);

	/* Return success result */
	return(success);
}

/* Reset a value in xfconf */
static gboolean _xfconf_settings_backend_reset_internal(GSettingsBackend *inBackend,
														const gchar *inKey,
														gpointer inOriginTag)
{
	XfconfSettingsBackend		*self=(XfconfSettingsBackend*)inBackend;
	gboolean					doRecursive=TRUE;

	g_message("%s: Resetting value of key '%s'", __func__, inKey);

	/* If key does not exists return FALSE here */
	if(!xfconf_channel_has_property(self->channel, inKey))
	{
		g_message("%s: Cannot reset value of non-existing key '%s'", __func__, inKey);
		return(FALSE);
	}

	/* Reset value in xfconf */
	xfconf_channel_reset_property(self->channel, inKey, doRecursive);

	/* Return success result */
	return(TRUE);
}


/* IMPLEMENTATION: GSettingsBackend */

/* Read a value from xfconf */
static GVariant* _xfconf_settings_backend_read(GSettingsBackend *inBackend,
												const gchar *inKey,
												const GVariantType *inExpectedType,
												gboolean inDefaultValue)
{
	XfconfSettingsBackend		*self=(XfconfSettingsBackend*)inBackend;
	GValue						xfconfValue=G_VALUE_INIT;
	GType						xfconfValueType;
	GVariant					*value;

	value=NULL;

g_message("%s: Reading key '%s' -> default=%s, expected type '%s'", __func__, inKey, inDefaultValue ? "yes" : "no", g_variant_type_peek_string(inExpectedType));

	/* If default value is requested return NULL */
	if(inDefaultValue) return(NULL);

	/* Get value from property */
	if(!xfconf_channel_get_property(self->channel, inKey, &xfconfValue))
	{
		g_message("%s: Key '%s' not found", __func__, inKey);

		/* Release allocated resources */
		if(G_IS_VALUE(&xfconfValue)) g_value_unset(&xfconfValue);

		return(NULL);
	}

	/* If variant type could not be mapped to a GType than the variant
	 * has to be created from a string representation ...
	 */
	xfconfValueType=_xfconf_settings_backend_gtype_from_gvariant_type(inExpectedType);
	if(xfconfValueType==G_TYPE_INVALID)
	{
		GError					*error;

		error=NULL;

		g_message("%s: Reading key '%s' by parsing string representation", __func__, inKey);

		/* Property value must be a string */
		if(!G_VALUE_HOLDS_STRING(&xfconfValue))
		{
			g_message("%s: Key '%s' has no value of type string but it is needed to convert to variant", __func__, inKey);

			/* Release allocated resources */
			g_value_unset(&xfconfValue);

			return(NULL);
		}
		g_message("%s: Reading key '%s' by parsing string representation '%s'", __func__, inKey, g_value_get_string(&xfconfValue));

		/* Create variant from string representation */
		value=g_variant_parse(NULL,
								g_value_get_string(&xfconfValue),
								NULL,
								NULL,
								&error);
		if(!value || error)
		{
			g_message("%s: Failed to create variant for key '%s' from '%s': %s",
						__func__,
						inKey,
						g_value_get_string(&xfconfValue),
						error ? error->message : "Unknown error");

			/* Release allocated resources */
			if(value) g_variant_unref(value);
			if(error) g_error_free(error);
			g_value_unset(&xfconfValue);

			return(NULL);
		}
	}
		/* ... otherwise it can be simply converted */
		else
		{
			/* Convert property value to variant */
			value=g_dbus_gvalue_to_gvariant(&xfconfValue, inExpectedType);
			g_message("%s: Reading key '%s' by conversion", __func__, inKey);
		}

	/* Release allocated resources */
	g_value_unset(&xfconfValue);

	/* Return variant created from property value */
	return(value);
}

/* Store a value to xfconf */
static gboolean _xfconf_settings_backend_write(GSettingsBackend *inBackend,
												const gchar *inKey,
												GVariant *inValue,
												gpointer inOriginTag)
{
	gboolean		success;

	/* Write value to xfconf */
	if(inValue)
	{
		success=_xfconf_settings_backend_write_internal(inBackend,
															inKey,
															inValue,
															inOriginTag);
	}
		else
		{
			success=_xfconf_settings_backend_reset_internal(inBackend,
																inKey,
																inOriginTag);
		}

	/* Emit 'changed' signal if writing was successful */
	if(success) g_settings_backend_changed(inBackend, inKey, inOriginTag);

	/* Return success result */
	return(success);
}

/* Store a set of values (tree) to xfconf */
static gboolean _xfconf_settings_backend_write_tree_callback(gpointer inKey,
																gpointer inValue,
																gpointer inUserData)
{
	XfconfSettingsBackendTreeWriteData		*data;
	const gchar								*key;
	GVariant								*variant;

	/* Get callback data */
	data=(XfconfSettingsBackendTreeWriteData*)inUserData;

	/* If at any time writing a value has failed, stop writing any further values */
	if(!data->success) return(FALSE);

	/* Get key and value to write */
	key=(const gchar*)inKey;
	variant=(GVariant*)inValue;

	/* Write value to xfconf and store writing success result in callback data
	 * if a variant is given for this key. If no variant is given (NULL pointer)
	 * then a reset of the key is requested.
	 */
	if(variant)
	{
		data->success=_xfconf_settings_backend_write_internal((GSettingsBackend*)data->backend,
																key,
																variant,
																data->originTag);
	}
		else
		{
			data->success=_xfconf_settings_backend_reset_internal((GSettingsBackend*)data->backend,
																	key,
																	data->originTag);
		}

	/* Return TRUE if writing failed to stop traversal on tree */
	if(!data->success) return(TRUE);

	/* If writing was successful remember the modified key and return FALSE
	 * to continue tree traversal.
	 */
	if(data->writtenKeys)
	{
		g_hash_table_insert(data->writtenKeys, g_strdup(key), GINT_TO_POINTER(1));
	}

	return(FALSE);
}

static void _xfconf_settings_backend_write_tree_collect_modified_keys(gpointer inKey,
																		gpointer inValue,
																		gpointer inUserData)
{
	gchar										*key;
	XfconfSettingsBackendTreeCollectKeysData	*data;

	key=(gchar*)inKey;
	data=(XfconfSettingsBackendTreeCollectKeysData*)inUserData;

	/* Add key to string array */
	data->keysList[data->index]=g_strdup(key);
g_message("%s: Index=%d -> %s", __func__, data->index, data->keysList[data->index]);
	data->index++;
}

static gboolean _xfconf_settings_backend_write_tree(GSettingsBackend *inBackend,
													GTree *inTree,
													gpointer inOriginTag)
{
	XfconfSettingsBackend						*self=(XfconfSettingsBackend*)inBackend;
	XfconfSettingsBackendTreeWriteData			writeData;
	guint										modifiedKeysCount;
	XfconfSettingsBackendTreeCollectKeysData	collectKeysData;

	/* If tree is empty there is nothing to store and writing was successful */
	if(g_tree_nnodes(inTree)==0)
	{
		g_message("%s: Empty tree", __func__);
		return(TRUE);
	}
	g_message("%s: Writing tree with %d nodes", __func__, g_tree_nnodes(inTree));

	/* Write each value to xfconf */
	writeData.backend=self;
	writeData.originTag=inOriginTag;
	writeData.writtenKeys=g_hash_table_new_full(g_str_hash,
												g_str_equal,
												(GDestroyNotify)g_free,
												NULL);
	writeData.success=TRUE;
	g_tree_foreach(inTree, _xfconf_settings_backend_write_tree_callback, &writeData);

	/* Emit 'path-changed' signal with all modified keys regardless if writing
	 * all keys was successful or not.
	 */
	modifiedKeysCount=g_hash_table_size(writeData.writtenKeys);
	if(modifiedKeysCount>0)
	{
		collectKeysData.keysList=(gchar**)g_new0(gchar*, modifiedKeysCount+1);
		collectKeysData.index=0;
		g_hash_table_foreach(writeData.writtenKeys,
								_xfconf_settings_backend_write_tree_collect_modified_keys,
								&collectKeysData);

		if(modifiedKeysCount==1)
		{
			g_settings_backend_changed(inBackend,
										*collectKeysData.keysList,
										inOriginTag);
		}
			else
			{
				g_settings_backend_keys_changed(inBackend,
												"/",
												(const gchar **)collectKeysData.keysList,
												inOriginTag);
			}

		g_strfreev(collectKeysData.keysList);
	}
	g_message("%s: Modified %d keys", __func__, modifiedKeysCount);

	/* Release allocated resources */
	g_hash_table_unref(writeData.writtenKeys);

	/* Return success result */
	return(writeData.success);
}

/* Reset a value in xfconf */
static void _xfconf_settings_backend_reset(GSettingsBackend *inBackend,
											const gchar *inKey,
											gpointer inOriginTag)
{
	XfconfSettingsBackend		*self=(XfconfSettingsBackend*)inBackend;
	gboolean					success;

	g_message("%s: Resetting value of key '%s'", __func__, inKey);

	/* Reset value in xfconf */
	success=_xfconf_settings_backend_reset_internal(inBackend, inKey, inOriginTag);

	/* Emit 'changed' signal if resetting was successful */
	if(success) g_settings_backend_changed(inBackend, inKey, inOriginTag);
}

/* Get writable state of a key at xfconf */
static gboolean _xfconf_settings_backend_get_writable(GSettingsBackend *inBackend,
														const gchar *inKey)
{
	XfconfSettingsBackend		*self=(XfconfSettingsBackend*)inBackend;
	gboolean					isWritable;

	isWritable=!xfconf_channel_is_property_locked(self->channel, inKey);
g_message("%s: %s -> %s", __func__, inKey, isWritable ? "writable" : "read-only");
	return(isWritable);
}

/* Subscribe */
static void _xfconf_settings_backend_subscribe(GSettingsBackend *inBackend,
												const gchar *inKey)
{
	XfconfSettingsBackend		*self=(XfconfSettingsBackend*)inBackend;

	/* TODO: I do not know what to do here :( */
	g_message("%s: Subscribe '%s' - not yet implemented!", __func__, inKey);
}

/* Unsubscribe */
static void _xfconf_settings_backend_unsubscribe(GSettingsBackend *inBackend,
													const gchar *inKey)
{
	XfconfSettingsBackend		*self=(XfconfSettingsBackend*)inBackend;

	/* TODO: I do not know what to do here :( */
	g_message("%s: Unsubscribe '%s' - not yet implemented!", __func__, inKey);
}

/* Sync */
static void _xfconf_settings_backend_sync(GSettingsBackend *inBackend)
{
	/* Do nothing - is always synced */
	g_message("%s: Sync not needed", __func__);
}

/* IMPLEMENTATION: GObject */

/* Finalize this object */
static void _xfconf_settings_backend_finalize(GObject *inObject)
{
	XfconfSettingsBackend		*self=(XfconfSettingsBackend*)inObject;

	if(self->channel)
	{
		g_object_unref(self->channel);
		self->channel=NULL;
	}

	G_OBJECT_CLASS(xfconf_settings_backend_parent_class)->finalize(inObject);
}

/* Class initialization
 * Override functions in parent classes and define properties
 * and signals
 */
static void xfconf_settings_backend_class_init(GSettingsBackendClass *klass)
{
	GObjectClass	*gobjectClass=G_OBJECT_CLASS(klass);

	/* Override functions */
	gobjectClass->finalize=_xfconf_settings_backend_finalize;

	klass->read=_xfconf_settings_backend_read;
	klass->write=_xfconf_settings_backend_write;
	klass->write_tree=_xfconf_settings_backend_write_tree;
	klass->reset=_xfconf_settings_backend_reset;
	klass->get_writable=_xfconf_settings_backend_get_writable;
	klass->subscribe=_xfconf_settings_backend_subscribe;
	klass->unsubscribe=_xfconf_settings_backend_unsubscribe;
	klass->sync=_xfconf_settings_backend_sync;
}

/* Object initialization
 * Create private structure and set up default values
 */
static void xfconf_settings_backend_init(XfconfSettingsBackend *self)
{
	/* Set default values */
	self->channel=xfconf_channel_new(XFCONF_SETTINGS_CHANNEL);
}

void g_io_module_load(GIOModule *inModule)
{
	GError		*error;

	error=NULL;
	if(!xfconf_init(&error))
	{
		g_critical("Could not initialize xfconf: %s", error->message);
		g_error_free(error);
		return;
	}

	g_type_module_use(G_TYPE_MODULE(inModule));
	g_io_extension_point_implement(G_SETTINGS_BACKEND_EXTENSION_POINT_NAME,
									xfconf_settings_backend_get_type(),
									"xfconf",
									-1);
	g_message("Module loaded: xfconf-gsettings");
}

void g_io_module_unload(GIOModule *inModule)
{
	xfconf_shutdown();
	g_message("Module unloaded: xfconf-gsettings");
}

gchar** g_io_module_query(void)
{
	return(g_strsplit(G_SETTINGS_BACKEND_EXTENSION_POINT_NAME, "!", 0));
}
