/* SPDX-License-Identifier: MIT
 *
 * amdgpu-wddm: driver configuration files that an administrator adds at run time, on builds without expat (Windows).
 *
 * A Windows build of Mesa has no XML configuration at run time: the build turns the .conf files into a static table
 * (driconf_static.py). This module adds the files of one machine-wide directory to that table, in the format of
 * /usr/share/drirc.d, so that a section of an upstream .conf file can be added as it is:
 *
 *   <profile root>\drirc.d\*.conf
 *
 * The profile root is <system drive>\ProgramData\amdgpu-wddm\profiles. The installer makes it and gives write access
 * to SYSTEM and Administrators only (docs/design/app-profiles.md in amdgpu-wddm). A lab build also takes the root
 * from AMDGPU_WDDM_PROFILE_DIR; a release build reads no path from the environment (BD-167).
 *
 * u_driconf_xml_parse reads the XML subset of the drirc files: elements with attributes, comments, an XML
 * declaration in UTF-8 and a document type declaration without entities. It reads the whole text before it gives
 * an event, so a file with an error gives no event at all. xmlconfig.c then checks the driconf rules on the events
 * and gives them to the same element handlers as the expat parser.
 */

#ifndef U_DRICONF_FILE_H
#define U_DRICONF_FILE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The largest file that is read, and the deepest element nesting. */
#define U_DRICONF_FILE_MAX_SIZE (1024 * 1024)
#define U_DRICONF_XML_MAX_DEPTH 16
#define U_DRICONF_XML_MAX_ATTRS 32

/* One start or end tag. A start tag has attr: name and value pairs, then NULL (no attributes: attr[0] is NULL). An
 * end tag has attr NULL. An empty-element tag (<option ... />) gives a start and an end event. */
struct u_driconf_xml_event {
   const char *name;
   const char **attr;
   unsigned line;
};

struct u_driconf_xml {
   struct u_driconf_xml_event *events;
   unsigned count;
   char *strings;
   const char **attrs;
};

/* Parses text (size bytes, no terminating zero needed). On success, xml holds the events and true is returned. On
 * an error, xml holds no events, error gets "line <n>: <reason>" and false is returned. u_driconf_xml_fini frees
 * xml in both cases. */
bool u_driconf_xml_parse(const char *text, size_t size, struct u_driconf_xml *xml, char *error, size_t error_size);
void u_driconf_xml_fini(struct u_driconf_xml *xml);

/* One line of the profile reader, through amdgpu_wddm_log in a driver DLL (src/util/amdgpu_wddm_stdio.h) and to
 * stderr elsewhere. The line gets the prefix "amdgpu-wddm drirc: " and a newline. */
void u_driconf_log(const char *format, ...);

#ifdef _WIN32
/* Called once for each file that passed the checks, in the order of the names (bytes of the UTF-8 name compared
 * as unsigned, the order of alphasort in the C locale). path is UTF-8; text is the whole file, size bytes, followed
 * by a zero. */
typedef void (*u_driconf_profile_file_cb)(void *data, const char *path, const char *text, size_t size);

/* Every *.conf file of <profile root>\drirc.d, each read whole. A directory or a file that another account than
 * SYSTEM, Administrators or TrustedInstaller owns or may change, a reparse point, a file larger than
 * U_DRICONF_FILE_MAX_SIZE and a name that is not valid UTF-16 are refused, each with one log line. No directory: no
 * call, no log line. */
void u_driconf_profile_files(u_driconf_profile_file_cb cb, void *data);

#ifdef U_DRICONF_FILE_TEST
/* Host tests only: the root to read (UTF-16), and whether the owner and access checks are skipped. */
extern const wchar_t *u_driconf_test_root;
extern bool u_driconf_test_trust_all;
#endif

/* The access rule of the profile files, on a security descriptor. Exposed for the host tests. why gets the reason
 * of a refusal. */
bool u_driconf_trusted_descriptor(void *security_descriptor, char *why, size_t why_size);
#endif

#ifdef __cplusplus
}
#endif

#endif /* U_DRICONF_FILE_H */
