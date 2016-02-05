/*
 * Xfconf GSettings backend
 *
 * Copyright 2015-2016 Stephan Haller <nomad@froevel.de>
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
 *
 */

// TODO: #include "config.h"

#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>
#include <gio/gio.h>

#include <xfconf/xfconf.h>

/* If defined variants are stored as an array containing magic number,
 * signature and value. If it is not defined variants are stored just
 * as a serialized string.
 */
#undef STORE_COMPLEX_VARIANTS

/* If defined print debug message. Do not define for silence ;) */
#define DEBUG


/* Definitions */
#define XFCONF_SETTINGS_CHANNEL			"xfconf-gsettings"

#ifdef STORE_COMPLEX_VARIANTS
#define XFCONF_VARIANT_STRUCT_MAGIC		((guint32)('G' << 24 | 'V' << 16 | 'a' << 8 | 'r'))
#define XFCONF_VARIANT_STRUCT_NAME		"xfconf-gsettings-variant-struct"
#endif

/* Define this class in GObject system */
typedef struct _XfconfSettingsBackendClass					XfconfSettingsBackendClass;
struct _XfconfSettingsBackendClass
{
	/*< private >*/
	/* Parent class */
	GSettingsBackendClass	parent_class;
};

typedef struct _XfconfSettingsBackend						XfconfSettingsBackend;
struct _XfconfSettingsBackend
{
	/* Parent instance */
	GSettingsBackend		backend;

	/* Private structure */
	XfconfChannel			*channel;
};

G_DEFINE_TYPE(XfconfSettingsBackend,
				xfconf_settings_backend,
				G_TYPE_SETTINGS_BACKEND)

#define XFCONF_TYPE_SETTINGS_BACKEND						(xfconf_settings_backend_get_type())
#define XFCONF_SETTINGS_BACKEND(obj)						(G_TYPE_CHECK_INSTANCE_CAST((obj), XFCONF_TYPE_SETTINGS_BACKEND, XfconfSettingsBackend))


/* IMPLEMENTATION: Private variables and methods */
typedef struct _XfconfSettingsBackendTypeMapping			XfconfSettingsBackendTypeMapping;
struct _XfconfSettingsBackendTypeMapping
{
	GType					type;
	GType					subType;

	const GVariantType		*variantType;
	const GVariantType		*variantSubtype;
};

typedef struct _XfconfSettingsBackendTreeWriteData			XfconfSettingsBackendTreeWriteData;
struct _XfconfSettingsBackendTreeWriteData
{
	XfconfSettingsBackend	*backend;
	gpointer				originTag;
	GHashTable				*writtenKeys;
};

typedef struct _XfconfSettingsBackendTreeCollectKeysData	XfconfSettingsBackendTreeCollectKeysData;
struct _XfconfSettingsBackendTreeCollectKeysData
{
	gchar					**keysList;
	guint					index;
};

#ifdef STORE_COMPLEX_VARIANTS
typedef struct _XfconfSettingsBackendVariantStruct			XfconfSettingsBackendVariantStruct;
struct _XfconfSettingsBackendVariantStruct
{
	guint					magic;
	gchar					*signature;
	gchar					*value;
};
#endif


/* Forward declarations */
static void _xfconf_settings_backend_reset(GSettingsBackend *inBackend,
											const gchar *inKey,
											gpointer inOriginTag);

#ifdef DEBUG
void _xfconf_settings_backend_debug(const gchar *inFormat, ...) G_GNUC_PRINTF(1, 2);

/* Print debug messages */
void _xfconf_settings_backend_debug(const gchar *inFormat, ...)
{
	gchar			*text;
	va_list			args;

	g_return_if_fail(inFormat && *inFormat);

	/* Build formatted text to set */
	va_start(args, inFormat);
	text=g_strdup_vprintf(inFormat, args);
	va_end(args);

	/* Show debug message */
	g_print("[xfconf-settings-backend debug]: %s\n", text);

	/* Release allocated resources */
	g_free(text);
}
#else
#define _xfconf_settings_backend_debug(inFormat, ...)
#endif

#ifdef STORE_COMPLEX_VARIANTS
/* Initialize variant structure */
static void _xfconf_settings_backend_init_variant_struct(XfconfSettingsBackendVariantStruct *inStruct)
{
	g_return_if_fail(inStruct);

	/* Initialize structure */
	inStruct->magic=XFCONF_VARIANT_STRUCT_MAGIC;
	inStruct->signature=NULL;
	inStruct->value=NULL;
}

/* Free variant structure */
static void _xfconf_settings_backend_free_variant_struct(XfconfSettingsBackendVariantStruct *inStruct)
{
	g_return_if_fail(inStruct);
	g_return_if_fail(inStruct->magic==XFCONF_VARIANT_STRUCT_MAGIC);

	/* Free allocated resources */
	if(inStruct->signature)
	{
		g_free(inStruct->signature);
		inStruct->signature=NULL;
	}

	if(inStruct->value)
	{
		g_free(inStruct->value);
		inStruct->value=NULL;
	}
}
#endif

/* Find matching GType for a GVariant type */
static gboolean _xfconf_settings_backend_gtype_from_gvariant_type(const GVariantType *inVariantType, XfconfSettingsBackendTypeMapping *ioMapping)
{
	const gchar				*iter;
	gsize					numberTypes;

	g_return_val_if_fail(inVariantType, FALSE);
	g_return_val_if_fail(ioMapping, FALSE);

	/* Initialize GVariant's type iterator */
	iter=g_variant_type_peek_string(inVariantType);
	numberTypes=g_variant_type_get_string_length(inVariantType);

	/* Initialize mapping with invalid types */
	ioMapping->type=G_TYPE_INVALID;
	ioMapping->subType=G_TYPE_INVALID;
	ioMapping->variantType=NULL;
	ioMapping->variantSubtype=NULL;

	/* If GVariant's signaure is a container and it can be handled like an
	 * array then get array type but it must have only two types in its
	 * signature *exactly*.
	 */
	switch(*iter)
	{
		case G_VARIANT_CLASS_ARRAY:
			if(numberTypes==2)
			{
				ioMapping->type=G_TYPE_ARRAY;
				ioMapping->variantType=G_VARIANT_TYPE_ARRAY;
			}
			break;

		/* GVariant's signature either not describes an container or it cannot
		 * be handled like an array.
		 */
		default:
			break;
	}

	/* GVariant's signature must define exactly one type to map it to a GType
	 * if it's not an array-like container. If it can be handled as an array-alike
	 * then if must have exactly two type in its signature.
	 */
	if((ioMapping->type!=G_TYPE_INVALID && numberTypes==2) ||
		(ioMapping->type==G_TYPE_INVALID && numberTypes==1))
	{
		/* Move iterator if it is array-alike */
		if(ioMapping->type!=G_TYPE_INVALID) iter++;

		/* Try mapping type */
		switch(*iter)
		{
			case G_VARIANT_CLASS_BOOLEAN:
				ioMapping->subType=G_TYPE_BOOLEAN;
				ioMapping->variantSubtype=G_VARIANT_TYPE_BOOLEAN;
				break;

			case G_VARIANT_CLASS_BYTE:
				ioMapping->subType=G_TYPE_UCHAR;
				ioMapping->variantSubtype=G_VARIANT_TYPE_BYTE;
				break;

			case G_VARIANT_CLASS_INT16:
				ioMapping->subType=XFCONF_TYPE_INT16;
				ioMapping->variantSubtype=G_VARIANT_TYPE_INT16;
				break;

			case G_VARIANT_CLASS_UINT16:
				ioMapping->subType=XFCONF_TYPE_UINT16;
				ioMapping->variantSubtype=G_VARIANT_TYPE_UINT16;
				break;

			case G_VARIANT_CLASS_INT32:
				ioMapping->subType=G_TYPE_INT;
				ioMapping->variantSubtype=G_VARIANT_TYPE_INT32;
				break;

			case G_VARIANT_CLASS_UINT32:
				ioMapping->subType=G_TYPE_UINT;
				ioMapping->variantSubtype=G_VARIANT_TYPE_UINT32;
				break;

			case G_VARIANT_CLASS_INT64:
				ioMapping->subType=G_TYPE_INT64;
				ioMapping->variantSubtype=G_VARIANT_TYPE_INT64;
				break;

			case G_VARIANT_CLASS_UINT64:
				ioMapping->subType=G_TYPE_INT64;
				ioMapping->variantSubtype=G_VARIANT_TYPE_UINT64;
				break;

			case G_VARIANT_CLASS_DOUBLE:
				ioMapping->subType=G_TYPE_DOUBLE;
				ioMapping->variantSubtype=G_VARIANT_TYPE_DOUBLE;
				break;

			case G_VARIANT_CLASS_STRING:
				ioMapping->subType=G_TYPE_STRING;
				ioMapping->variantSubtype=G_VARIANT_TYPE_STRING;
				break;

			case G_VARIANT_CLASS_OBJECT_PATH:
				ioMapping->subType=G_TYPE_STRING;
				ioMapping->variantSubtype=G_VARIANT_TYPE_OBJECT_PATH;
				break;

			case G_VARIANT_CLASS_SIGNATURE:
				ioMapping->subType=G_TYPE_STRING;
				ioMapping->variantSubtype=G_VARIANT_TYPE_SIGNATURE;
				break;

			/* The following type cannot be mapped to a GValue type processable
			 * by xfconf properly. If it is array-alike then reset also its type.
			 */
			case G_VARIANT_CLASS_HANDLE:
			case G_VARIANT_CLASS_VARIANT:
			case G_VARIANT_CLASS_MAYBE:
			case G_VARIANT_CLASS_ARRAY:
			case G_VARIANT_CLASS_TUPLE:
			case G_VARIANT_CLASS_DICT_ENTRY:
			default:
				ioMapping->type=G_TYPE_INVALID;
				ioMapping->variantType=NULL;
				break;
		}

		/* If sub-type is set but type is invalid move sub-type to type */
		if(ioMapping->subType!=G_TYPE_INVALID &&
			ioMapping->type==G_TYPE_INVALID)
		{
			ioMapping->type=ioMapping->subType;
			ioMapping->subType=G_TYPE_INVALID;

			ioMapping->variantType=ioMapping->variantSubtype;
			ioMapping->variantSubtype=NULL;
		}
	}

	/* Return with success result even if no mapping could be found */
	_xfconf_settings_backend_debug("GVariant's signature '%s' with %lu elements resolved to type %s and sub-type %s",
									g_variant_type_peek_string(inVariantType),
									numberTypes,
									g_type_name(ioMapping->type),
									g_type_name(ioMapping->subType));
	return(TRUE);
}

/* Store a value in xfconf */
static gboolean _xfconf_settings_backend_write_internal(XfconfSettingsBackend *self,
														const gchar *inKey,
														GVariant *inValue,
														gpointer inOriginTag)
{
	XfconfSettingsBackendTypeMapping		valueType;
	gboolean								success;

	/* Get GType of property value for variant */
	if(!_xfconf_settings_backend_gtype_from_gvariant_type(g_variant_get_type(inValue), &valueType))
	{
		g_critical("Failed to determine types when writting key %s.", inKey);
		return(FALSE);
	}

	/* If variant type could not be mapped to a GType than get a string
	 * representation of variant which will be store instead along with
	 * variant's signature ...
	 */
	if(valueType.type==G_TYPE_INVALID)
	{
#ifdef STORE_COMPLEX_VARIANTS
		XfconfSettingsBackendVariantStruct	variantStruct;

		/* Set up value to store */
		_xfconf_settings_backend_init_variant_struct(&variantStruct);
		variantStruct.signature=g_variant_type_dup_string(g_variant_get_type(inValue));
		variantStruct.value=g_variant_print(inValue, FALSE);

		/* Store value in xfconf */
		success=xfconf_channel_set_named_struct(self->channel, inKey, XFCONF_VARIANT_STRUCT_NAME, &variantStruct);

		/* Release allocated resources */
		_xfconf_settings_backend_free_variant_struct(&variantStruct);
#else
		gchar								*variantString;
		GValue								xfconfValue=G_VALUE_INIT;

		/* Get string representation of variant */
		variantString=g_variant_print(inValue, FALSE);

		/* Set up property value */
		g_value_init(&xfconfValue, G_TYPE_STRING);
		g_value_set_string(&xfconfValue, variantString);

		/* Store value in xfconf */
		success=xfconf_channel_set_property(self->channel, inKey, &xfconfValue);

		/* Release allocated resources */
		g_value_unset(&xfconfValue);
		g_free(variantString);
#endif
	}
		/* ... otherwise check for array ... */
		else if(valueType.type==G_TYPE_ARRAY && valueType.subType!=G_TYPE_INVALID)
		{
			gsize							arraySize;
			GPtrArray						*array;
			gsize							i;
			GValue							*xfconfValue;

			/* Get size of array */
			arraySize=g_variant_n_children(inValue);

			/* Set up array for storing in xfconf */
			array=g_ptr_array_sized_new(arraySize);
			for(i=0; i<arraySize; i++)
			{
				xfconfValue=g_new0(GValue, 1);
				g_dbus_gvariant_to_gvalue(g_variant_get_child_value(inValue, i), xfconfValue);
				g_ptr_array_add(array, xfconfValue);
			}

			/* Store value in xfconf */
			success=xfconf_channel_set_arrayv(self->channel, inKey, array);

			/* Release allocated resources */
			xfconf_array_free(array);
		}
		/* ... otherwise the variant can be simply converted */
		else
		{
			GValue							xfconfValue=G_VALUE_INIT;

			/* Convert variant to GValue */
			g_dbus_gvariant_to_gvalue(inValue, &xfconfValue);

			/* Store value in xfconf */
			success=xfconf_channel_set_property(self->channel, inKey, &xfconfValue);

			/* Release allocated resources */
			g_value_unset(&xfconfValue);
		}

	/* Return success result */
	_xfconf_settings_backend_debug("Wrote key '%s' %s",
									inKey,
									success ? "successfully" : "unsuccessfully");
	return(success);
}

/* Reset a value in xfconf */
static gboolean _xfconf_settings_backend_reset_internal(XfconfSettingsBackend *self,
														const gchar *inKey,
														gpointer inOriginTag)
{
	/* If key does not exists return FALSE here */
	if(!xfconf_channel_has_property(self->channel, inKey))
	{
		_xfconf_settings_backend_debug("Cannot reset non-existing key '%s'", inKey);
		return(FALSE);
	}

	/* Reset value in xfconf */
	xfconf_channel_reset_property(self->channel, inKey, TRUE);

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
	XfconfSettingsBackend					*self=(XfconfSettingsBackend*)inBackend;
	XfconfSettingsBackendTypeMapping		valueType;
	GVariant								*value;

	value=NULL;


	/* If default value is requested return NULL */
	if(inDefaultValue) return(NULL);

	/* Check that requested property exists */
	if(!xfconf_channel_has_property(self->channel, inKey))
	{
		_xfconf_settings_backend_debug("Cannot read non-existing key '%s'", inKey);
		return(NULL);
	}

	/* Get GType of property value for variant */
	if(!_xfconf_settings_backend_gtype_from_gvariant_type(inExpectedType, &valueType))
	{
		g_critical("Failed to determine types when writting key %s.", inKey);
		return(FALSE);
	}

	/* If variant type could not be mapped to a GType than the variant
	 * has to be created from a string representation ...
	 */
	if(valueType.type==G_TYPE_INVALID)
	{
#ifdef STORE_COMPLEX_VARIANTS
		XfconfSettingsBackendVariantStruct	variantStruct;
		GError								*error;

		error=NULL;

		/* Initialize struct to get value of property at */
		_xfconf_settings_backend_init_variant_struct(&variantStruct);

		/* Get value of property */
		if(!xfconf_channel_get_named_struct(self->channel, inKey, XFCONF_VARIANT_STRUCT_NAME, &variantStruct))
		{
			g_critical("Failed to get complex array to determine value for key '%s'", inKey);

			/* Release allocated resources */
			_xfconf_settings_backend_free_variant_struct(&variantStruct);

			return(NULL);
		}

		/* Create variant from string representation for expected type */
		value=g_variant_parse(inExpectedType,
								variantStruct.value,
								NULL,
								NULL,
								&error);
		if(!value || error)
		{
			g_critical("Failed to parse variant for key '%s' from '%s': %s",
						inKey,
						variantStruct.value,
						error ? error->message : "Unknown error");

			/* Release allocated resources */
			if(value) g_variant_unref(value);
			if(error) g_error_free(error);
			_xfconf_settings_backend_free_variant_struct(&variantStruct);

			return(NULL);
		}

		/* Release allocated resources */
		_xfconf_settings_backend_free_variant_struct(&variantStruct);
#else
		GValue								xfconfValue=G_VALUE_INIT;
		GError								*error;

		error=NULL;

		/* Get stored value of property */
		if(!xfconf_channel_get_property(self->channel, inKey, &xfconfValue))
		{
			g_critical("Failed to get value for key '%s'", inKey);

			/* Release allocated resources */
			if(G_IS_VALUE(&xfconfValue)) g_value_unset(&xfconfValue);

			return(NULL);
		}

		/* Property value must be a string */
		if(!G_VALUE_HOLDS_STRING(&xfconfValue))
		{
			g_critical("Failed to parse variant for key '%s': %s",
						inKey,
						"Value is not a string");

			/* Release allocated resources */
			g_value_unset(&xfconfValue);

			return(NULL);
		}

		/* Create variant from string representation for expected type */
		value=g_variant_parse(inExpectedType,
								g_value_get_string(&xfconfValue),
								NULL,
								NULL,
								&error);
		if(!value || error)
		{
			g_critical("Failed to parse variant for key '%s' from '%s': %s",
						inKey,
						g_value_get_string(&xfconfValue),
						error ? error->message : "Unknown error");

			/* Release allocated resources */
			g_value_unset(&xfconfValue);
			if(value) g_variant_unref(value);
			if(error) g_error_free(error);

			return(NULL);
		}

		/* Release allocated resources */
		g_value_unset(&xfconfValue);
#endif
	}
		/* ... otherwise check for array ... */
		else if(valueType.type==G_TYPE_ARRAY && valueType.subType!=G_TYPE_INVALID)
		{
			GPtrArray						*array;
			gsize							arraySize;
			GVariant						**elements;
			gsize							i;

			/* Get array from xfconf */
			array=xfconf_channel_get_arrayv(self->channel, inKey);
			if(array)
			{
				/* Get size of array */
				arraySize=array->len;

				/* Set up array for storing GVariants */
				elements=g_new0(GVariant*, arraySize);
				for(i=0; i<arraySize; i++)
				{
					elements[i]=g_dbus_gvalue_to_gvariant((GValue*)g_ptr_array_index(array, i), valueType.variantSubtype);
				}

				/* Get final GVariant array */
				value=g_variant_new_array(valueType.variantSubtype, elements, arraySize);

				/* Release allocated resources */
				xfconf_array_free(array);
			}
		}
		/* ... otherwise it can be simply converted */
		else
		{
			GValue							xfconfValue=G_VALUE_INIT;

			/* Get stored value of property */
			if(!xfconf_channel_get_property(self->channel, inKey, &xfconfValue))
			{
				g_critical("Failed to get value for key '%s'", inKey);

				/* Release allocated resources */
				if(G_IS_VALUE(&xfconfValue)) g_value_unset(&xfconfValue);

				return(NULL);
			}

			/* Convert property value to variant */
			value=g_dbus_gvalue_to_gvariant(&xfconfValue, inExpectedType);

			/* Release allocated resources */
			g_value_unset(&xfconfValue);
		}

	/* Return variant created from property value */
	_xfconf_settings_backend_debug("Read key '%s' %s",
									inKey,
									value ? "successfully" : "unsuccessfully");
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
		success=_xfconf_settings_backend_write_internal(XFCONF_SETTINGS_BACKEND(inBackend),
														inKey,
														inValue,
														inOriginTag);
	}
		else
		{
			success=_xfconf_settings_backend_reset_internal(XFCONF_SETTINGS_BACKEND(inBackend),
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
	gboolean								success;

	/* Get callback data */
	data=(XfconfSettingsBackendTreeWriteData*)inUserData;

	/* Get key and value to write */
	key=(const gchar*)inKey;
	variant=(GVariant*)inValue;

	/* Write value to xfconf and store writing success result in callback data
	 * if a variant is given for this key. If no variant is given (NULL pointer)
	 * then a reset of the key is requested.
	 */
	if(variant)
	{
		success=_xfconf_settings_backend_write_internal(XFCONF_SETTINGS_BACKEND(data->backend),
														key,
														variant,
														data->originTag);
	}
		else
		{
			success=_xfconf_settings_backend_reset_internal(XFCONF_SETTINGS_BACKEND(data->backend),
															key,
															data->originTag);
		}

	/* If writing was successful remember the modified key */
	if(success && data->writtenKeys)
	{
		g_hash_table_insert(data->writtenKeys, g_strdup(key), GINT_TO_POINTER(1));
	}

	/* Return FALSE to continue tree traversal regardless if this write was
	 * successful or not.
	 */
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
	data->index++;
}

static gboolean _xfconf_settings_backend_write_tree(GSettingsBackend *inBackend,
													GTree *inTree,
													gpointer inOriginTag)
{
	XfconfSettingsBackend						*self=(XfconfSettingsBackend*)inBackend;
	XfconfSettingsBackendTreeWriteData			writeData;
	gint										treeSize;
	guint										modifiedKeysCount;
	XfconfSettingsBackendTreeCollectKeysData	collectKeysData;

	/* If tree is empty there is nothing to store and writing was successful */
	treeSize=g_tree_nnodes(inTree);
	if(treeSize==0)
	{
		_xfconf_settings_backend_debug("Do not write tree because tree is empty");
		return(TRUE);
	}

	/* Write each value to xfconf */
	writeData.backend=self;
	writeData.originTag=inOriginTag;
	writeData.writtenKeys=g_hash_table_new_full(g_str_hash,
												g_str_equal,
												(GDestroyNotify)g_free,
												NULL);
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

	/* Release allocated resources */
	g_hash_table_unref(writeData.writtenKeys);

	/* Return success result */
	_xfconf_settings_backend_debug("Wrote tree with %d nodes and modified %d keys",
									treeSize,
									modifiedKeysCount);
	return(TRUE);
}

/* Reset a value in xfconf */
static void _xfconf_settings_backend_reset(GSettingsBackend *inBackend,
											const gchar *inKey,
											gpointer inOriginTag)
{
	XfconfSettingsBackend		*self=(XfconfSettingsBackend*)inBackend;
	gboolean					success;

	/* Reset value in xfconf */
	success=_xfconf_settings_backend_reset_internal(self, inKey, inOriginTag);

	/* Emit 'changed' signal if resetting was successful */
	if(success) g_settings_backend_changed(inBackend, inKey, inOriginTag);
}

/* Get writable state of a key at xfconf */
static gboolean _xfconf_settings_backend_get_writable(GSettingsBackend *inBackend,
														const gchar *inKey)
{
	XfconfSettingsBackend		*self=(XfconfSettingsBackend*)inBackend;
	gboolean					isWritable;

	/* Determine if key is writable */
	isWritable=!xfconf_channel_is_property_locked(self->channel, inKey);

	/* Return result */
	_xfconf_settings_backend_debug("Key '%s' is %s",
									inKey,
									isWritable ? "writable" : "read-only");
	return(isWritable);
}


/* IMPLEMENTATION: GObject */

/* Finalize this object */
static void _xfconf_settings_backend_finalize(GObject *inObject)
{
	XfconfSettingsBackend		*self=(XfconfSettingsBackend*)inObject;

	/* Release allocated resources */
	if(self->channel)
	{
		g_object_unref(self->channel);
		self->channel=NULL;
	}

	/* Call parent class virtual function */
	G_OBJECT_CLASS(xfconf_settings_backend_parent_class)->finalize(inObject);
}

/* Class initialization
 * Override functions in parent classes and define properties
 * and signals
 */
static void xfconf_settings_backend_class_init(XfconfSettingsBackendClass *klass)
{
	GSettingsBackendClass	*backendClass=G_SETTINGS_BACKEND_CLASS(klass);
	GObjectClass			*gobjectClass=G_OBJECT_CLASS(klass);

	/* Override functions */
	gobjectClass->finalize=_xfconf_settings_backend_finalize;

	backendClass->read=_xfconf_settings_backend_read;
	backendClass->write=_xfconf_settings_backend_write;
	backendClass->write_tree=_xfconf_settings_backend_write_tree;
	backendClass->reset=_xfconf_settings_backend_reset;
	backendClass->get_writable=_xfconf_settings_backend_get_writable;
}

/* Object initialization
 * Create private structure and set up default values
 */
static void xfconf_settings_backend_init(XfconfSettingsBackend *self)
{
	/* Set default values */
	self->channel=xfconf_channel_new(XFCONF_SETTINGS_CHANNEL);
}


/* IMPLEMENTATION: GIOModule */

/* Module loading and initialization */
void g_io_module_load(GIOModule *inModule)
{
	GError		*error;
#ifdef STORE_COMPLEX_VARIANTS
	GType		xfconfVariantStruct[]={ G_TYPE_UINT, G_TYPE_STRING, G_TYPE_STRING };
#endif

	error=NULL;

	/* Initialize xfconf */
	if(!xfconf_init(&error))
	{
		g_critical("Could not initialize xfconf: %s", error->message);
		g_error_free(error);
		return;
	}

	/* Register GSettings backend */
	g_type_module_use(G_TYPE_MODULE(inModule));
	g_io_extension_point_implement(G_SETTINGS_BACKEND_EXTENSION_POINT_NAME,
									xfconf_settings_backend_get_type(),
									"xfconf",
									-1);

#ifdef STORE_COMPLEX_VARIANTS
	/* Register named structs at xfconf */
	xfconf_named_struct_register(XFCONF_VARIANT_STRUCT_NAME,
									G_N_ELEMENTS(xfconfVariantStruct),
									xfconfVariantStruct);
#endif

#ifdef STORE_COMPLEX_VARIANTS
	_xfconf_settings_backend_debug("Module loaded: xfconf-gsettings (storing variants as complex arrays)");
#else
	_xfconf_settings_backend_debug("Module loaded: xfconf-gsettings (storing simple serialized variants)");
#endif
}

/* Module unloading */
void g_io_module_unload(GIOModule *inModule)
{
	/* Shutdown xfconf */
	xfconf_shutdown();

	_xfconf_settings_backend_debug("Module unloaded: xfconf-gsettings");
}

/* Module query */
gchar** g_io_module_query(void)
{
	return(g_strsplit(G_SETTINGS_BACKEND_EXTENSION_POINT_NAME, "!", 0));
}
