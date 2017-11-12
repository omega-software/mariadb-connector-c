/* Copyright (C) 2000 MySQL AB & MySQL Finland AB & TCX DataKonsult AB
                 2016 MariaDB Corporation AB

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with this library; if not, write to the Free
   Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02111-1301, USA */

#include <ma_global.h>
#include <ma_sys.h>
#include "ma_string.h"
#include <ctype.h>
#include "mariadb_ctype.h"
#include <mysql.h>
#include <ma_common.h>
#include <mariadb/ma_io.h>

#ifdef _WIN32
#include <io.h>
#include "shlwapi.h"

static const char *ini_exts[]= {"ini", "cnf", 0};
#define R_OK 4
#else
#include <unistd.h>
static const char *ini_exts[]= {"cnf", 0};
#endif

char **configuration_dirs= NULL;
#define MAX_CONFIG_DIRS 6

static int add_cfg_dir(char **cfg_dirs, const char *directory)
{
  int i;

  for (i=0; i < MAX_CONFIG_DIRS && cfg_dirs[i]; i++);

  if (i < MAX_CONFIG_DIRS) {
    cfg_dirs[i]= strdup(directory);
    return 0;
  }
  return 1;
}

void release_configuration_dirs()
{
  if (configuration_dirs)
  {
    int i= 0;
    while (configuration_dirs[i])
      free(configuration_dirs[i++]);
    free(configuration_dirs);
  }
}

char **get_default_configuration_dirs()
{
#ifdef _WIN32
  char dirname[FN_REFLEN];
#endif
  char *env;

  configuration_dirs= (char **)calloc(1, (MAX_CONFIG_DIRS + 1) * sizeof(char *));
  if (!configuration_dirs)
    goto end;

#ifdef _WIN32
  /* On Windows operating systems configuration files are stored in
     1. System directory
     2. Windows directory
     3. C:\
  */
  if (!GetSystemDirectory(dirname, FN_REFLEN) ||
      add_cfg_dir(configuration_dirs, dirname))
    goto error;

  if (!GetWindowsDirectory(dirname, FN_REFLEN) ||
      add_cfg_dir(configuration_dirs, dirname))
    goto error;

  if (add_cfg_dir(configuration_dirs, "C:"))
    goto error;

  if (GetModuleFileName(NULL, dirname, FN_REFLEN))
  {
    PathRemoveFileSpec(dirname);
    if (add_cfg_dir(configuration_dirs, dirname))
      goto error;
  }
#else
  /* on *nix platforms configuration files are stored in
     1. SYSCONFDIR (if build happens inside server package, or
        -DDEFAULT_SYSCONFDIR was specified
     2. /etc
     3. /etc/mysql
  */
#ifdef DEFAULT_SYSCONFDIR
  if (add_cfg_dir(configuration_dirs, DEFAULT_SYSCONFDIR))
    goto error;
#else
  if (add_cfg_dir(configuration_dirs, "/etc"))
    goto error;
  if (add_cfg_dir(configuration_dirs, "/etc/mysql"))
    goto error;
#endif
#endif
/* This differs from https://mariadb.com/kb/en/mariadb/configuring-mariadb-with-mycnf/ where MYSQL_HOME is not specified for Windows */
  if ((env= getenv("MYSQL_HOME")) &&
      add_cfg_dir(configuration_dirs, env))
    goto error;
end:
  return configuration_dirs;
error:
  return NULL;
}

extern my_bool _mariadb_set_conf_option(MYSQL *mysql, const char *config_option, const char *config_value);

static my_bool is_group(char *ptr, const char **groups)
{
  while (*groups)
  {
    if (!strcmp(ptr, *groups))
      return 1;
    groups++;
  }
  return 0;
}

static my_bool _mariadb_read_options_from_file(MYSQL *mysql,
                                               const char *config_file,
                                               const char *group)
{
  uint line=0;
  my_bool read_values= 0, found_group= 0, is_escaped= 0, is_quoted= 0;
  char buff[4096],*ptr,*end,*value, *key= 0, *optval;
  MA_FILE *file= NULL;
  my_bool rc= 1;
  const char *groups[5]= {"client",
                          "client-server",
                          "client-mariadb",
                          group,
                          NULL};
  my_bool (*set_option)(MYSQL *mysql, const char *config_option, const char *config_value);


  /* if a plugin registered a hook we will call this hook, otherwise
   * default (_mariadb_set_conf_option) will be called */
  if (mysql->options.extension && mysql->options.extension->set_option)
    set_option= mysql->options.extension->set_option;
  else
    set_option= _mariadb_set_conf_option;

  if (!(file = ma_open(config_file, "r", NULL)))
    goto err;

  while (ma_gets(buff,sizeof(buff)-1,file))
  {
    line++;
    key= 0;
    /* Ignore comment and empty lines */
    for (ptr=buff ; isspace(*ptr) ; ptr++ );
    if (!is_escaped && (*ptr == '\"' || *ptr== '\''))
    {
      is_quoted= !is_quoted;
      continue;
    }
    if (*ptr == '#' || *ptr == ';' || !*ptr)
      continue;
    is_escaped= (*ptr == '\\');
    if (*ptr == '[')				/* Group name */
    {
      found_group=1;
      if (!(end=(char *) strchr(++ptr,']')))
      {
        /* todo: set error */
        goto err;
      }
      for ( ; isspace(end[-1]) ; end--) ;	/* Remove end space */
      end[0]=0;
      read_values= is_group(ptr, groups);
      continue;
    }
    if (!found_group)
    {
      /* todo: set error */
      goto err;
    }
    if (!read_values)
      continue;
    if (!(end=value=strchr(ptr,'=')))
    {
      end=strchr(ptr, '\0');				/* Option without argument */
      set_option(mysql, ptr, NULL);
    }
    if (!key)
      key= ptr;
    for ( ; isspace(end[-1]) ; end--) ;

    if (!value)
    {
      if (!key)
        key= ptr;
    }
    else
    {
      /* Remove pre- and end space */
      char *value_end;
      *value= 0;
      value++;
      ptr= value;
      for ( ; isspace(*value); value++) ;
      optval= value;
      value_end=strchr(value, '\0');
      for ( ; isspace(value_end[-1]) ; value_end--) ;
      /* remove possible quotes */
      if (*value == '\'' || *value == '\"')
      {
        value++;
        if (value_end[-1] == '\'' || value_end[-1] == '\"')
          value_end--;
      }
      if (value_end < value)			/* Empty string */
        value_end=value;
      for ( ; value != value_end; value++)
      {
        if (*value == '\\' && value != value_end-1)
        {
          switch(*++value) {
            case 'n':
              *ptr++='\n';
              break;
            case 't':
              *ptr++= '\t';
              break;
            case 'r':
              *ptr++ = '\r';
              break;
            case 'b':
              *ptr++ = '\b';
              break;
            case 's':
              *ptr++= ' ';			/* space */
              break;
            case '\"':
              *ptr++= '\"';
              break;
            case '\'':
              *ptr++= '\'';
              break;
            case '\\':
              *ptr++= '\\';
              break;
            default:				/* Unknown; Keep '\' */
              *ptr++= '\\';
              *ptr++= *value;
              break;
          }
        }
        else
          *ptr++= *value;
      }
      *ptr=0;
      set_option(mysql, key, optval);
      key= optval= 0;
    }
  }
  if (file)
    ma_close(file);
  rc= 0;

err:
  return rc;
}


my_bool _mariadb_read_options(MYSQL *mysql,
                              const char *config_file,
                              const char *group)
{
  int i= 0,
      exts,
      errors= 0;
  char filename[FN_REFLEN + 1];
#ifndef _WIN32
  char *env;
#endif

  if (config_file)
    return _mariadb_read_options_from_file(mysql, config_file, group);

  for (i=0; i < MAX_CONFIG_DIRS && configuration_dirs[i]; i++)
  {
    for (exts= 0; ini_exts[exts]; exts++)
    {
      snprintf(filename, FN_REFLEN,
               "%s%cmy.%s", configuration_dirs[i], FN_LIBCHAR, ini_exts[exts]);
      if (!access(filename, R_OK))
        errors+= _mariadb_read_options_from_file(mysql, filename, group);
    }
  }
#ifndef _WIN32
  /* special case: .my.cnf in Home directory */
  if ((env= getenv("HOME")))
  {
    for (exts= 0; ini_exts[exts]; exts++)
    {
      snprintf(filename, FN_REFLEN,
               "%s%c.my.%s", env, FN_LIBCHAR, ini_exts[exts]);
      if (!access(filename, R_OK))
        errors+= _mariadb_read_options_from_file(mysql, filename, group);
    }
  }
#endif
  return errors;
}
