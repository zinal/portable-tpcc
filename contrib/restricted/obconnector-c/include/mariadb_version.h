/* Copyright Abandoned 1996, 1999, 2001 MySQL AB
   This file is public domain and comes with NO WARRANTY of any kind */

/* Version numbers for protocol & mysqld */

#ifndef _mariadb_version_h_
#define _mariadb_version_h_

#ifdef _CUSTOMCONFIG_
#include <custom_conf.h>
#else
#define PROTOCOL_VERSION		10
#define MARIADB_CLIENT_VERSION_STR	"10.5.5"
#ifndef MARIADB_BASE_VERSION
#define MARIADB_BASE_VERSION		"obclient-10.5"
#endif
#define MARIADB_VERSION_ID		100505
#define MARIADB_PORT	        	3306
#define MARIADB_UNIX_ADDR               "/tmp/mysql.sock"
#ifndef MYSQL_UNIX_ADDR
#define MYSQL_UNIX_ADDR MARIADB_UNIX_ADDR
#endif
#ifndef MYSQL_PORT
#define MYSQL_PORT MARIADB_PORT
#endif

#define MYSQL_CONFIG_NAME               "my"
#define MYSQL_VERSION_ID                100505
#ifndef MYSQL_SERVER_VERSION
#define MYSQL_SERVER_VERSION            "10.5.5-MariaDB"
#endif

#define MARIADB_PACKAGE_VERSION "2.2.13"
#define MARIADB_PACKAGE_VERSION_ID 20213
#define MARIADB_SYSTEM_TYPE "Unix"
#define MARIADB_MACHINE_TYPE "unknown"
#define MARIADB_PLUGINDIR "plugin"

/* mysqld compile time options */
#ifndef MYSQL_CHARSET
#define MYSQL_CHARSET			""
#endif
#endif

/* Source information */
#define CC_SOURCE_REVISION "vendored"

#ifndef LIBOBCLIENT_VERSION_MAJOR
#define LIBOBCLIENT_VERSION_MAJOR 2
#endif

#ifndef LIBOBCLIENT_VERSION_MINOR
#define LIBOBCLIENT_VERSION_MINOR 2
#endif

#ifndef LIBOBCLIENT_VERSION_PATCH
#define LIBOBCLIENT_VERSION_PATCH 13
#endif

#ifndef LIBOBCLIENT_VERSION
#define LIBOBCLIENT_VERSION "2.2.13"
#endif

#endif /* _mariadb_version_h_ */
