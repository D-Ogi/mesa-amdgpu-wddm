/* SPDX-License-Identifier: MIT
 *
 * amdgpu-wddm: the reader of the run-time driver configuration files (u_driconf_file.h).
 */

#include "u_driconf_file.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <wchar.h>
#pragma comment(lib, "advapi32.lib")
#endif

void
u_driconf_log(const char *format, ...)
{
   char line[1024];
   va_list args;
   va_start(args, format);
   vsnprintf(line, sizeof(line), format, args);
   va_end(args);
#ifdef AMDGPU_WDDM_STDIO_H
   amdgpu_wddm_log("amdgpu-wddm drirc: %s\n", line);
#else
   fprintf(stderr, "amdgpu-wddm drirc: %s\n", line);
#endif
}

/* ---- the XML subset ------------------------------------------------------------------------------------------- */

/* One event while the text is read: offsets into the string store, because the stores can move. */
struct raw_event {
   size_t name;
   size_t attr;      /* index of the first attribute offset; SIZE_MAX for an end tag */
   unsigned line;
};

struct xml_parser {
   const char *p, *end;
   unsigned line;
   bool failed;
   char *error;
   size_t error_size;

   char *strings;
   size_t strings_used, strings_size;
   size_t *attrs;    /* string offsets; SIZE_MAX ends the attributes of one start tag */
   size_t attr_count, attr_size;
   struct raw_event *events;
   size_t event_count, event_size;

   size_t stack[U_DRICONF_XML_MAX_DEPTH];
   unsigned depth;
};

static bool
fail(struct xml_parser *x, const char *reason)
{
   if (!x->failed) {
      x->failed = true;
      snprintf(x->error, x->error_size, "line %u: %s", x->line, reason);
   }
   return false;
}

static bool
is_space(char c)
{
   return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool
name_start(char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == ':';
}

static bool
name_char(char c)
{
   return name_start(c) || (c >= '0' && c <= '9') || c == '.' || c == '-';
}

/* Moves n bytes on, counting lines. */
static void
advance(struct xml_parser *x, size_t n)
{
   for (; n && x->p < x->end; n--, x->p++) {
      if (*x->p == '\n')
         x->line++;
   }
}

/* ASCII case-insensitive comparison of n bytes. */
static bool
same_ascii(const char *a, const char *b, size_t n)
{
   for (size_t i = 0; i < n; i++) {
      char x = a[i] >= 'A' && a[i] <= 'Z' ? a[i] + 32 : a[i];
      char y = b[i] >= 'A' && b[i] <= 'Z' ? b[i] + 32 : b[i];
      if (x != y)
         return false;
   }
   return true;
}

static bool
starts(const struct xml_parser *x, const char *s)
{
   size_t n = strlen(s);
   return (size_t)(x->end - x->p) >= n && !memcmp(x->p, s, n);
}

static void
skip_space(struct xml_parser *x)
{
   while (x->p < x->end && is_space(*x->p))
      advance(x, 1);
}

/* The whole text: UTF-8 without overlong forms or surrogates, and only the characters of the XML Char rule. */
static bool
check_text(struct xml_parser *x)
{
   const unsigned char *s = (const unsigned char *)x->p, *end = (const unsigned char *)x->end;
   unsigned line = 1;
   while (s < end) {
      unsigned c = *s, n, cp;
      if (c == '\n')
         line++;
      if (c < 0x80) {
         if (c < 0x20 && c != '\t' && c != '\n' && c != '\r')
            break;
         s++;
         continue;
      }
      if (c >= 0xc2 && c <= 0xdf) {
         n = 1;
         cp = c & 0x1f;
      } else if (c >= 0xe0 && c <= 0xef) {
         n = 2;
         cp = c & 0x0f;
      } else if (c >= 0xf0 && c <= 0xf4) {
         n = 3;
         cp = c & 0x07;
      } else {
         break;
      }
      if ((size_t)(end - s) <= n)
         break;
      bool ok = true;
      for (unsigned i = 1; i <= n; i++) {
         if ((s[i] & 0xc0) != 0x80)
            ok = false;
         cp = (cp << 6) | (s[i] & 0x3f);
      }
      if (!ok || (n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10ffff)) ||
          (cp >= 0xd800 && cp <= 0xdfff) || cp == 0xfffe || cp == 0xffff)
         break;
      s += n + 1;
   }
   if (s == end)
      return true;
   x->line = line;
   return fail(x, "the file is not UTF-8 text, or it has a character that XML does not allow");
}

/* Room for one more string of at most n bytes and its zero. */
static char *
string_room(struct xml_parser *x, size_t n)
{
   if (x->strings_size - x->strings_used < n + 1) {
      fail(x, "out of memory");
      return NULL;
   }
   return x->strings + x->strings_used;
}

static bool
push_attr(struct xml_parser *x, size_t offset)
{
   if (x->attr_count == x->attr_size) {
      size_t size = x->attr_size ? x->attr_size * 2 : 64;
      size_t *attrs = realloc(x->attrs, size * sizeof(*attrs));
      if (!attrs)
         return fail(x, "out of memory");
      x->attrs = attrs;
      x->attr_size = size;
   }
   x->attrs[x->attr_count++] = offset;
   return true;
}

static bool
push_event(struct xml_parser *x, size_t name, size_t attr, unsigned line)
{
   if (x->event_count == x->event_size) {
      size_t size = x->event_size ? x->event_size * 2 : 64;
      if (size > 1u << 20)
         return fail(x, "too many elements");
      struct raw_event *events = realloc(x->events, size * sizeof(*events));
      if (!events)
         return fail(x, "out of memory");
      x->events = events;
      x->event_size = size;
   }
   x->events[x->event_count++] = (struct raw_event){name, attr, line};
   return true;
}

/* A name, copied into the string store. Returns its offset, or SIZE_MAX. */
static size_t
parse_name(struct xml_parser *x, const char *what)
{
   const char *start = x->p;
   if (x->p >= x->end || !name_start(*x->p)) {
      char reason[96];
      snprintf(reason, sizeof(reason), "%s: a name was expected", what);
      fail(x, reason);
      return SIZE_MAX;
   }
   while (x->p < x->end && name_char(*x->p))
      x->p++;
   size_t n = (size_t)(x->p - start);
   char *out = string_room(x, n);
   if (!out)
      return SIZE_MAX;
   memcpy(out, start, n);
   out[n] = 0;
   size_t offset = x->strings_used;
   x->strings_used += n + 1;
   return offset;
}

static void
put_utf8(char **out, unsigned cp)
{
   char *o = *out;
   if (cp < 0x80) {
      *o++ = (char)cp;
   } else if (cp < 0x800) {
      *o++ = (char)(0xc0 | (cp >> 6));
      *o++ = (char)(0x80 | (cp & 0x3f));
   } else if (cp < 0x10000) {
      *o++ = (char)(0xe0 | (cp >> 12));
      *o++ = (char)(0x80 | ((cp >> 6) & 0x3f));
      *o++ = (char)(0x80 | (cp & 0x3f));
   } else {
      *o++ = (char)(0xf0 | (cp >> 18));
      *o++ = (char)(0x80 | ((cp >> 12) & 0x3f));
      *o++ = (char)(0x80 | ((cp >> 6) & 0x3f));
      *o++ = (char)(0x80 | (cp & 0x3f));
   }
   *out = o;
}

/* An entity or character reference at x->p ('&'), decoded to *out. The encoding of a reference is never longer than
 * the reference itself ("&#x10FFFF;" is 10 bytes, its UTF-8 4). */
static bool
parse_reference(struct xml_parser *x, char **out)
{
   static const struct { const char *name; char c; } entities[] = {
      {"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''},
   };
   for (unsigned i = 0; i < sizeof(entities) / sizeof(entities[0]); i++) {
      if (starts(x, entities[i].name)) {
         *(*out)++ = entities[i].c;
         advance(x, strlen(entities[i].name));
         return true;
      }
   }
   if (!starts(x, "&#"))
      return fail(x, "a reference to an entity that is not one of the five XML entities");
   const char *s = x->p + 2;
   unsigned base = 10, digits = 0;
   uint32_t cp = 0;
   if (s < x->end && *s == 'x') {
      base = 16;
      s++;
   }
   for (; s < x->end && *s != ';'; s++) {
      unsigned d;
      if (*s >= '0' && *s <= '9')
         d = *s - '0';
      else if (base == 16 && *s >= 'a' && *s <= 'f')
         d = *s - 'a' + 10;
      else if (base == 16 && *s >= 'A' && *s <= 'F')
         d = *s - 'A' + 10;
      else
         return fail(x, "a character reference that is not a number");
      if (++digits > 8)
         return fail(x, "a character reference that is too long");
      cp = cp * base + d;
   }
   if (s >= x->end || !digits)
      return fail(x, "a character reference without its ';'");
   if (!(cp == 0x9 || cp == 0xa || cp == 0xd || (cp >= 0x20 && cp <= 0xd7ff) || (cp >= 0xe000 && cp <= 0xfffd) ||
         (cp >= 0x10000 && cp <= 0x10ffff)))
      return fail(x, "a character reference to a character that XML does not allow");
   put_utf8(out, cp);
   advance(x, (size_t)(s + 1 - x->p));
   return true;
}

/* A quoted attribute value, decoded and normalized as XML does (tab, line feed and carriage return become a space,
 * a carriage return and line feed one space). Returns the offset, or SIZE_MAX. */
static size_t
parse_value(struct xml_parser *x)
{
   if (x->p >= x->end || (*x->p != '"' && *x->p != '\'')) {
      fail(x, "an attribute value must be in quotes");
      return SIZE_MAX;
   }
   char quote = *x->p;
   advance(x, 1);
   const char *close = memchr(x->p, quote, (size_t)(x->end - x->p));
   if (!close) {
      fail(x, "an attribute value without its closing quote");
      return SIZE_MAX;
   }
   char *start = string_room(x, (size_t)(close - x->p));
   if (!start)
      return SIZE_MAX;
   char *out = start;
   while (x->p < close) {
      char c = *x->p;
      if (c == '<') {
         fail(x, "'<' in an attribute value");
         return SIZE_MAX;
      }
      if (c == '&') {
         if (!parse_reference(x, &out))
            return SIZE_MAX;
         if (x->p > close) {
            fail(x, "a reference across the end of an attribute value");
            return SIZE_MAX;
         }
         continue;
      }
      if (c == '\r' && x->p + 1 < close && x->p[1] == '\n')
         advance(x, 1);
      *out++ = (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
      advance(x, 1);
   }
   advance(x, 1);
   *out = 0;
   size_t offset = x->strings_used;
   x->strings_used += (size_t)(out - start) + 1;
   return offset;
}

static bool
parse_comment(struct xml_parser *x)
{
   advance(x, 4);   /* "<!--" */
   while (x->p < x->end) {
      if (starts(x, "--")) {
         if (!starts(x, "-->"))
            return fail(x, "\"--\" inside a comment");
         advance(x, 3);
         return true;
      }
      advance(x, 1);
   }
   return fail(x, "a comment without its \"-->\"");
}

/* The XML declaration at the start: version, encoding (UTF-8 or US-ASCII) and standalone. */
static bool
parse_declaration(struct xml_parser *x)
{
   advance(x, 5);   /* "<?xml" */
   for (;;) {
      bool space = x->p < x->end && is_space(*x->p);
      skip_space(x);
      if (starts(x, "?>")) {
         advance(x, 2);
         return true;
      }
      if (!space)
         return fail(x, "the XML declaration is not valid");
      const char *name = x->p;
      while (x->p < x->end && name_char(*x->p))
         x->p++;
      size_t n = (size_t)(x->p - name);
      skip_space(x);
      if (x->p >= x->end || *x->p != '=')
         return fail(x, "the XML declaration is not valid");
      advance(x, 1);
      skip_space(x);
      if (x->p >= x->end || (*x->p != '"' && *x->p != '\''))
         return fail(x, "the XML declaration is not valid");
      char quote = *x->p;
      const char *value = x->p + 1;
      const char *close = memchr(value, quote, (size_t)(x->end - value));
      if (!close)
         return fail(x, "the XML declaration is not valid");
      size_t v = (size_t)(close - value);
      if (n == 8 && !memcmp(name, "encoding", 8)) {
         if (!((v == 5 && same_ascii(value, "UTF-8", 5)) || (v == 8 && same_ascii(value, "US-ASCII", 8))))
            return fail(x, "only the UTF-8 encoding is read");
      } else if (!((n == 7 && !memcmp(name, "version", 7)) || (n == 10 && !memcmp(name, "standalone", 10)))) {
         return fail(x, "the XML declaration is not valid");
      }
      advance(x, (size_t)(close + 1 - x->p));
   }
}

/* One declaration of the internal subset or of the document type: up to its '>', with quoted strings. */
static bool
skip_declaration(struct xml_parser *x)
{
   while (x->p < x->end && *x->p != '>') {
      if (*x->p == '"' || *x->p == '\'') {
         const char *close = memchr(x->p + 1, *x->p, (size_t)(x->end - x->p - 1));
         if (!close)
            return fail(x, "a declaration without its closing quote");
         advance(x, (size_t)(close + 1 - x->p));
         continue;
      }
      if (*x->p == '%' || *x->p == '<' || *x->p == '[')
         return fail(x, "the document type declaration is not read: it is not the simple form");
      advance(x, 1);
   }
   if (x->p >= x->end)
      return fail(x, "a declaration without its '>'");
   advance(x, 1);
   return true;
}

/* <!DOCTYPE name ...> with an optional internal subset of element and attribute list declarations and comments.
 * Entities are refused: their expansion would change the text that the handlers see. */
static bool
parse_doctype(struct xml_parser *x)
{
   advance(x, 9);   /* "<!DOCTYPE" */
   if (x->p >= x->end || !is_space(*x->p))
      return fail(x, "the document type declaration is not valid");
   while (x->p < x->end && *x->p != '[' && *x->p != '>') {
      if (*x->p == '"' || *x->p == '\'') {
         const char *close = memchr(x->p + 1, *x->p, (size_t)(x->end - x->p - 1));
         if (!close)
            return fail(x, "the document type declaration is not valid");
         advance(x, (size_t)(close + 1 - x->p));
         continue;
      }
      advance(x, 1);
   }
   if (x->p < x->end && *x->p == '[') {
      advance(x, 1);
      for (;;) {
         skip_space(x);
         if (x->p >= x->end)
            return fail(x, "the document type declaration is not complete");
         if (*x->p == ']') {
            advance(x, 1);
            break;
         }
         if (starts(x, "<!--")) {
            if (!parse_comment(x))
               return false;
         } else if (starts(x, "<!ELEMENT") || starts(x, "<!ATTLIST")) {
            advance(x, 9);
            if (!skip_declaration(x))
               return false;
         } else {
            return fail(x, "the document type declaration has an entity or another declaration that is not read");
         }
      }
      skip_space(x);
   }
   if (x->p >= x->end || *x->p != '>')
      return fail(x, "the document type declaration is not complete");
   advance(x, 1);
   return true;
}

/* A start tag at x->p ('<' and a name start), with its end tag event if it is an empty-element tag. */
static bool
parse_start_tag(struct xml_parser *x)
{
   unsigned line = x->line;
   advance(x, 1);
   size_t name = parse_name(x, "a tag");
   if (name == SIZE_MAX)
      return false;
   size_t first = x->attr_count;
   unsigned count = 0;
   for (;;) {
      bool space = x->p < x->end && is_space(*x->p);
      skip_space(x);
      if (x->p >= x->end)
         return fail(x, "a tag without its '>'");
      if (*x->p == '>' || starts(x, "/>"))
         break;
      if (!space)
         return fail(x, "attributes must be separated by white space");
      if (++count > U_DRICONF_XML_MAX_ATTRS)
         return fail(x, "too many attributes");
      size_t attr = parse_name(x, "an attribute");
      if (attr == SIZE_MAX)
         return false;
      for (size_t i = first; i < x->attr_count; i += 2) {
         if (!strcmp(x->strings + x->attrs[i], x->strings + attr))
            return fail(x, "an attribute is given twice");
      }
      skip_space(x);
      if (x->p >= x->end || *x->p != '=')
         return fail(x, "an attribute without '='");
      advance(x, 1);
      skip_space(x);
      size_t value = parse_value(x);
      if (value == SIZE_MAX)
         return false;
      if (!push_attr(x, attr) || !push_attr(x, value))
         return false;
   }
   if (!push_attr(x, SIZE_MAX) || !push_event(x, name, first, line))
      return false;
   if (*x->p == '/') {
      advance(x, 2);
      return push_event(x, name, SIZE_MAX, line);
   }
   advance(x, 1);
   if (x->depth == U_DRICONF_XML_MAX_DEPTH)
      return fail(x, "the elements are nested too deep");
   x->stack[x->depth++] = name;
   return true;
}

static bool
parse_end_tag(struct xml_parser *x)
{
   unsigned line = x->line;
   advance(x, 2);   /* "</" */
   const char *start = x->p;
   while (x->p < x->end && name_char(*x->p))
      x->p++;
   size_t n = (size_t)(x->p - start);
   if (!x->depth)
      return fail(x, "an end tag without its start tag");
   const char *open = x->strings + x->stack[x->depth - 1];
   if (strlen(open) != n || memcmp(open, start, n))
      return fail(x, "the end tag does not match the start tag");
   skip_space(x);
   if (x->p >= x->end || *x->p != '>')
      return fail(x, "an end tag without its '>'");
   advance(x, 1);
   x->depth--;
   return push_event(x, x->stack[x->depth], SIZE_MAX, line);
}

/* Comments and white space before and after the root element. */
static bool
parse_misc(struct xml_parser *x)
{
   for (;;) {
      skip_space(x);
      if (!starts(x, "<!--"))
         return true;
      if (!parse_comment(x))
         return false;
   }
}

static bool
parse_document(struct xml_parser *x)
{
   if (!check_text(x))
      return false;
   if (starts(x, "\xef\xbb\xbf"))
      x->p += 3;
   if (starts(x, "<?xml") && x->p + 5 < x->end && (is_space(x->p[5]) || x->p[5] == '?')) {
      if (!parse_declaration(x))
         return false;
   }
   if (!parse_misc(x))
      return false;
   if (starts(x, "<!DOCTYPE")) {
      if (!parse_doctype(x) || !parse_misc(x))
         return false;
   }
   if (x->p >= x->end || *x->p != '<' || x->p + 1 >= x->end || !name_start(x->p[1]))
      return fail(x, "no root element");
   if (!parse_start_tag(x))
      return false;
   while (x->depth) {
      if (x->p >= x->end)
         return fail(x, "the file ends inside an element");
      char c = *x->p;
      if (is_space(c)) {
         advance(x, 1);
      } else if (starts(x, "<!--")) {
         if (!parse_comment(x))
            return false;
      } else if (starts(x, "</")) {
         if (!parse_end_tag(x))
            return false;
      } else if (c == '<' && x->p + 1 < x->end && name_start(x->p[1])) {
         if (!parse_start_tag(x))
            return false;
      } else if (starts(x, "<![CDATA[")) {
         return fail(x, "CDATA sections are not read");
      } else if (starts(x, "<?")) {
         return fail(x, "processing instructions are not read");
      } else {
         return fail(x, "text between the tags: a driconf file has only elements and comments");
      }
   }
   if (!parse_misc(x))
      return false;
   if (x->p != x->end)
      return fail(x, "something after the root element");
   return true;
}

bool
u_driconf_xml_parse(const char *text, size_t size, struct u_driconf_xml *xml, char *error, size_t error_size)
{
   memset(xml, 0, sizeof(*xml));
   if (error_size)
      error[0] = 0;
   struct xml_parser x = {
      .p = text,
      .end = text + size,
      .line = 1,
      .error = error,
      .error_size = error_size,
   };
   if (size > U_DRICONF_FILE_MAX_SIZE) {
      fail(&x, "the file is too large");
      return false;
   }
   /* Every string comes from the text and is never longer than its source, and each needs one zero. */
   x.strings_size = 2 * size + 2;
   x.strings = malloc(x.strings_size);
   if (!x.strings) {
      fail(&x, "out of memory");
      return false;
   }
   bool ok = parse_document(&x);
   if (ok) {
      xml->events = calloc(x.event_count ? x.event_count : 1, sizeof(*xml->events));
      xml->attrs = calloc(x.attr_count ? x.attr_count : 1, sizeof(*xml->attrs));
      ok = xml->events && xml->attrs;
      if (!ok)
         fail(&x, "out of memory");
   }
   if (ok) {
      for (size_t i = 0; i < x.attr_count; i++)
         xml->attrs[i] = x.attrs[i] == SIZE_MAX ? NULL : x.strings + x.attrs[i];
      for (size_t i = 0; i < x.event_count; i++) {
         xml->events[i].name = x.strings + x.events[i].name;
         xml->events[i].attr = x.events[i].attr == SIZE_MAX ? NULL : xml->attrs + x.events[i].attr;
         xml->events[i].line = x.events[i].line;
      }
      xml->count = (unsigned)x.event_count;
      xml->strings = x.strings;
   } else {
      free(xml->events);
      free(xml->attrs);
      memset(xml, 0, sizeof(*xml));
      free(x.strings);
   }
   free(x.attrs);
   free(x.events);
   return ok;
}

void
u_driconf_xml_fini(struct u_driconf_xml *xml)
{
   free(xml->events);
   free(xml->attrs);
   free(xml->strings);
   memset(xml, 0, sizeof(*xml));
}

/* ---- the profile directory (Windows) ------------------------------------------------------------------------- */

#ifdef _WIN32

#ifdef U_DRICONF_FILE_TEST
const wchar_t *u_driconf_test_root;
bool u_driconf_test_trust_all;
#endif

#define MAX_FILES 256

/* SYSTEM, Administrators and TrustedInstaller. */
static bool
trusted_sid(PSID sid)
{
   static const struct {
      BYTE revision, count;
      SID_IDENTIFIER_AUTHORITY authority;
      DWORD sub[6];
   } trusted_installer = {SID_REVISION, 6, {SECURITY_NT_AUTHORITY},
                          {80, 956008885, 3418522649u, 1831038044, 1853292631, 2271478464u}};
   return IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid) ||
          EqualSid(sid, (PSID)&trusted_installer);
}

static void
sid_text(PSID sid, char *out, size_t size)
{
   char *text = NULL;
   if (ConvertSidToStringSidA(sid, &text)) {
      snprintf(out, size, "%s", text);
      LocalFree(text);
   } else {
      snprintf(out, size, "?");
   }
}

bool
u_driconf_trusted_descriptor(void *security_descriptor, char *why, size_t why_size)
{
   /* The rights that change the content of a file or the entries of a directory, or the access rules. */
   const DWORD write = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER |
                       GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED;
   PSECURITY_DESCRIPTOR sd = security_descriptor;
   PSID owner = NULL;
   BOOL defaulted = FALSE, present = FALSE;
   PACL dacl = NULL;
   char sid[96];

   if (!GetSecurityDescriptorOwner(sd, &owner, &defaulted) || !owner) {
      snprintf(why, why_size, "it has no owner");
      return false;
   }
   if (!trusted_sid(owner)) {
      sid_text(owner, sid, sizeof(sid));
      snprintf(why, why_size, "its owner %s is not SYSTEM, Administrators or TrustedInstaller", sid);
      return false;
   }
   if (!GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) || !present || !dacl) {
      snprintf(why, why_size, "it has no access list, so every account may change it");
      return false;
   }
   for (DWORD i = 0; i < dacl->AceCount; i++) {
      ACE_HEADER *ace = NULL;
      if (!GetAce(dacl, i, (void **)&ace)) {
         snprintf(why, why_size, "its access list cannot be read");
         return false;
      }
      if (ace->AceFlags & INHERIT_ONLY_ACE)
         continue;
      if (ace->AceType == ACCESS_DENIED_ACE_TYPE || ace->AceType == ACCESS_DENIED_CALLBACK_ACE_TYPE)
         continue;
      if (ace->AceType != ACCESS_ALLOWED_ACE_TYPE && ace->AceType != ACCESS_ALLOWED_CALLBACK_ACE_TYPE) {
         if (ace->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE || ace->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) {
            snprintf(why, why_size, "its access list has an object access entry");
            return false;
         }
         continue;   /* audit and label entries give no access */
      }
      /* ACCESS_ALLOWED_CALLBACK_ACE starts as ACCESS_ALLOWED_ACE does. */
      ACCESS_ALLOWED_ACE *allowed = (ACCESS_ALLOWED_ACE *)ace;
      PSID who = (PSID)&allowed->SidStart;
      if (!(allowed->Mask & write) || trusted_sid(who) || IsWellKnownSid(who, WinCreatorOwnerRightsSid) ||
          IsWellKnownSid(who, WinCreatorOwnerSid))
         continue;
      sid_text(who, sid, sizeof(sid));
      snprintf(why, why_size, "it gives write access to %s", sid);
      return false;
   }
   return true;
}

static bool
trusted_handle(HANDLE handle, char *why, size_t why_size)
{
#ifdef U_DRICONF_FILE_TEST
   if (u_driconf_test_trust_all)
      return true;
#endif
   PSECURITY_DESCRIPTOR sd = NULL;
   DWORD status = GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                  NULL, NULL, NULL, NULL, &sd);
   if (status != ERROR_SUCCESS) {
      snprintf(why, why_size, "its access rules cannot be read (error %lu)", (unsigned long)status);
      return false;
   }
   bool ok = u_driconf_trusted_descriptor(sd, why, why_size);
   LocalFree(sd);
   return ok;
}

static void
utf8_path(const wchar_t *path, char *out, size_t size)
{
   if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, out, (int)size, NULL, NULL))
      snprintf(out, size, "?");
}

/* The profile root, "<system drive>\ProgramData\amdgpu-wddm\profiles". Microsoft does not support another place
 * for ProgramData, and the drive comes from the system, not from the environment of the process. */
static bool
profile_root(wchar_t *out, size_t size)
{
#ifdef U_DRICONF_FILE_TEST
   if (u_driconf_test_root) {
      return swprintf(out, size, L"%ls", u_driconf_test_root) > 0;
   }
#endif
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
   /* BD-167: only a lab build takes the root from the environment. */
   DWORD n = GetEnvironmentVariableW(L"AMDGPU_WDDM_PROFILE_DIR", out, (DWORD)size);
   if (n && n < size)
      return true;
#endif
   wchar_t windows[MAX_PATH];
   UINT length = GetSystemWindowsDirectoryW(windows, MAX_PATH);
   if (!length || length >= MAX_PATH || windows[1] != L':')
      return false;
   return swprintf(out, size, L"%lc:\\ProgramData\\amdgpu-wddm\\profiles", windows[0]) > 0;
}

/* Opens a directory of the profile path and checks it. false: absent (no log line) or refused (one log line). */
static bool
check_directory(const wchar_t *path)
{
   char name[MAX_PATH * 3], why[256];
   HANDLE handle = CreateFileW(path, READ_CONTROL | FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
   if (handle == INVALID_HANDLE_VALUE) {
      DWORD error = GetLastError();
      if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
         utf8_path(path, name, sizeof(name));
         u_driconf_log("%s is not read: it cannot be opened (error %lu)", name, (unsigned long)error);
      }
      return false;
   }
   BY_HANDLE_FILE_INFORMATION info;
   bool ok = GetFileInformationByHandle(handle, &info);
   if (!ok)
      snprintf(why, sizeof(why), "its attributes cannot be read");
   else if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      snprintf(why, sizeof(why), "it is not a directory"), ok = false;
   else if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
      snprintf(why, sizeof(why), "it is a reparse point (a link or a junction)"), ok = false;
   else
      ok = trusted_handle(handle, why, sizeof(why));
   CloseHandle(handle);
   if (!ok) {
      utf8_path(path, name, sizeof(name));
      u_driconf_log("%s is not read: %s", name, why);
   }
   return ok;
}

/* Reads one checked file whole. NULL: refused, with one log line. */
static char *
read_file(const wchar_t *path, const char *name, size_t *size)
{
   char why[256];
   HANDLE handle = CreateFileW(path, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
   if (handle == INVALID_HANDLE_VALUE) {
      u_driconf_log("%s is not read: it cannot be opened (error %lu)", name, (unsigned long)GetLastError());
      return NULL;
   }
   char *text = NULL;
   BY_HANDLE_FILE_INFORMATION info;
   bool ok = GetFileInformationByHandle(handle, &info);
   if (!ok)
      snprintf(why, sizeof(why), "its attributes cannot be read");
   else if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
      snprintf(why, sizeof(why), "it is not a regular file"), ok = false;
   else if (info.nFileSizeHigh || info.nFileSizeLow > U_DRICONF_FILE_MAX_SIZE)
      snprintf(why, sizeof(why), "it is larger than %u bytes", (unsigned)U_DRICONF_FILE_MAX_SIZE), ok = false;
   else
      ok = trusted_handle(handle, why, sizeof(why));
   if (ok) {
      DWORD length = info.nFileSizeLow, done = 0;
      text = malloc((size_t)length + 1);
      if (!text)
         snprintf(why, sizeof(why), "out of memory"), ok = false;
      else if (length && (!ReadFile(handle, text, length, &done, NULL) || done != length))
         snprintf(why, sizeof(why), "it cannot be read"), ok = false;
      if (ok) {
         text[length] = 0;
         *size = length;
      }
   }
   CloseHandle(handle);
   if (!ok) {
      free(text);
      u_driconf_log("%s is not read: %s", name, why);
      return NULL;
   }
   return text;
}

struct found_file {
   wchar_t name[MAX_PATH];
   char utf8[MAX_PATH * 3];
};

static int
compare_files(const void *a, const void *b)
{
   return strcmp(((const struct found_file *)a)->utf8, ((const struct found_file *)b)->utf8);
}

void
u_driconf_profile_files(u_driconf_profile_file_cb cb, void *data)
{
   wchar_t root[MAX_PATH], dir[MAX_PATH], pattern[MAX_PATH], path[MAX_PATH];
   char dir_name[MAX_PATH * 3], file_name[MAX_PATH * 4];

   if (!profile_root(root, MAX_PATH))
      return;
   if (swprintf(dir, MAX_PATH, L"%ls\\drirc.d", root) <= 0 || swprintf(pattern, MAX_PATH, L"%ls\\*.conf", dir) <= 0)
      return;
   if (!check_directory(root) || !check_directory(dir))
      return;
   utf8_path(dir, dir_name, sizeof(dir_name));

   WIN32_FIND_DATAW fd;
   HANDLE find = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
   if (find == INVALID_HANDLE_VALUE)
      return;
   struct found_file *files = calloc(MAX_FILES, sizeof(*files));
   unsigned count = 0;
   bool too_many = false;
   if (!files) {
      FindClose(find);
      u_driconf_log("%s is not read: out of memory", dir_name);
      return;
   }
   do {
      size_t length = wcslen(fd.cFileName);
      /* The pattern also finds names whose short 8.3 form matches; the long name must end in ".conf". */
      if (length <= 5 || _wcsicmp(fd.cFileName + length - 5, L".conf") || (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
         continue;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
         utf8_path(fd.cFileName, file_name, sizeof(file_name));
         u_driconf_log("%s\\%s is not read: it is a reparse point (a link)", dir_name, file_name);
         continue;
      }
      if (count == MAX_FILES) {
         too_many = true;
         break;
      }
      struct found_file *f = &files[count];
      if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, fd.cFileName, -1, f->utf8, sizeof(f->utf8), NULL, NULL)) {
         u_driconf_log("%s: a file name that is not valid UTF-16 is not read", dir_name);
         continue;
      }
      wcscpy(f->name, fd.cFileName);
      count++;
   } while (FindNextFileW(find, &fd));
   FindClose(find);

   if (too_many) {
      u_driconf_log("%s is not read: it has more than %u .conf files", dir_name, (unsigned)MAX_FILES);
      free(files);
      return;
   }
   qsort(files, count, sizeof(*files), compare_files);
   for (unsigned i = 0; i < count; i++) {
      if (swprintf(path, MAX_PATH, L"%ls\\%ls", dir, files[i].name) <= 0)
         continue;
      snprintf(file_name, sizeof(file_name), "%s\\%s", dir_name, files[i].utf8);
      size_t size = 0;
      char *text = read_file(path, file_name, &size);
      if (!text)
         continue;
      cb(data, file_name, text, size);
      free(text);
   }
   free(files);
}

#endif /* _WIN32 */
