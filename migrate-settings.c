/*
 * Xfconf GSettings backend - migration tool
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

#include <stdio.h>


/* IMPLEMENTATION: Private variables and methods */
G_LOCK_DEFINE_STATIC(loaded);

typedef enum
{
	MIGRATE_MODE_NONE=0,

	MIGRATE_MODE_DRY_RUN=1 << 0,			/* Turn on dry-run */
	MIGRATE_MODE_CLEAN_DESTINATION=1 << 1,	/* Reset all keys before migration to get clean settings storage */
	MIGRATE_MODE_OVERWRITE=1 << 2,			/* Just overwrite existing keys at destincation backend */
} MigrateMode;

/* Ensures that all GIOModules are loaded */
void _ensure_loaded(void)
{
	static gboolean		loaded=FALSE;
	GIOModuleScope		*scope;
	const char			*modulePath;
	GIOExtensionPoint	*extensionPoint;

	G_LOCK(loaded);

	/* Load modules only once */
	if(!loaded)
	{
		/* Set flag that loading module was done */
		loaded=TRUE;

		/* Register extension point for modules providing GSettingsBackend */
		extensionPoint=g_io_extension_point_lookup(G_SETTINGS_BACKEND_EXTENSION_POINT_NAME);
		if(!extensionPoint)
		{
			extensionPoint=g_io_extension_point_register(G_SETTINGS_BACKEND_EXTENSION_POINT_NAME);
			g_io_extension_point_set_required_type (extensionPoint, G_TYPE_OBJECT);
		}

		/* Create scope for loading modules to avoid duplicates */
		scope=g_io_module_scope_new(G_IO_MODULE_SCOPE_BLOCK_DUPLICATES);

		/* First load any extra module that may be defined in GIO_EXTRA_MODULES */
		modulePath=g_getenv("GIO_EXTRA_MODULES");
		if(modulePath)
		{
			gchar		**paths;
			int			i;

			paths=g_strsplit(modulePath, G_SEARCHPATH_SEPARATOR_S, 0);
			for(i=0; paths[i]; i++)
			{
				g_io_modules_scan_all_in_directory_with_scope(paths[i], scope);
			}
			g_strfreev(paths);
		}

		/* Then load modules built into GIO from local module path */
		modulePath=g_getenv("GIO_MODULE_DIR");
		if(!modulePath) modulePath=GIO_MODULE_DIR;

		g_io_modules_scan_all_in_directory_with_scope(modulePath, scope);

		/* Release allocated resources */
		g_io_module_scope_free(scope);
	}

	G_UNLOCK(loaded);
}

/* Load and create instance of requested backend */
GSettingsBackend* _get_backend_by_name(const gchar *inBackendName)
{
	GIOExtensionPoint	*extensionPoint;
	GIOExtension		*backendExtension;
	GType				backendType;
	GObject				*backend;

	/* Ensure all GIO modules are loaded */
	_ensure_loaded();

	/* Get extension point for extension providng GSettingsBackend */
	extensionPoint=g_io_extension_point_lookup(G_SETTINGS_BACKEND_EXTENSION_POINT_NAME);
	if(!extensionPoint)
	{
		g_critical("GIO extension point '%s' not found.", G_SETTINGS_BACKEND_EXTENSION_POINT_NAME);
		return(NULL);
	}

	/* Get extension for requested backend */
	backendExtension=g_io_extension_point_get_extension_by_name(extensionPoint, inBackendName);
	if(!backendExtension)
	{
		g_critical("GIO extension '%s' not found.", inBackendName);
		return(NULL);
	}

	/* Create object for type of requested backend */
	backendType=g_io_extension_get_type(backendExtension);
	if(!g_type_is_a(backendType, G_TYPE_SETTINGS_BACKEND))
	{
		g_critical("GIO extension '%s' has type %s but is derived from %s",
					inBackendName,
					g_type_name(backendType),
					g_type_name(G_TYPE_SETTINGS_BACKEND));
		return(NULL);
	}

	backend=g_object_new(backendType, NULL);
	if(!backend)
	{
		g_critical("Could not create object of type %s from GIO extension '%s'",
					g_type_name(backendType),
					inBackendName);
		return(NULL);
	}

	/* Check for valid GSettingsBackend object */
	if(!G_IS_SETTINGS_BACKEND(backend))
	{
		g_critical("Object from GIO extension '%s' has type %s but expected it to be derived from %s",
					inBackendName,
					g_type_name(backendType),
					g_type_name(G_TYPE_SETTINGS_BACKEND));

		/* Release allocated resources */
		g_object_unref(backend);

		return(NULL);
	}

	/* Return backend found */
	return(G_SETTINGS_BACKEND(backend));
}

/* Migration from one backend to another one */
static gboolean _migrate(GSettingsBackend *inSource,
							GSettingsBackend *inDestination,
							MigrateMode inMode)
{
	GSettingsSchemaSource	*schemaSource;
	gchar					**schemas;
	const gchar				**schemaIter;

	g_return_val_if_fail(G_IS_SETTINGS_BACKEND(inSource), FALSE);
	g_return_val_if_fail(G_IS_SETTINGS_BACKEND(inDestination), FALSE);

	/* Get all installed schemas and iterate through all keys of each schema.
	 * Try to read the value of each key and write it to destination backend
	 * if dry-run was turned off.
	 */
	schemaSource=g_settings_schema_source_ref(g_settings_schema_source_get_default());
	g_settings_schema_source_list_schemas(schemaSource, TRUE, &schemas, NULL);

	for(schemaIter=(const gchar**)schemas; *schemaIter; schemaIter++)
	{
		GSettingsSchema		*schema;
		const gchar			*schemaID;
		gchar				**keys;
		const gchar			**keyIter;
		GSettings			*sourceSettings;
		GSettings			*destinationSettings;

		/* Get ID of schema */
		schemaID=*schemaIter;
		g_print("  Migrating schema %s\n", schemaID);

		/* Get schema */
		schema=g_settings_schema_source_lookup(schemaSource, schemaID, TRUE);
		if(!schema)
		{
			g_critical("Could not load schema %s.", schemaID);

			/* Release allocated resources */
			if(schemas) g_strfreev(schemas);

			/* Return error */
			return(FALSE);
		}

		/* Get settings from source backend */
		sourceSettings=g_settings_new_with_backend(schemaID, inSource);
		if(!sourceSettings)
		{
			g_critical("Could load settings from source backend %s for schema %s.",
						G_OBJECT_TYPE_NAME(inSource),
						schemaID);

			/* Release allocated resources */
			if(schema) g_settings_schema_unref(schema);
			if(schemas) g_strfreev(schemas);

			/* Return error */
			return(FALSE);
		}

		/* Get settings from destination backend */
		destinationSettings=g_settings_new_with_backend(schemaID, inDestination);
		if(!destinationSettings)
		{
			g_critical("Could create settings for destination backend %s with schema %s.",
						G_OBJECT_TYPE_NAME(inDestination),
						schemaID);

			/* Release allocated resources */
			if(sourceSettings) g_object_unref(sourceSettings);
			if(schema) g_settings_schema_unref(schema);
			if(schemas) g_strfreev(schemas);

			/* Return error */
			return(FALSE);
		}

		/* Get all keys from currently iterated schema */
		keys=g_settings_list_keys(sourceSettings);
		if(!keys)
		{
			g_critical("Could get keys from settings of source backend %s for schema %s.",
						G_OBJECT_TYPE_NAME(inSource),
						schemaID);

			/* Release allocated resources */
			if(destinationSettings) g_object_unref(destinationSettings);
			if(sourceSettings) g_object_unref(sourceSettings);
			if(schema) g_settings_schema_unref(schema);
			if(schemas) g_strfreev(schemas);

			/* Return error */
			return(FALSE);
		}

		/* Try to read values of all keys for current schema from source backend
		 * and write it to destination backend if dry-run is turned off.
		 */
		for(keyIter=(const gchar**)keys; *keyIter; keyIter++)
		{
			const gchar		*keyName;
			GVariant		*sourceValue;
			GVariant		*destinationValue;

			/* Get key name */
			keyName=*keyIter;

			/* Get user-modified value for currently iterate key from source backend.
			 * Do not use g_settings_get_value() as it will return the default value
			 * as defined in schema which is not needed to be migrated. Just continue
			 * with next key in schema if there is no user-modified value.
			 */
			sourceValue=g_settings_get_user_value(sourceSettings, keyName);
			if(!sourceValue) continue;

			/* Check if key exists at destination backend and if we can overwrite it */
			destinationValue=g_settings_get_user_value(destinationSettings, keyName);
			if(destinationValue)
			{
				gboolean	canOverwrite;

				/* Before any check we cannot overwrite value at destination */
				canOverwrite=FALSE;

				/* If we do a dry-run and cleaning destination was requested also
				 * then assume that key does not exist.
				 */
				if((inMode & MIGRATE_MODE_DRY_RUN) &&
					(inMode & MIGRATE_MODE_CLEAN_DESTINATION))
				{
					canOverwrite=TRUE;
				}

				/* If overwriting keys at destination was requested then we can overwrite */
				if(inMode & MIGRATE_MODE_OVERWRITE)
				{
					canOverwrite=TRUE;
				}

				/* Show error if we cannot overwrite key at destination backend */
				if(!canOverwrite)
				{
					g_critical("Cannot overwrite key %s for schema %s at destination backend %s.",
								keyName,
								schemaID,
								G_OBJECT_TYPE_NAME(inDestination));

					/* Release allocated resources */
					if(destinationValue) g_variant_unref(destinationValue);
					if(destinationSettings) g_object_unref(destinationSettings);
					if(sourceSettings) g_object_unref(sourceSettings);
					if(schema) g_settings_schema_unref(schema);
					if(schemas) g_strfreev(schemas);

					/* Return error */
					return(FALSE);
				}
			}

			if(destinationValue) g_variant_unref(destinationValue);

			/* Check if key at destination backend is writable at all */
			if(!g_settings_is_writable(destinationSettings, keyName))
			{
				g_critical("Cannot migrate key %s for schema %s at destination backend %s because it is not writable.",
							keyName,
							schemaID,
							G_OBJECT_TYPE_NAME(inDestination));

				/* Release allocated resources */
				if(destinationSettings) g_object_unref(destinationSettings);
				if(sourceSettings) g_object_unref(sourceSettings);
				if(schema) g_settings_schema_unref(schema);
				if(schemas) g_strfreev(schemas);

				/* Return error */
				return(FALSE);
			}

			/* If we do not perform a dry-run then write value at destination backend */
			if(!(inMode & MIGRATE_MODE_DRY_RUN))
			{
				if(!g_settings_set_value(destinationSettings, keyName, sourceValue))
				{
					g_critical("Migrating key %s of schema %s to destination backend %s failed.",
								keyName,
								schemaID,
								G_OBJECT_TYPE_NAME(inDestination));

					/* Release allocated resources */
					if(sourceValue) g_variant_unref(sourceValue);
					if(destinationSettings) g_object_unref(destinationSettings);
					if(sourceSettings) g_object_unref(sourceSettings);
					if(schema) g_settings_schema_unref(schema);
					if(schemas) g_strfreev(schemas);

					/* Return error */
					return(FALSE);
				}

				g_print("    Migrated key %s of schema %s\n",
						keyName,
						schemaID);
			}
				else
				{
					g_print("    Would migrate key %s of schema %s\n",
							keyName,
							schemaID);
				}

			/* Release value */
			if(sourceValue) g_variant_unref(sourceValue);
		}

		/* Release allocated resources */
		if(keys) g_strfreev(keys);
		if(destinationSettings) g_object_unref(destinationSettings);
		if(sourceSettings) g_object_unref(sourceSettings);
		if(schema) g_settings_schema_unref(schema);

		g_print("  Migrated schema %s\n\n", schemaID);
	}

	/* Release allocated resources */
	if(schemas) g_strfreev(schemas);

	/* If we get here, everything went well */
	return(TRUE);
}

/* Main entry point */
int main(int argc, char **argv)
{
	const gchar			*fromBackendName="dconf";
	GSettingsBackend	*fromBackend=NULL;
	const gchar			*toBackendName="xfconf";
	GSettingsBackend	*toBackend=NULL;
	MigrateMode			mode=(MIGRATE_MODE_CLEAN_DESTINATION | MIGRATE_MODE_OVERWRITE);

#if !GLIB_CHECK_VERSION(2, 36, 0)
	/* Initialize GObject type system */
	g_type_init();
#endif

	/* Get backend to migrate from */
	fromBackend=_get_backend_by_name(fromBackendName);
	if(!fromBackend)
	{
		g_critical("Could not get backend for '%s'", fromBackendName);

		/* Release allocated resources */
		if(fromBackend) g_object_unref(fromBackend);
		if(toBackend) g_object_unref(toBackend);

		/* Return error code */
		return(1);
	}

	/* Get backend to migrate to */
	toBackend=_get_backend_by_name(toBackendName);
	if(!toBackend)
	{
		g_critical("Could not get backend for '%s'", toBackendName);

		/* Release allocated resources */
		if(fromBackend) g_object_unref(fromBackend);
		if(toBackend) g_object_unref(toBackend);

		/* Return error code */
		return(1);
	}

	/* First do a dry run of migration to check if migration could succeed */
	g_print("Migrating from backend '%s' using backend class %s to backend '%s' using backend class %s\n\n",
				fromBackendName,
				G_OBJECT_TYPE_NAME(fromBackend),
				toBackendName,
				G_OBJECT_TYPE_NAME(toBackend));

	g_print("* PERFORMING DRY-RUN MIGRATION\n");
	if(!_migrate(fromBackend, toBackend, mode | MIGRATE_MODE_DRY_RUN))
	{
		g_critical("Dry-run of migration failed!");

		/* Release allocated resources */
		if(fromBackend) g_object_unref(fromBackend);
		if(toBackend) g_object_unref(toBackend);

		/* Return error code */
		return(1);
	}
	g_print("* DRY-RUN MIGRATION WAS SUCCESSFULLY.\n\n");

	if(!(mode & MIGRATE_MODE_DRY_RUN))
	{
		g_print("* STARTING MIGRATION\n");
		if(!_migrate(fromBackend, toBackend, mode & ~MIGRATE_MODE_DRY_RUN))
		{
			g_critical("Dry-run of migration failed!");

			/* Release allocated resources */
			if(fromBackend) g_object_unref(fromBackend);
			if(toBackend) g_object_unref(toBackend);

			/* Return error code */
			return(1);
		}
		g_print("* MIGRATION DONE!\n\n");
	}

	/* Release allocated resources */
	if(fromBackend) g_object_unref(fromBackend);
	if(toBackend) g_object_unref(toBackend);

	/* Return success status code */
	return(0);
}
