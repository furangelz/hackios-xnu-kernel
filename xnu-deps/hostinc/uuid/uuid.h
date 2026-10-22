/*
 * Stand-in for libuuid's <uuid/uuid.h> on hosts without its development package
 * (libuuid-devel): the host tools only need the type; uuid_generate is hostcompat.h's own.
 */
#ifndef _UUID_UUID_H
#define _UUID_UUID_H
typedef unsigned char uuid_t[16];
#endif
