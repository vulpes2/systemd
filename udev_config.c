/*
 * udev_config.c
 *
 * Userspace devfs
 *
 * Copyright (C) 2003,2004 Greg Kroah-Hartman <greg@kroah.com>
 *
 *
 *	This program is free software; you can redistribute it and/or modify it
 *	under the terms of the GNU General Public License as published by the
 *	Free Software Foundation version 2 of the License.
 * 
 *	This program is distributed in the hope that it will be useful, but
 *	WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *	General Public License for more details.
 * 
 *	You should have received a copy of the GNU General Public License along
 *	with this program; if not, write to the Free Software Foundation, Inc.,
 *	675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

/* define this to enable parsing debugging */
/* #define DEBUG_PARSER */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>

#include "libsysfs/sysfs/libsysfs.h"
#include "udev.h"
#include "udev_lib.h"
#include "udev_version.h"
#include "logging.h"
#include "namedev.h"

/* global variables */
char sysfs_path[SYSFS_PATH_MAX];
char udev_root[PATH_MAX];
char udev_db_filename[PATH_MAX+NAME_MAX];
char udev_permissions_filename[PATH_MAX+NAME_MAX];
char udev_rules_filename[PATH_MAX+NAME_MAX];
char udev_config_filename[PATH_MAX+NAME_MAX];
char default_mode_str[MODE_SIZE];
char default_owner_str[OWNER_SIZE];
char default_group_str[GROUP_SIZE];
int udev_log;
int udev_sleep;


static int string_is_true(char *str)
{
	if (strcasecmp(str, "true") == 0)
		return 1;
	if (strcasecmp(str, "yes") == 0)
		return 1;
	return 0;
}

static void init_variables(void)
{
	/* fill up the defaults.  
	 * If any config values are specified, they will
	 * override these values. */
	strfieldcpy(udev_root, UDEV_ROOT);
	strfieldcpy(udev_db_filename, UDEV_DB);
	strfieldcpy(udev_config_filename, UDEV_CONFIG_FILE);
	strfieldcpy(udev_rules_filename, UDEV_RULES_FILE);
	strfieldcpy(udev_permissions_filename, UDEV_PERMISSION_FILE);
	udev_log = string_is_true(UDEV_LOG_DEFAULT);

	udev_sleep = 1;
	if (getenv("UDEV_NO_SLEEP") != NULL)
		udev_sleep = 0;
}

#define set_var(_name, _var)				\
	if (strcasecmp(variable, _name) == 0) {		\
		dbg_parse("%s = '%s'", _name, value);	\
		strfieldcpymax(_var, value, sizeof(_var));\
	}

#define set_bool(_name, _var)				\
	if (strcasecmp(variable, _name) == 0) {		\
		dbg_parse("%s = '%s'", _name, value);	\
		_var = string_is_true(value);		\
	}

int parse_get_pair(char **orig_string, char **left, char **right)
{
	char *temp;
	char *string = *orig_string;

	if (!string)
		return -ENODEV;

	/* eat any whitespace */
	while (isspace(*string) || *string == ',')
		++string;

	/* split based on '=' */
	temp = strsep(&string, "=");
	*left = temp;
	if (!string)
		return -ENODEV;

	/* take the right side and strip off the '"' */
	while (isspace(*string))
		++string;
	if (*string == '"')
		++string;
	else
		return -ENODEV;

	temp = strsep(&string, "\"");
	if (!string || *temp == '\0')
		return -ENODEV;
	*right = temp;
	*orig_string = string;
	
	return 0;
}

static int parse_config_file(void)
{
	char line[255];
	char *temp;
	char *variable;
	char *value;
	char *buf;
	size_t bufsize;
	size_t cur;
	size_t count;
	int lineno;
	int retval = 0;

	if (file_map(udev_config_filename, &buf, &bufsize) == 0) {
		dbg("reading '%s' as config file", udev_config_filename);
	} else {
		dbg("can't open '%s' as config file", udev_config_filename);
		return -ENODEV;
	}

	/* loop through the whole file */
	lineno = 0;
	cur = 0;
	while (1) {
		count = buf_get_line(buf, bufsize, cur);

		strncpy(line, buf + cur, count);
		line[count] = '\0';
		temp = line;
		lineno++;

		cur += count+1;
		if (cur > bufsize)
			break;

		dbg_parse("read '%s'", temp);

		/* eat the whitespace at the beginning of the line */
		while (isspace(*temp))
			++temp;

		/* empty line? */
		if (*temp == 0x00)
			continue;

		/* see if this is a comment */
		if (*temp == COMMENT_CHARACTER)
			continue;

		retval = parse_get_pair(&temp, &variable, &value);
		if (retval)
			break;
		
		dbg_parse("variable = '%s', value = '%s'", variable, value);

		set_var("udev_root", udev_root);
		set_var("udev_db", udev_db_filename);
		set_var("udev_rules", udev_rules_filename);
		set_var("udev_permissions", udev_permissions_filename);
		set_var("default_mode", default_mode_str);
		set_var("default_owner", default_owner_str);
		set_var("default_group", default_group_str);
		set_bool("udev_log", udev_log);
	}
	dbg_parse("%s:%d:%Zd: error parsing '%s'", udev_config_filename,
		  lineno, temp - line, temp);

	file_unmap(buf, bufsize);
	return retval;
}

static void get_dirs(void)
{
	char *temp;
	int retval;

	retval = sysfs_get_mnt_path(sysfs_path, SYSFS_PATH_MAX);
	if (retval)
		dbg("sysfs_get_mnt_path failed");

	/* see if we should try to override any of the default values */
	temp = getenv("UDEV_TEST");
	if (temp != NULL) {
		/* hm testing is happening, use the specified values, if they are present */
		temp = getenv("SYSFS_PATH");
		if (temp)
			strfieldcpy(sysfs_path, temp);
		temp = getenv("UDEV_CONFIG_FILE");
		if (temp)
			strfieldcpy(udev_config_filename, temp);
	}
	dbg("sysfs_path='%s'", sysfs_path);

	dbg_parse("udev_root = %s", udev_root);
	dbg_parse("udev_config_filename = %s", udev_config_filename);
	dbg_parse("udev_db_filename = %s", udev_db_filename);
	dbg_parse("udev_rules_filename = %s", udev_rules_filename);
	dbg_parse("udev_permissions_filename = %s", udev_permissions_filename);
	dbg_parse("udev_log = %d", udev_log);
	parse_config_file();

	dbg_parse("udev_root = %s", udev_root);
	dbg_parse("udev_config_filename = %s", udev_config_filename);
	dbg_parse("udev_db_filename = %s", udev_db_filename);
	dbg_parse("udev_rules_filename = %s", udev_rules_filename);
	dbg_parse("udev_permissions_filename = %s", udev_permissions_filename);
	dbg_parse("udev_log_str = %d", udev_log);
}

void udev_init_config(void)
{
	init_variables();
	get_dirs();
}


