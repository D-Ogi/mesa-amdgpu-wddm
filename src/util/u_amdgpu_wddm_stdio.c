/* SPDX-License-Identifier: MIT */

/* amdgpu-wddm: the streams behind amdgpu_wddm_stdio.h. This file also gets that header forced in, so it reaches the
 * C runtime's own streams through __acrt_iob_func and never names stdout or stderr. */
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "util/amdgpu_wddm_stdio.h"

static INIT_ONCE amdgpu_wddm_stdio_once = INIT_ONCE_STATIC_INIT;
/* Null: the C runtime's own streams. Otherwise NUL, the AMDGPU_WDDM_LOG file or the timed switch's file, for both
 * streams. */
static FILE *amdgpu_wddm_stdio_sink;
/* The amdgpu-wddm lines (amdgpu_wddm_log): null, the C runtime's stderr, the AMDGPU_WDDM_LOG file or the timed
 * switch's file. */
static FILE *amdgpu_wddm_log_sink;

/* The control app's timed switch ("Record game logs"), the contract of the D3D12 shell's stdio-log.h. A tester
 * cannot set an environment variable for a game that Steam or Epic starts, so the control app's elevated helper
 * writes HKLM\SOFTWARE\amdgpu-wddm\Diagnostics GameLogUntil, a REG_QWORD holding the UTC FILETIME at which logging
 * stops, and prepares %ProgramData%\amdgpu-wddm\logs\games with an access list that lets any user create files
 * there. A Vulkan game loads this ICD through the Vulkan loader, not through the shell, so nothing may have named a
 * file for it: with no marker and no AMDGPU_WDDM_LOG, and that instant still ahead, this module opens a new file of
 * its own and marks the process as the shell does, with AMDGPU_WDDM_LOG=file:<path> and
 * AMDGPU_WDDM_GAMELOG=<pid>;<the instant as 16 hex digits>. Every reader of the switch answers the marker the same
 * way:
 *   the marker names this process: another module opened the file. Append to the AMDGPU_WDDM_LOG file under the
 *     marker's instant and the size cap, with no second reading of the registry; without "file:<path>", no output.
 *   the marker names another process: both variables were inherited from the parent and say nothing about this
 *     one. Both are ignored and the switch decides again, for a file of this process with its own marker.
 *   no marker and AMDGPU_WDDM_LOG set: in the lab flavour the process asked for the lines itself, and nothing
 *     bounds them; in the release flavour the switch decides, as with the variable unset (BD-167).
 * The release flavour joins only a file that already exists directly in the switch's folder.
 * A marker of any other shape, or one longer than its buffer, counts as absent.
 *
 * Three bounds hold a switch somebody forgets: every line checks the instant, the file stops at
 * AMDGPU_WDDM_GAME_LOG_CAP bytes with one last line saying so, and the file is created once per process. The cap
 * is measured on the file, so the lines of every module that writes into it count. */
#define AMDGPU_WDDM_GAME_LOG_KEY "SOFTWARE\\amdgpu-wddm\\Diagnostics"
#define AMDGPU_WDDM_GAME_LOG_VALUE "GameLogUntil"
/* "<pid>;<16 hex digits of the UTC FILETIME at which recording stops>", the process that owns the recording. */
#define AMDGPU_WDDM_GAME_LOG_MARKER "AMDGPU_WDDM_GAMELOG"
/* Below %ProgramData%. The control app creates it and owns its access list. */
#define AMDGPU_WDDM_GAME_LOG_FOLDER "amdgpu-wddm\\logs\\games"
/* One file of one process, the shell's bound. */
#define AMDGPU_WDDM_GAME_LOG_CAP (64ull << 20)

/* The UTC FILETIME after which the sink writes nothing; 0 when the sink is not the timed switch's file. */
static ULONGLONG amdgpu_wddm_game_log_until;
/* Set once, by the line that finds a bound passed. */
static volatile LONG amdgpu_wddm_game_log_stopped;

/* Lines arrive whole in a file that other modules and processes append to, the timed switch's file above all. A
 * formatted print to an unbuffered stream reached the file a character at a time: two writers tore 7980 of 8000
 * lines, at about 350 us a line. An amdgpu-wddm line is formatted whole, at most AMDGPU_WDDM_LOG_LINE bytes (a
 * longer one is cut and ends with AMDGPU_WDDM_LOG_CUT), and written with one WriteFile on the append handle: 0 of
 * 8000 torn, about 5 us a line. Mesa's own prints reach the file through the redirected streams, which this file
 * cannot format whole, so a stream made here holds AMDGPU_WDDM_STDIO_BUFFER bytes and is flushed at every use of
 * the streams and before every amdgpu-wddm line: each write then carries whole prints, 0 of 8000 torn (one print
 * longer than the buffer still reaches the file in pieces). A buffer that only flushes when it is full cuts lines
 * (about 300 of 8000 torn at 2 KiB, 150 at 4 KiB). The cost: a print of Mesa reaches the file at the next use of
 * the streams or the next amdgpu-wddm line, or when the module is unloaded or the process ends normally; a process
 * that is terminated or crashes loses that last print. */
#define AMDGPU_WDDM_LOG_LINE 2048
#define AMDGPU_WDDM_LOG_CUT "...[line cut]\n"
#define AMDGPU_WDDM_STDIO_BUFFER 16384

static FILE *
amdgpu_wddm_stdio_from_handle(HANDLE handle)
{
   int fd = _open_osfhandle((intptr_t)handle, _O_APPEND | _O_WRONLY);
   if (fd < 0) {
      CloseHandle(handle);
      return NULL;
   }
   FILE *file = _fdopen(fd, "a");
   if (!file) {
      _close(fd);
      return NULL;
   }
   setvbuf(file, NULL, _IOFBF, AMDGPU_WDDM_STDIO_BUFFER);
   return file;
}

/* One line, one write: one WriteFile on the handle of a stream made here, which an append handle puts at the end of
 * the file in one piece, or one _write on the C runtime's stderr, which keeps its text mode. */
static int
amdgpu_wddm_log_write(FILE *out, const char *text, int length)
{
   if (length <= 0)
      return 0;
   if (out == __acrt_iob_func(2))
      return _write(_fileno(out), text, (unsigned)length);
   DWORD written = 0;
   if (!WriteFile((HANDLE)_get_osfhandle(_fileno(out)), text, (DWORD)length, &written, NULL))
      return 0;
   return (int)written;
}

/* A line formatted whole, then written once. */
static int
amdgpu_wddm_log_vprint(FILE *out, const char *format, va_list args)
{
   char line[AMDGPU_WDDM_LOG_LINE];
   int length = vsnprintf(line, sizeof(line), format, args);
   if (length < 0)
      return length;
   if (length >= (int)sizeof(line)) {
      length = (int)sizeof(line) - 1;
      memcpy(line + length - (sizeof(AMDGPU_WDDM_LOG_CUT) - 1), AMDGPU_WDDM_LOG_CUT, sizeof(AMDGPU_WDDM_LOG_CUT) - 1);
   }
   return amdgpu_wddm_log_write(out, line, length);
}

static int
amdgpu_wddm_log_print(FILE *out, const char *format, ...)
{
   va_list args;
   va_start(args, format);
   int written = amdgpu_wddm_log_vprint(out, format, args);
   va_end(args);
   return written;
}

#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
/* The lab flavour only (BD-167): the release flavour opens no file that AMDGPU_WDDM_LOG names by itself. */
static FILE *
amdgpu_wddm_stdio_open_append(const char *path)
{
   /* Append only and shared: the D3D12 shell and the engine may write to the same file, and so may other
    * processes; FILE_APPEND_DATA keeps each write whole at the end of the file. */
   HANDLE handle = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   if (handle == INVALID_HANDLE_VALUE)
      return NULL;
   return amdgpu_wddm_stdio_from_handle(handle);
}
#endif

static ULONGLONG
amdgpu_wddm_game_log_now(void)
{
   FILETIME now;
   GetSystemTimeAsFileTime(&now);
   return ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
}

/* GameLogUntil, read only, from the 64-bit view of the registry, so a 32-bit build reads the value the control app
 * wrote. 0: no switch, a value of another type or length, or a key that cannot be read. */
static ULONGLONG
amdgpu_wddm_game_log_switch(void)
{
   HKEY key = NULL;
   if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, AMDGPU_WDDM_GAME_LOG_KEY, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) !=
       ERROR_SUCCESS)
      return 0;
   ULONGLONG until = 0;
   DWORD type = 0, bytes = sizeof(until);
   LSTATUS status = RegQueryValueExA(key, AMDGPU_WDDM_GAME_LOG_VALUE, NULL, &type, (LPBYTE)&until, &bytes);
   RegCloseKey(key);
   return status == ERROR_SUCCESS && type == REG_QWORD && bytes == sizeof(until) ? until : 0;
}

/* The marker: one to ten decimal digits that fit in 32 bits, ';', exactly 16 hex digits of either case, and nothing
 * after them. Any other shape is refused and counts as no marker: a larger number would name this process once cut
 * to the 32 bits of a process id. */
static bool
amdgpu_wddm_game_log_parse_marker(const char *value, unsigned long *pid, ULONGLONG *until)
{
   const char *p = value;
   ULONGLONG id = 0, instant = 0;
   unsigned digits = 0, hex = 0;
   for (; *p >= '0' && *p <= '9'; p++, digits++) {
      if (digits == 10)
         return false;
      id = id * 10 + (unsigned)(*p - '0');
   }
   if (!digits || *p != ';' || id > 0xffffffffu)
      return false;
   for (p++; *p; p++, hex++) {
      unsigned nibble;
      if (*p >= '0' && *p <= '9')
         nibble = (unsigned)(*p - '0');
      else if (*p >= 'a' && *p <= 'f')
         nibble = (unsigned)(*p - 'a' + 10);
      else if (*p >= 'A' && *p <= 'F')
         nibble = (unsigned)(*p - 'A' + 10);
      else
         return false;
      if (hex == 16)
         return false;
      instant = (instant << 4) | nibble;
   }
   if (hex != 16)
      return false;
   *pid = (unsigned long)id;
   *until = instant;
   return true;
}

/* The base name of a process image for a file name: no directory, no extension, at most 64 characters, and only
 * letters, digits, '.', '-' and '_' (anything else becomes '_'). An empty path gives "process". */
static void
amdgpu_wddm_game_log_image_name(char *out, size_t size, const char *path)
{
   const char *base = path, *dot = NULL;
   for (const char *p = path; *p; p++) {
      if (*p == '\\' || *p == '/' || *p == ':')
         base = p + 1;
   }
   for (const char *p = base; *p; p++) {
      if (*p == '.')
         dot = p;
   }
   size_t length = dot && dot != base ? (size_t)(dot - base) : strlen(base);
   if (length > 64)
      length = 64;
   if (!length) {
      snprintf(out, size, "process");
      return;
   }
   size_t written = 0;
   for (size_t i = 0; i < length && written + 1 < size; i++) {
      unsigned char c = (unsigned char)base[i];
      bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' ||
                   c == '-' || c == '_';
      out[written++] = plain ? (char)c : '_';
   }
   out[written] = 0;
}

/* One new file of the timed switch. CREATE_NEW, so a name that exists is never written to and never truncated;
 * FILE_FLAG_OPEN_REPARSE_POINT with a check of what was really opened, so a name planted in the folder cannot send
 * the lines anywhere else. FILE_READ_ATTRIBUTES serves that check and the size bound. */
static FILE *
amdgpu_wddm_game_log_open_new(const char *path)
{
   HANDLE handle = CreateFileA(path, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
   if (handle == INVALID_HANDLE_VALUE)
      return NULL;
   BY_HANDLE_FILE_INFORMATION info;
   if (!GetFileInformationByHandle(handle, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      CloseHandle(handle);
      return NULL;
   }
   return amdgpu_wddm_stdio_from_handle(handle);
}

/* The timed switch: the instant, the folder, one new file and its first line. Null when the switch is off or has
 * run out, or when no file could be made. */
static FILE *
amdgpu_wddm_game_log_open(void)
{
   ULONGLONG until = amdgpu_wddm_game_log_switch();
   if (!until || amdgpu_wddm_game_log_now() >= until)
      return NULL;

   /* Windows sets ProgramData for every process; a process without it has no folder this module may write to. */
   char folder[MAX_PATH];
   DWORD length = GetEnvironmentVariableA("ProgramData", folder, sizeof(folder));
   if (!length || length >= sizeof(folder))
      return NULL;

   char image[MAX_PATH];
   length = GetModuleFileNameA(NULL, image, sizeof(image));
   if (!length || length >= sizeof(image))
      image[0] = 0;
   char name[72];
   amdgpu_wddm_game_log_image_name(name, sizeof(name), image);

   SYSTEMTIME utc;
   GetSystemTime(&utc);
   unsigned long pid = GetCurrentProcessId();
   char path[MAX_PATH];
   FILE *file = NULL;
   /* "<image>-<pid>-<yyyymmddThhmmssZ>.log", and from the second attempt on "-<attempt>" before the extension:
    * another module of this process (the D3D12 shell, another ICD build) may have taken the name in the same
    * second. */
   for (unsigned attempt = 0; attempt < 8 && !file; attempt++) {
      char suffix[16] = "";
      if (attempt)
         snprintf(suffix, sizeof(suffix), "-%u", attempt);
      int n = snprintf(path, sizeof(path), "%s\\%s\\%s-%lu-%04u%02u%02uT%02u%02u%02uZ%s.log", folder,
                       AMDGPU_WDDM_GAME_LOG_FOLDER, name, pid, utc.wYear, utc.wMonth, utc.wDay, utc.wHour,
                       utc.wMinute, utc.wSecond, suffix);
      if (n < 0 || n >= (int)sizeof(path))
         return NULL;
      file = amdgpu_wddm_game_log_open_new(path);
   }
   if (!file)
      return NULL;

   /* Mark the process, so that a module it loads later (the D3D12 shell, DXVK, vkd3d-proton, the other ICD line)
    * joins this file under the same bounds instead of making its own, and a process started from here finds another
    * pid in the marker and does not take the file at all. */
   char value[MAX_PATH + 8], mark[48];
   snprintf(value, sizeof(value), "file:%s", path);
   SetEnvironmentVariableA("AMDGPU_WDDM_LOG", value);
   snprintf(mark, sizeof(mark), "%lu;%016llX", pid, until);
   SetEnvironmentVariableA(AMDGPU_WDDM_GAME_LOG_MARKER, mark);

   /* The module that opened the file, so that a report says which ICD wrote it. */
   char module[MAX_PATH] = "";
   HMODULE self = NULL;
   if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCSTR)&amdgpu_wddm_game_log_open, &self)) {
      length = GetModuleFileNameA(self, module, sizeof(module));
      if (!length || length >= sizeof(module))
         module[0] = 0;
   }
   amdgpu_wddm_log_print(file,
                         "amdgpu-wddm game log: %s pid %lu, module %s, opened %04u-%02u-%02uT%02u:%02u:%02uZ, "
                         "recording until the time the control app set, at most %llu MiB\n",
                         image[0] ? image : "(unknown)", pid, module[0] ? module : "(unknown)", utc.wYear,
                         utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, AMDGPU_WDDM_GAME_LOG_CAP >> 20);
   amdgpu_wddm_game_log_until = until;
   return file;
}

/* Another module of this process opened the timed switch's file and marked the process: append to the file that
 * AMDGPU_WDDM_LOG names, under the marker's instant. GetFileSizeEx needs no access right beyond FILE_APPEND_DATA, so
 * the size bound works on this handle too. An instant of 0 lies in 1601: that recording is over. */
#if !defined(AMDGPU_WDDM_LAB_FLAVOUR)
/* Whether path names a file directly in folder: the folder, one backslash and a plain file name, with no directory
 * part, no stream and neither "." nor "..". The folder is compared without regard to case, as Windows compares it. */
static bool
amdgpu_wddm_game_log_member(const char *path, const char *folder)
{
   size_t length = strlen(folder);
   if (!length || _strnicmp(path, folder, length) || path[length] != '\\')
      return false;
   const char *name = path + length + 1;
   if (!*name || !strcmp(name, ".") || !strcmp(name, ".."))
      return false;
   for (const char *p = name; *p; p++) {
      if (*p == '\\' || *p == '/' || *p == ':')
         return false;
   }
   return true;
}

/* The release flavour joins only the switch's file (BD-167): a file that already exists directly in
 * %ProgramData%\amdgpu-wddm\logs\games, opened OPEN_EXISTING, so a join never creates a name, with the check of
 * open_new, and refused unless it is a plain file on a disk with one name, so a device name, a reparse point or a
 * second link cannot send the lines anywhere else. */
static FILE *
amdgpu_wddm_game_log_open_joined(const char *path)
{
   char root[MAX_PATH], folder[MAX_PATH];
   DWORD length = GetEnvironmentVariableA("ProgramData", root, sizeof(root));
   if (!length || length >= sizeof(root))
      return NULL;
   int n = snprintf(folder, sizeof(folder), "%s\\%s", root, AMDGPU_WDDM_GAME_LOG_FOLDER);
   if (n < 0 || n >= (int)sizeof(folder) || !amdgpu_wddm_game_log_member(path, folder))
      return NULL;
   HANDLE handle = CreateFileA(path, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
   if (handle == INVALID_HANDLE_VALUE)
      return NULL;
   BY_HANDLE_FILE_INFORMATION info;
   if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &info) ||
       (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
       info.nNumberOfLinks != 1) {
      CloseHandle(handle);
      return NULL;
   }
   return amdgpu_wddm_stdio_from_handle(handle);
}
#endif

static FILE *
amdgpu_wddm_game_log_join(const char *value, ULONGLONG until)
{
   if (!until || strncmp(value, "file:", 5) || !value[5])
      return NULL;
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
   FILE *file = amdgpu_wddm_stdio_open_append(value + 5);
#else
   FILE *file = amdgpu_wddm_game_log_open_joined(value + 5);
#endif
   if (file)
      amdgpu_wddm_game_log_until = until;
   return file;
}

/* Whether a line may go to the sink: always, unless the sink is the timed switch's file. First, what the stream
 * holds goes to the file, so that it keeps its place before the next line and counts toward the size bound. The
 * first line that finds a bound passed writes one last line and turns the file's descriptor into NUL, so a stream a
 * caller already holds (mesa_log keeps the stream of its first line, amdgpu_wddm_log_stream) writes nowhere from
 * then on and never turns null under its holder. */
static bool
amdgpu_wddm_game_log_admit(FILE *file)
{
   fflush(file);
   if (!amdgpu_wddm_game_log_until)
      return true;
   if (amdgpu_wddm_game_log_stopped)
      return false;
   const char *reason = NULL;
   LARGE_INTEGER size;
   if (amdgpu_wddm_game_log_now() >= amdgpu_wddm_game_log_until)
      reason = "the recording time is over";
   else if (!GetFileSizeEx((HANDLE)_get_osfhandle(_fileno(file)), &size))
      reason = "the recording stopped";
   else if ((ULONGLONG)size.QuadPart >= AMDGPU_WDDM_GAME_LOG_CAP)
      reason = "this file is full";
   if (!reason)
      return true;
   if (!InterlockedCompareExchange(&amdgpu_wddm_game_log_stopped, 1, 0)) {
      amdgpu_wddm_log_print(file, "amdgpu-wddm game log: nothing more is written, %s.\n", reason);
      int nul = _open("NUL", _O_WRONLY);
      if (nul >= 0) {
         _dup2(nul, _fileno(file));
         _close(nul);
      }
   }
   return false;
}

static BOOL CALLBACK
amdgpu_wddm_stdio_resolve(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
   (void)once;
   (void)parameter;
   (void)context;

   /* An executable built from this tree keeps its streams: the policy is for a DLL in someone else's process. A
    * module that cannot be told apart is treated as the DLL. */
   HMODULE self = NULL;
   if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCSTR)&amdgpu_wddm_stdio_resolve, &self) &&
       self == GetModuleHandleA(NULL)) {
      amdgpu_wddm_log_sink = __acrt_iob_func(2);
      return TRUE;
   }

   char value[MAX_PATH + 8];
   DWORD length = GetEnvironmentVariableA("AMDGPU_WDDM_LOG", value, sizeof(value));
   bool valid = length && length < sizeof(value);
   char mark[48];
   DWORD marked = GetEnvironmentVariableA(AMDGPU_WDDM_GAME_LOG_MARKER, mark, sizeof(mark));
   unsigned long pid = 0;
   ULONGLONG until = 0;
   /* The lab flavour: a value of AMDGPU_WDDM_LOG too long for the buffer is still a value: no file, whatever the
    * marker says. With the marker: this process marked it, so the marker is the file, joined or nothing; another
    * process marked it, so both variables were inherited and the switch decides again. Without the marker: a value
    * the process carries wins, with no bounds, and no AMDGPU_WDDM_LOG at all leaves it to the switch. The timed
    * switch's file takes precedence over the Mesa debug variables below, as any file sink does. */
   bool marker = marked && marked < sizeof(mark) && amdgpu_wddm_game_log_parse_marker(mark, &pid, &until);
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
   if (length < sizeof(value) && marker) {
      if (pid != GetCurrentProcessId())
         amdgpu_wddm_stdio_sink = amdgpu_wddm_log_sink = amdgpu_wddm_game_log_open();
      else if (valid)
         amdgpu_wddm_stdio_sink = amdgpu_wddm_log_sink = amdgpu_wddm_game_log_join(value, until);
   } else if (valid && !strcmp(value, "stderr")) {
      amdgpu_wddm_log_sink = __acrt_iob_func(2);
      return TRUE;
   } else if (valid && !strncmp(value, "file:", 5) && value[5]) {
      amdgpu_wddm_stdio_sink = amdgpu_wddm_log_sink = amdgpu_wddm_stdio_open_append(value + 5);
   } else if (!length) {
      amdgpu_wddm_stdio_sink = amdgpu_wddm_log_sink = amdgpu_wddm_game_log_open();
   }
#else
   /* The release flavour reads AMDGPU_WDDM_LOG only beside a marker of this process, as the switch's file to join.
    * Without one, or with an inherited one, the variable is as if it were unset, whatever it holds, and the timed
    * switch decides (BD-167). */
   if (marker && pid == GetCurrentProcessId()) {
      if (valid)
         amdgpu_wddm_stdio_sink = amdgpu_wddm_log_sink = amdgpu_wddm_game_log_join(value, until);
   } else {
      amdgpu_wddm_stdio_sink = amdgpu_wddm_log_sink = amdgpu_wddm_game_log_open();
   }
#endif
   /* An explicit Mesa debug variable asks for its output on stderr, as an explicit VKD3D_DEBUG does in the engine:
    * RADV_DEBUG (psocachestats and the rest), MESA_SHADER_CACHE_SHOW_STATS, MESA_LOG and the compiler dumps. Only
    * the file sink takes precedence. */
   static const char *const explicit_debug[] = {
#ifdef AMDGPU_WDDM_LAB_FLAVOUR   /* BD-167: only the lab flavour reads these two */
      "RADV_DEBUG", "ACO_DEBUG",
#endif
      "NIR_DEBUG", "MESA_DEBUG", "MESA_LOG", "MESA_SHADER_CACHE_SHOW_STATS",
   };
   for (unsigned i = 0; !amdgpu_wddm_stdio_sink && i < sizeof(explicit_debug) / sizeof(explicit_debug[0]); i++) {
      if (GetEnvironmentVariableA(explicit_debug[i], NULL, 0))
         return TRUE;
   }
   /* A file that cannot be opened means no output, as in the shell and the engine. */
   if (!amdgpu_wddm_stdio_sink)
      amdgpu_wddm_stdio_sink = fopen("NUL", "w");
   return TRUE;
}

FILE *
amdgpu_wddm_stdio(int fd)
{
   InitOnceExecuteOnce(&amdgpu_wddm_stdio_once, amdgpu_wddm_stdio_resolve, NULL, NULL);
   if (amdgpu_wddm_stdio_sink) {
      /* Every use of stdout or stderr flushes the prints before it, whole, and checks the timed switch's bounds; a
       * stopped file stays the same stream. */
      amdgpu_wddm_game_log_admit(amdgpu_wddm_stdio_sink);
      return amdgpu_wddm_stdio_sink;
   }
   return __acrt_iob_func(fd == 1 ? 1 : 2);
}

FILE *
amdgpu_wddm_log_stream(void)
{
   InitOnceExecuteOnce(&amdgpu_wddm_stdio_once, amdgpu_wddm_stdio_resolve, NULL, NULL);
   /* The timed switch's bounds hold for a caller that writes to the stream itself (radv_wddm2_cs.c with
    * AMDGPU_WDDM_LOG_PDD=1: its hex dump stays in the buffer until the next use, which writes it whole). */
   if (amdgpu_wddm_log_sink)
      amdgpu_wddm_game_log_admit(amdgpu_wddm_log_sink);
   return amdgpu_wddm_log_sink;
}

int
amdgpu_wddm_log(const char *format, ...)
{
   InitOnceExecuteOnce(&amdgpu_wddm_stdio_once, amdgpu_wddm_stdio_resolve, NULL, NULL);
   FILE *out = amdgpu_wddm_log_sink;
   if (!out || !amdgpu_wddm_game_log_admit(out))
      return 0;
   va_list args;
   va_start(args, format);
   int written = amdgpu_wddm_log_vprint(out, format, args);
   va_end(args);
   return written;
}

int
amdgpu_wddm_stdio_puts(const char *text)
{
   FILE *out = amdgpu_wddm_stdio(1);
   return fputs(text, out) < 0 || fputc('\n', out) == EOF ? EOF : 0;
}

int
amdgpu_wddm_stdio_putchar(int c)
{
   return fputc(c, amdgpu_wddm_stdio(1));
}
