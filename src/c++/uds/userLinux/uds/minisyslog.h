/*
 * %COPYRIGHT%
 *
 * %LICENSE%
 */

#ifndef MINISYSLOG_H
#define MINISYSLOG_H 1

#include <linux/compiler_attributes.h>

#include <syslog.h>
#include <stdarg.h>

/**
 * @file
 *
 * Replacements for the syslog library functions so that the library
 * calls do not conflict with the application calling syslog.
 **/

/**
 * Open the logger. The function mimics the openlog() c-library function.
 *
 * @param ident    The identity string to prepended to all log messages
 * @param option   The logger options (see the openlog(3) man page).
 * @param facility The type of program logging the message.
 **/
void mini_openlog(const char *ident, int option, int facility);

/**
 * Log a message. This function mimics the syslog() c-library function.
 *
 * @param priority The priority level of the message
 * @param format   A printf style message format
 **/
void mini_syslog(int priority, const char *format, ...)
	__printf(2, 3);

/**
 * Log a message. This function mimics the vsyslog() c-library function.
 *
 * @param priority The priority level of the message
 * @param format   A printf style message format
 * @param ap       An argument list obtained from stdarg()
 **/
void mini_vsyslog(int priority, const char *format, va_list ap)
	__printf(2, 0);

/**
 * Log a message pack consisting of multiple variable sections.
 *
 * @param priority      the priority at which to log the message
 * @param prefix        optional string prefix to message, may be NULL
 * @param fmt1          format of message first part, may be NULL
 * @param args1         arguments for message first part
 * @param fmt2          format of message second part, may be NULL
 * @param args2         arguments for message second part
 **/
void mini_syslog_pack(int priority,
		      const char *prefix,
		      const char *fmt1,
		      va_list args1,
		      const char *fmt2,
		      va_list args2)
	__printf(3, 0) __printf(5, 0);

/**
 * Close a logger. This function mimics the closelog() c-library function.
 **/
void mini_closelog(void);

#endif /* MINI_SYSLOG_H */
