/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */


#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "util/compress.h"
#include "util/crc32.h"
#include "util/u_debug.h"
#include "util/disk_cache.h"
#include "util/disk_cache_os.h"

#if DETECT_OS_WINDOWS

#include <windows.h>

/* The build identity of a loaded module, read from its PE headers in memory: what an ELF build-id note is elsewhere.
 * The linker writes a new time stamp and a new CodeView record (PDB GUID and age) for every link, so a rebuilt DLL gets
 * a new identity while a copy of the same DLL keeps it.
 *
 * The file at GetModuleFileName's path is not used: it need not hold the code that runs. A loaded DLL can be renamed
 * aside and another build put in its place, which is how a driver in use is replaced, and the loader keeps reporting
 * the old path. An identity read from that file would let the old code write cache entries under the new build's
 * identity.
 */
static bool
module_build_identity(HMODULE mod, blake3_hasher *ctx)
{
   const uint8_t *base = (const uint8_t *)mod;
   const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
   if (dos->e_magic != IMAGE_DOS_SIGNATURE)
      return false;

   const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
   if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC)
      return false;

   struct {
      uint32_t machine;
      uint32_t time_date_stamp;
      uint32_t size_of_image;
      uint32_t check_sum;
      uint8_t pdb_guid[16];
      uint32_t pdb_age;
   } id;
   memset(&id, 0, sizeof(id));
   id.machine = nt->FileHeader.Machine;
   id.time_date_stamp = nt->FileHeader.TimeDateStamp;
   id.size_of_image = nt->OptionalHeader.SizeOfImage;
   id.check_sum = nt->OptionalHeader.CheckSum;

   /* The CodeView record: "RSDS", the PDB GUID, the age, the PDB path. */
   const IMAGE_DATA_DIRECTORY *debug_dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
   if (nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DEBUG && debug_dir->VirtualAddress &&
       (uint64_t)debug_dir->VirtualAddress + debug_dir->Size <= id.size_of_image) {
      const IMAGE_DEBUG_DIRECTORY *entry = (const IMAGE_DEBUG_DIRECTORY *)(base + debug_dir->VirtualAddress);
      for (unsigned i = 0; i < debug_dir->Size / sizeof(*entry); i++) {
         if (entry[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || !entry[i].AddressOfRawData || entry[i].SizeOfData < 24 ||
             (uint64_t)entry[i].AddressOfRawData + 24 > id.size_of_image)
            continue;

         const uint8_t *cv = base + entry[i].AddressOfRawData;
         if (memcmp(cv, "RSDS", 4) != 0)
            continue;

         memcpy(id.pdb_guid, cv + 4, sizeof(id.pdb_guid));
         memcpy(&id.pdb_age, cv + 20, sizeof(id.pdb_age));
         break;
      }
   }

   _mesa_blake3_update(ctx, &id, sizeof(id));
   return true;
}

bool
disk_cache_get_function_identifier(void *ptr, blake3_hasher *ctx)
{
   HMODULE mod = NULL;
   GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      (LPCWSTR)ptr,
                      &mod);
   if (!mod)
      return false;

   return module_build_identity(mod, ctx);
}

#endif

#ifdef ENABLE_SHADER_CACHE

#if DETECT_OS_WINDOWS

#include <stdio.h>
#include <string.h>

#include "util/u_string.h"

#else

#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "utime.h"

#endif

#include "util/blob.h"
#include "util/crc32.h"
#include "util/u_debug.h"
#include "util/ralloc.h"
#include "util/rand_xor.h"

#if DETECT_OS_WINDOWS

/* The Windows implementation of the multi-file cache. It keeps the layout and the protocol of the POSIX one:
 * one file per entry in a two-character subdirectory, written to "<name>.tmp" and renamed into place, and an index
 * file mapped shared by every process that uses the directory, holding the total size of the entries.
 *
 * Paths stay in UTF-8, as elsewhere in Mesa, with the forward slashes the shared code puts between components, and
 * are widened for each call into the file system: a profile path need not fit the ANSI code page.
 *
 * The single-file (Fossilize) and database caches are not implemented here: fossilize_db.c needs flock() and
 * mesa_cache_db.c is POSIX-only. disk_cache_type_create() makes a database or single-file cache, such as RADV's
 * built-in shader cache or a main cache under MESA_DISK_CACHE_SINGLE_FILE, a multi-file one.
 */

/* The size an entry takes on the disk, as st_blocks * 512 is on POSIX: its length rounded up to the 4 KiB cluster
 * NTFS uses by default. Writes add it and evictions subtract it, both from the file length, so the total in the
 * index stays exact whatever the real cluster size. */
static uint64_t
cache_file_size(uint64_t length)
{
   return (length + 4095) & ~(uint64_t)4095;
}

static wchar_t *
utf8_to_wide(const char *path)
{
   int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
   if (len <= 0)
      return NULL;

   wchar_t *wpath = malloc(len * sizeof(wchar_t));
   if (!wpath)
      return NULL;

   if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, len)) {
      free(wpath);
      return NULL;
   }

   /* Backslashes only, and no runs of them past a UNC prefix: the directory walk below splits at them. */
   wchar_t *out = wpath;
   for (const wchar_t *in = wpath; *in; in++) {
      wchar_t c = *in == L'/' ? L'\\' : *in;
      if (c == L'\\' && out - wpath >= 2 && out[-1] == L'\\')
         continue;
      *out++ = c;
   }
   *out = L'\0';

   return wpath;
}

static char *
wide_to_utf8(const wchar_t *wstr)
{
   int len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wstr, -1, NULL, 0, NULL, NULL);
   if (len <= 0)
      return NULL;

   char *str = malloc(len);
   if (str && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wstr, -1, str, len, NULL, NULL)) {
      free(str);
      return NULL;
   }

   return str;
}

/* An environment variable in UTF-8, or NULL when it is unset or empty. */
static char *
get_env_utf8(void *mem_ctx, const wchar_t *name)
{
   DWORD len = GetEnvironmentVariableW(name, NULL, 0);
   if (len <= 1)
      return NULL;

   wchar_t *wvalue = malloc(len * sizeof(wchar_t));
   if (!wvalue)
      return NULL;

   char *value = NULL;
   DWORD ret = GetEnvironmentVariableW(name, wvalue, len);
   if (ret > 0 && ret < len) {
      char *utf8 = wide_to_utf8(wvalue);
      if (utf8) {
         value = ralloc_strdup(mem_ctx, utf8);
         free(utf8);
      }
   }
   free(wvalue);

   return value;
}

static bool
get_file_length(const char *path, uint64_t *length)
{
   wchar_t *wpath = utf8_to_wide(path);
   if (!wpath)
      return false;

   WIN32_FILE_ATTRIBUTE_DATA data;
   bool ret = GetFileAttributesExW(wpath, GetFileExInfoStandard, &data) &&
              !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
   free(wpath);

   if (ret)
      *length = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
   return ret;
}

static bool
delete_file(const char *path)
{
   wchar_t *wpath = utf8_to_wide(path);
   if (!wpath)
      return false;

   bool ret = DeleteFileW(wpath);
   free(wpath);
   return ret;
}

/* Creates the directory and any missing parents. Returns true when the path exists afterwards, as whatever it is. */
static bool
create_dir_with_parents(wchar_t *wpath)
{
   if (CreateDirectoryW(wpath, NULL) || GetLastError() == ERROR_ALREADY_EXISTS)
      return true;

   if (GetLastError() != ERROR_PATH_NOT_FOUND)
      return false;

   wchar_t *sep = wcsrchr(wpath, L'\\');
   if (!sep || sep == wpath)
      return false;

   *sep = L'\0';
   bool parent = create_dir_with_parents(wpath);
   *sep = L'\\';

   return parent && (CreateDirectoryW(wpath, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
}

/* Check if directory exists or if mkdir param is set create a directory named
 * 'path' if it does not already exist, including parent directories if
 * required.
 *
 * Returns: 0 if path already exists as a directory or if created.
 *         -1 in all other cases.
 */
static int
find_or_create_dir(const char *path, bool mkdir_with_parents_if_needed)
{
   if (path[0] == '\0')
      return -1;

   wchar_t *wpath = utf8_to_wide(path);
   if (!wpath)
      return -1;

   int ret = -1;
   DWORD attrs = GetFileAttributesW(wpath);
   if (attrs == INVALID_FILE_ATTRIBUTES && mkdir_with_parents_if_needed) {
      if (create_dir_with_parents(wpath)) {
         attrs = GetFileAttributesW(wpath);
      } else {
         fprintf(stderr, "Failed to create %s for shader cache (error %lu)---disabling.\n",
                 path, GetLastError());
      }
   }

   if (attrs != INVALID_FILE_ATTRIBUTES) {
      if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
         ret = 0;
      } else {
         fprintf(stderr, "Cannot use %s for shader cache (not a directory)"
                         "---disabling.\n", path);
      }
   }

   free(wpath);
   return ret;
}

#else

/* Check if directory exists or if mkdir_if_needed param is set create a
 * directory named 'path' if it does not already exist.
 * This is for use by find_or_create_dir(). Use that instead.
 *
 * Returns: 0 if path already exists as a directory or if created.
 *         -1 in all other cases.
 */
static int
find_or_mkdir_if_needed(const char *path, bool mkdir_if_needed)
{
   struct stat sb;

   /* If the path exists already, then our work is done if it's a
    * directory, but it's an error if it is not.
    */
   if (stat(path, &sb) == 0) {
      if (S_ISDIR(sb.st_mode)) {
         return 0;
      } else {
         fprintf(stderr, "Cannot use %s for shader cache (not a directory)"
                         "---disabling.\n", path);
         return -1;
      }
   }

   if (!mkdir_if_needed)
      return -1;

   int ret = mkdir(path, 0700);
   if (ret == 0 || (ret == -1 && errno == EEXIST))
     return 0;

   fprintf(stderr, "Failed to create %s for shader cache (%s)---disabling.\n",
           path, strerror(errno));

   return -1;
}

/* Check if directory exists or if mkdir param is set create a directory named
 * 'path' if it does not already exist, including parent directories if
 * required.
 *
 * Returns: 0 if path already exists as a directory or if created.
 *         -1 in all other cases.
 */
static int
find_or_create_dir(const char *path, bool mkdir_with_parents_if_needed)
{
   char *p;
   const char *end;

   if (path[0] == '\0')
      return -1;

   p = strdup(path);
   end = p + strlen(p) + 1; /* end points to the \0 terminator */
   for (char *q = p; q != end; q++) {
      if (*q == '/' || q == end - 1) {
         if (q == p) {
            /* Skip the first / of an absolute path. */
            continue;
         }

         *q = '\0';

         if (find_or_mkdir_if_needed(p, mkdir_with_parents_if_needed) == -1) {
            free(p);
            return -1;
         }

         *q = '/';
      }
   }
   free(p);

   return 0;
}

#endif

/* Concatenate an existing path and a new name to form a new path.  If the new
 * path does not exist as a directory, create it if the mkdir param is set
 * then return the resulting name of the new path (ralloc'ed off of 'ctx').
 *
 * Returns NULL on any error, such as:
 *
 *      <path>/<name> exists but is not a directory
 *      <path>/<name> cannot be created as a directory
 *      <path>/<name> does not exist and mkdir param is false
 */
static char *
concatenate_and_mkdir(void *ctx, const char *path, const char *name,
                      bool mkdir)
{
   char *new_path;

   new_path = ralloc_asprintf(ctx, "%s/%s", path, name);

   if (find_or_create_dir(new_path, mkdir) == 0)
      return new_path;

   return NULL;
}

struct lru_file {
   struct list_head node;
   char *lru_name;
   size_t lru_file_size;
   time_t lru_atime;
};

static void
free_lru_file_list(struct list_head *lru_file_list)
{
   struct lru_file *e, *next;
   LIST_FOR_EACH_ENTRY_SAFE(e, next, lru_file_list, node) {
      free(e->lru_name);
      free(e);
   }
   free(lru_file_list);
}

#if DETECT_OS_WINDOWS

struct lru_candidate {
   char *name;
   uint64_t length;
   time_t atime;
};

static int
compare_lru_candidates(const void *a, const void *b)
{
   const struct lru_candidate *ca = a, *cb = b;
   return ca->atime < cb->atime ? -1 : ca->atime > cb->atime;
}

/* Given a directory path and predicate function, create a linked list of the
 * entries with the oldest access time in that directory for which the
 * predicate returns true, oldest first: a tenth of them, and at least one.
 *
 * Returns: A malloc'ed linked list for the paths of chosen files, (or
 * NULL on any error). The caller should free the linked list via
 * free_lru_file_list() when finished.
 */
static struct list_head *
choose_lru_file_matching(const char *dir_path,
                         bool (*predicate)(const char *dir_path,
                                           const WIN32_FIND_DATAW *,
                                           const char *, const size_t))
{
   char *pattern = NULL;
   if (asprintf(&pattern, "%s/*", dir_path) == -1)
      return NULL;

   wchar_t *wpattern = utf8_to_wide(pattern);
   free(pattern);
   if (!wpattern)
      return NULL;

   WIN32_FIND_DATAW fd;
   HANDLE find = FindFirstFileExW(wpattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL,
                                  FIND_FIRST_EX_LARGE_FETCH);
   free(wpattern);
   if (find == INVALID_HANDLE_VALUE)
      return NULL;

   struct lru_candidate *candidates = NULL;
   unsigned count = 0, capacity = 0;
   do {
      char *name = wide_to_utf8(fd.cFileName);
      if (!name)
         continue;

      if (!predicate(dir_path, &fd, name, strlen(name))) {
         free(name);
         continue;
      }

      if (count == capacity) {
         unsigned new_capacity = capacity ? capacity * 2 : 64;
         struct lru_candidate *grown = realloc(candidates, new_capacity * sizeof(*candidates));
         if (!grown) {
            free(name);
            break;
         }
         candidates = grown;
         capacity = new_capacity;
      }

      ULARGE_INTEGER atime = {
         .LowPart = fd.ftLastAccessTime.dwLowDateTime,
         .HighPart = fd.ftLastAccessTime.dwHighDateTime,
      };
      candidates[count].name = name;
      candidates[count].length = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
      /* Seconds since 1970 from 100 ns ticks since 1601, as st_atime. */
      candidates[count].atime = (time_t)(atime.QuadPart / 10000000) - 11644473600ll;
      count++;
   } while (FindNextFileW(find, &fd));
   FindClose(find);

   struct list_head *lru_file_list = NULL;
   if (count) {
      qsort(candidates, count, sizeof(*candidates), compare_lru_candidates);

      /* Collect 10% of files in this directory for removal. Note: This should work
       * out to only be around 0.04% of total cache items.
       */
      unsigned lru_file_count = count > 10 ? count / 10 : 1;
      lru_file_list = malloc(sizeof(struct list_head));
      if (lru_file_list) {
         list_inithead(lru_file_list);
         for (unsigned i = 0; i < lru_file_count; i++) {
            struct lru_file *entry = calloc(1, sizeof(struct lru_file));
            if (!entry)
               break;

            if (asprintf(&entry->lru_name, "%s/%s", dir_path, candidates[i].name) == -1) {
               free(entry);
               break;
            }
            entry->lru_file_size = cache_file_size(candidates[i].length);
            entry->lru_atime = candidates[i].atime;
            list_addtail(&entry->node, lru_file_list);
         }

         if (list_is_empty(lru_file_list)) {
            free(lru_file_list);
            lru_file_list = NULL;
         }
      }
   }

   for (unsigned i = 0; i < count; i++)
      free(candidates[i].name);
   free(candidates);

   return lru_file_list;
}

/* Is entry a regular file, and not having a name with a trailing
 * ".tmp"
 */
static bool
is_regular_non_tmp_file(const char *path, const WIN32_FIND_DATAW *fd,
                        const char *d_name, const size_t len)
{
   if (fd->dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
      return false;

   if (len >= 4 && strcmp(&d_name[len-4], ".tmp") == 0)
      return false;

   return true;
}

/* Returns the size of the deleted files, (or 0 on any error). */
static size_t
unlink_lru_file_from_directory(const char *path)
{
   struct list_head *lru_file_list =
      choose_lru_file_matching(path, is_regular_non_tmp_file);
   if (lru_file_list == NULL)
      return 0;

   assert(!list_is_empty(lru_file_list));

   size_t total_unlinked_size = 0;
   struct lru_file *e;
   LIST_FOR_EACH_ENTRY(e, lru_file_list, node) {
      if (delete_file(e->lru_name))
         total_unlinked_size += e->lru_file_size;
   }
   free_lru_file_list(lru_file_list);

   return total_unlinked_size;
}

/* Is entry a directory with a two-character name, (and not the
 * special name of ".."). We also return false if the dir is empty.
 */
static bool
is_two_character_sub_directory(const char *path, const WIN32_FIND_DATAW *fd,
                               const char *d_name, const size_t len)
{
   if (!(fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
       (fd->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
      return false;

   if (len != 2)
      return false;

   if (strcmp(d_name, "..") == 0)
      return false;

   char *pattern;
   if (asprintf(&pattern, "%s/%s/*", path, d_name) == -1)
      return false;

   wchar_t *wpattern = utf8_to_wide(pattern);
   free(pattern);
   if (!wpattern)
      return false;

   WIN32_FIND_DATAW sub;
   HANDLE find = FindFirstFileExW(wpattern, FindExInfoBasic, &sub, FindExSearchNameMatch, NULL, 0);
   free(wpattern);
   if (find == INVALID_HANDLE_VALUE)
      return false;

   /* If dir only contains '.' and '..' it must be empty */
   bool empty = true;
   do {
      if (wcscmp(sub.cFileName, L".") != 0 && wcscmp(sub.cFileName, L"..") != 0)
         empty = false;
   } while (empty && FindNextFileW(find, &sub));
   FindClose(find);

   return !empty;
}

#else

/* Given a directory path and predicate function, create a linked list of entrys
 * with the oldest access time in that directory for which the predicate
 * returns true.
 *
 * Returns: A malloc'ed linkd list for the paths of chosen files, (or
 * NULL on any error). The caller should free the linked list via
 * free_lru_file_list() when finished.
 */
static struct list_head *
choose_lru_file_matching(const char *dir_path,
                         bool (*predicate)(const char *dir_path,
                                           const struct stat *,
                                           const char *, const size_t))
{
   DIR *dir;
   struct dirent *dir_ent;

   dir = opendir(dir_path);
   if (dir == NULL)
      return NULL;

   const int dir_fd = dirfd(dir);

   /* First count the number of files in the directory */
   unsigned total_file_count = 0;
   while ((dir_ent = readdir(dir)) != NULL) {
#ifdef HAVE_DIRENT_D_TYPE
      if (dir_ent->d_type == DT_REG) { /* If the entry is a regular file */
         total_file_count++;
      }
#else
      struct stat st;

      if (fstatat(dir_fd, dir_ent->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
         if (S_ISREG(st.st_mode)) {
            total_file_count++;
         }
      }
#endif
   }

   /* Reset to the start of the directory */
   rewinddir(dir);

   /* Collect 10% of files in this directory for removal. Note: This should work
    * out to only be around 0.04% of total cache items.
    */
   unsigned lru_file_count = total_file_count > 10 ? total_file_count / 10 : 1;
   struct list_head *lru_file_list = malloc(sizeof(struct list_head));
   list_inithead(lru_file_list);

   unsigned processed_files = 0;
   while (1) {
      dir_ent = readdir(dir);
      if (dir_ent == NULL)
         break;

      struct stat sb;
      if (fstatat(dir_fd, dir_ent->d_name, &sb, 0) == 0) {
         struct lru_file *entry = NULL;
         if (!list_is_empty(lru_file_list))
            entry = list_first_entry(lru_file_list, struct lru_file, node);

         if (!entry|| sb.st_atime < entry->lru_atime) {
            size_t len = strlen(dir_ent->d_name);
            if (!predicate(dir_path, &sb, dir_ent->d_name, len))
               continue;

            bool new_entry = false;
            if (processed_files < lru_file_count) {
               entry = calloc(1, sizeof(struct lru_file));
               new_entry = true;
            }
            processed_files++;

            char *tmp = realloc(entry->lru_name, len + 1);
            if (tmp) {
               /* Find location to insert new lru item. We want to keep the
                * list ordering from most recently used to least recently used.
                * This allows us to just evict the head item from the list as
                * we process the directory and find older entrys.
                */
               struct list_head *list_node = lru_file_list;
               struct lru_file *e;
               LIST_FOR_EACH_ENTRY(e, lru_file_list, node) {
                  if (sb.st_atime < entry->lru_atime) {
                     list_node = &e->node;
                     break;
                  }
               }

               if (new_entry) {
                  list_addtail(&entry->node, list_node);
               } else {
                  if (list_node != lru_file_list) {
                     list_del(lru_file_list);
                     list_addtail(lru_file_list, list_node);
                  }
               }

               entry->lru_name = tmp;
               memcpy(entry->lru_name, dir_ent->d_name, len + 1);
               entry->lru_atime = sb.st_atime;
               entry->lru_file_size = sb.st_blocks * 512;
            }
         }
      }
   }

   if (list_is_empty(lru_file_list)) {
      closedir(dir);
      free(lru_file_list);
      return NULL;
   }

   /* Create the full path for the file list we found */
   struct lru_file *e;
   LIST_FOR_EACH_ENTRY(e, lru_file_list, node) {
      char *filename = e->lru_name;
      if (asprintf(&e->lru_name, "%s/%s", dir_path, filename) < 0)
         e->lru_name = NULL;

      free(filename);
   }

   closedir(dir);

   return lru_file_list;
}

/* Is entry a regular file, and not having a name with a trailing
 * ".tmp"
 */
static bool
is_regular_non_tmp_file(const char *path, const struct stat *sb,
                        const char *d_name, const size_t len)
{
   if (!S_ISREG(sb->st_mode))
      return false;

   if (len >= 4 && strcmp(&d_name[len-4], ".tmp") == 0)
      return false;

   return true;
}

/* Returns the size of the deleted file, (or 0 on any error). */
static size_t
unlink_lru_file_from_directory(const char *path)
{
   struct list_head *lru_file_list =
      choose_lru_file_matching(path, is_regular_non_tmp_file);
   if (lru_file_list == NULL)
      return 0;

   assert(!list_is_empty(lru_file_list));

   size_t total_unlinked_size = 0;
   struct lru_file *e;
   LIST_FOR_EACH_ENTRY(e, lru_file_list, node) {
      if (unlink(e->lru_name) == 0)
         total_unlinked_size += e->lru_file_size;
   }
   free_lru_file_list(lru_file_list);

   return total_unlinked_size;
}

/* Is entry a directory with a two-character name, (and not the
 * special name of ".."). We also return false if the dir is empty.
 */
static bool
is_two_character_sub_directory(const char *path, const struct stat *sb,
                               const char *d_name, const size_t len)
{
   if (!S_ISDIR(sb->st_mode))
      return false;

   if (len != 2)
      return false;

   if (strcmp(d_name, "..") == 0)
      return false;

   char *subdir;
   if (asprintf(&subdir, "%s/%s", path, d_name) == -1)
      return false;
   DIR *dir = opendir(subdir);
   free(subdir);

   if (dir == NULL)
     return false;

   unsigned subdir_entries = 0;
   struct dirent *d;
   while ((d = readdir(dir)) != NULL) {
      if(++subdir_entries > 2)
         break;
   }
   closedir(dir);

   /* If dir only contains '.' and '..' it must be empty */
   if (subdir_entries <= 2)
      return false;

   return true;
}

#endif

/* Create the directory that will be needed for the cache file for \key.
 *
 * Obviously, the implementation here must closely match
 * _get_cache_file above.
*/
static void
make_cache_file_directory(struct disk_cache *cache, const cache_key key)
{
   char *dir;
   char buf[BLAKE3_HEX_LEN];

   _mesa_blake3_format(buf, key);
   if (asprintf(&dir, "%s/%c%c", cache->path, buf[0], buf[1]) == -1)
      return;

   find_or_create_dir(dir, true);
   free(dir);
}

#if DETECT_OS_WINDOWS

static bool
read_all(HANDLE file, void *buf, size_t count)
{
   char *in = buf;

   for (size_t done = 0; done < count;) {
      DWORD chunk = (DWORD)MIN2(count - done, 1u << 30), read_bytes;
      if (!ReadFile(file, in + done, chunk, &read_bytes, NULL) || read_bytes == 0)
         return false;
      done += read_bytes;
   }
   return true;
}

static bool
write_all(HANDLE file, const void *buf, size_t count)
{
   const char *out = buf;

   for (size_t done = 0; done < count;) {
      DWORD chunk = (DWORD)MIN2(count - done, 1u << 30), written;
      if (!WriteFile(file, out + done, chunk, &written, NULL) || written == 0)
         return false;
      done += written;
   }
   return true;
}

#else

static ssize_t
read_all(int fd, void *buf, size_t count)
{
   char *in = buf;
   ssize_t read_ret;
   size_t done;

   for (done = 0; done < count; done += read_ret) {
      read_ret = read(fd, in + done, count - done);
      if (read_ret == -1 || read_ret == 0)
         return -1;
   }
   return done;
}

static ssize_t
write_all(int fd, const void *buf, size_t count)
{
   const char *out = buf;
   ssize_t written;
   size_t done;

   for (done = 0; done < count; done += written) {
      written = write(fd, out + done, count - done);
      if (written == -1)
         return -1;
   }
   return done;
}

#endif

/* Evict least recently used cache item */
void
disk_cache_evict_lru_item(struct disk_cache *cache)
{
   char *dir_path;

   /* With a reasonably-sized, full cache, (and with keys generated
    * from a cryptographic hash), we can choose two random hex digits
    * and reasonably expect the directory to exist with a file in it.
    * Provides pseudo-LRU eviction to reduce checking all cache files.
    */
   uint64_t rand64 = rand_xorshift128plus(cache->seed_xorshift128plus);
   if (asprintf(&dir_path, "%s/%02" PRIx64 , cache->path, rand64 & 0xff) < 0)
      return;

   size_t size = unlink_lru_file_from_directory(dir_path);

   free(dir_path);

   if (size) {
      p_atomic_add(&cache->size->value, - (uint64_t)size);
      return;
   }

   /* In the case where the random choice of directory didn't find
    * something, we choose the least recently accessed from the
    * existing directories.
    *
    * Really, the only reason this code exists is to allow the unit
    * tests to work, (which use an artificially-small cache to be able
    * to force a single cached item to be evicted).
    */
   struct list_head *lru_file_list =
      choose_lru_file_matching(cache->path, is_two_character_sub_directory);
   if (lru_file_list == NULL)
      return;

   assert(!list_is_empty(lru_file_list));

   struct lru_file *lru_file_dir =
      list_first_entry(lru_file_list, struct lru_file, node);

   size = unlink_lru_file_from_directory(lru_file_dir->lru_name);

   free_lru_file_list(lru_file_list);

   if (size)
      p_atomic_add(&cache->size->value, - (uint64_t)size);
}

#if DETECT_OS_WINDOWS

void
disk_cache_evict_item(struct disk_cache *cache, char *filename)
{
   uint64_t length;
   if (get_file_length(filename, &length) && delete_file(filename))
      p_atomic_add(&cache->size->value, - cache_file_size(length));

   free(filename);
}

#else

void
disk_cache_evict_item(struct disk_cache *cache, char *filename)
{
   struct stat sb;
   if (stat(filename, &sb) == -1) {
      free(filename);
      return;
   }

   unlink(filename);
   free(filename);

   if (sb.st_blocks)
      p_atomic_add(&cache->size->value, - (uint64_t)sb.st_blocks * 512);
}

#endif

static void *
parse_and_validate_cache_item(struct disk_cache *cache, void *cache_item,
                              size_t cache_item_size, size_t *size)
{
   uint8_t *uncompressed_data = NULL;

   struct blob_reader ci_blob_reader;
   blob_reader_init(&ci_blob_reader, cache_item, cache_item_size);

   size_t header_size = cache->driver_keys_blob_size;
   const void *keys_blob = blob_read_bytes(&ci_blob_reader, header_size);
   if (ci_blob_reader.overrun)
      goto fail;

   /* Check for extremely unlikely hash collisions */
   if (memcmp(cache->driver_keys_blob, keys_blob, header_size) != 0) {
      assert(!"Mesa cache keys mismatch!");
      goto fail;
   }

   uint32_t md_type = blob_read_uint32(&ci_blob_reader);
   if (ci_blob_reader.overrun)
      goto fail;

   if (md_type == CACHE_ITEM_TYPE_GLSL) {
      uint32_t num_keys = blob_read_uint32(&ci_blob_reader);
      if (ci_blob_reader.overrun)
         goto fail;

      /* The cache item metadata is currently just used for distributing
       * precompiled shaders, they are not used by Mesa so just skip them for
       * now.
       * TODO: pass the metadata back to the caller and do some basic
       * validation.
       */
      const void UNUSED *metadata =
         blob_read_bytes(&ci_blob_reader, num_keys * sizeof(cache_key));
      if (ci_blob_reader.overrun)
         goto fail;
   }

   /* Load the CRC that was created when the file was written. */
   struct cache_entry_file_data *cf_data =
      (struct cache_entry_file_data *)
         blob_read_bytes(&ci_blob_reader, sizeof(struct cache_entry_file_data));
   if (ci_blob_reader.overrun)
      goto fail;

   size_t cache_data_size = ci_blob_reader.end - ci_blob_reader.current;
   const uint8_t *data = (uint8_t *) blob_read_bytes(&ci_blob_reader, cache_data_size);

   /* Check the data for corruption */
   if (cf_data->crc32 != util_hash_crc32(data, cache_data_size))
      goto fail;

   /* Uncompress the cache data */
   uncompressed_data = malloc(cf_data->uncompressed_size);
   if (!uncompressed_data)
      goto fail;

   if (cache->compression_disabled) {
      if (cf_data->uncompressed_size != cache_data_size)
         goto fail;

      memcpy(uncompressed_data, data, cache_data_size);
   } else {
      if (!util_compress_inflate(data, cache_data_size, uncompressed_data,
                                 cf_data->uncompressed_size))
         goto fail;
   }

   if (size)
      *size = cf_data->uncompressed_size;

   return uncompressed_data;

 fail:
   free(uncompressed_data);

   return NULL;
}

#if DETECT_OS_WINDOWS

/* One day in 100 ns ticks. NTFS keeps last-access times only when the volume is set to, so a load sets the time
 * itself when it is older than this, as relatime would: eviction picks the files with the oldest. */
#define CACHE_ACCESS_TIME_GRANULARITY (24ll * 60 * 60 * 10000000)

void *
disk_cache_load_item(struct disk_cache *cache, char *filename, size_t *size)
{
   uint8_t *data = NULL;
   uint8_t *uncompressed_data = NULL;
   uint64_t length = 0;
   bool corrupt = false;

   wchar_t *wfilename = utf8_to_wide(filename);
   if (!wfilename)
      goto done;

   /* Shared for writing and deleting too: an entry never changes after its rename, a writer still holds it for a
    * moment after, and an eviction in another process may delete it while it is read here.
    */
   HANDLE file = CreateFileW(wfilename, GENERIC_READ | FILE_WRITE_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                             FILE_FLAG_SEQUENTIAL_SCAN, NULL);
   if (file == INVALID_HANDLE_VALUE)
      goto done;

   LARGE_INTEGER file_size;
   FILE_BASIC_INFO basic;
   if (!GetFileSizeEx(file, &file_size) ||
       !GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)))
      goto close;

   length = file_size.QuadPart;
   if (length == 0 || length > SIZE_MAX) {
      corrupt = true;
      goto close;
   }

   data = malloc(length);
   if (data == NULL)
      goto close;

   /* Read entire file into memory */
   if (!read_all(file, data, length)) {
      corrupt = true;
      goto close;
   }

   uncompressed_data = parse_and_validate_cache_item(cache, data, length, size);
   if (!uncompressed_data) {
      corrupt = true;
      goto close;
   }

   FILETIME now;
   GetSystemTimeAsFileTime(&now);
   int64_t now_ticks = ((int64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;
   if (now_ticks - basic.LastAccessTime.QuadPart > CACHE_ACCESS_TIME_GRANULARITY) {
      FILE_BASIC_INFO touch = { 0 }; /* zero leaves a field as it is */
      touch.LastAccessTime.QuadPart = now_ticks;
      SetFileInformationByHandle(file, FileBasicInfo, &touch, sizeof(touch));
   }

 close:
   CloseHandle(file);

   /* An entry that cannot be read back, such as one whose data a power loss cut off after its rename, would stay
    * as it is: every load would miss, and every write of its key would find the file there and skip it. Delete it
    * so that the next write replaces it. A load that fails for want of memory deletes a good entry too, which costs
    * one compile.
    */
   if (corrupt && delete_file(filename))
      p_atomic_add(&cache->size->value, - cache_file_size(length));

 done:
   free(data);
   free(wfilename);
   free(filename);

   return uncompressed_data;
}

#else

void *
disk_cache_load_item(struct disk_cache *cache, char *filename, size_t *size)
{
   uint8_t *data = NULL;

   int fd = open(filename, O_RDONLY | O_CLOEXEC);
   if (fd == -1)
      goto fail;

   struct stat sb;
   if (fstat(fd, &sb) == -1)
      goto fail;

   data = malloc(sb.st_size);
   if (data == NULL)
      goto fail;

   /* Read entire file into memory */
   int ret = read_all(fd, data, sb.st_size);
   if (ret == -1)
      goto fail;

    uint8_t *uncompressed_data =
       parse_and_validate_cache_item(cache, data, sb.st_size, size);
   if (!uncompressed_data)
      goto fail;

   free(data);
   free(filename);
   close(fd);

   return uncompressed_data;

 fail:
   free(data);
   free(filename);
   if (fd != -1)
      close(fd);

   return NULL;
}

#endif

/* Return a filename within the cache's directory corresponding to 'key'.
 *
 * Returns NULL if out of memory.
 */
char *
disk_cache_get_cache_filename(struct disk_cache *cache, const cache_key key)
{
   char buf[BLAKE3_HEX_LEN];
   char *filename;

   if (cache->path_init_failed)
      return NULL;

   _mesa_blake3_format(buf, key);
   if (asprintf(&filename, "%s/%c%c/%s", cache->path, buf[0],
                buf[1], buf + 2) == -1)
      return NULL;

   return filename;
}

static bool
create_cache_item_header_and_blob(struct disk_cache_put_job *dc_job,
                                  struct blob *cache_blob)
{

   /* Compress the cache item data */
   size_t max_buf = util_compress_max_compressed_len(dc_job->size);
   size_t compressed_size;
   void *compressed_data;

   if (dc_job->cache->compression_disabled) {
      compressed_size = dc_job->size;
      compressed_data = dc_job->data;
   } else {
      compressed_data = malloc(max_buf);
      if (compressed_data == NULL)
         return false;
      compressed_size =
         util_compress_deflate(dc_job->data, dc_job->size,
                              compressed_data, max_buf);
      if (compressed_size == 0)
         goto fail;
   }

   /* Copy the driver_keys_blob, this can be used find information about the
    * mesa version that produced the entry or deal with hash collisions,
    * should that ever become a real problem.
    */
   if (!blob_write_bytes(cache_blob, dc_job->cache->driver_keys_blob,
                         dc_job->cache->driver_keys_blob_size))
      goto fail;

   /* Write the cache item metadata. This data can be used to deal with
    * hash collisions, as well as providing useful information to 3rd party
    * tools reading the cache files.
    */
   if (!blob_write_uint32(cache_blob, dc_job->cache_item_metadata.type))
      goto fail;

   if (dc_job->cache_item_metadata.type == CACHE_ITEM_TYPE_GLSL) {
      if (!blob_write_uint32(cache_blob, dc_job->cache_item_metadata.num_keys))
         goto fail;

      size_t metadata_keys_size =
         dc_job->cache_item_metadata.num_keys * sizeof(cache_key);
      if (!blob_write_bytes(cache_blob, dc_job->cache_item_metadata.keys[0],
                            metadata_keys_size))
         goto fail;
   }

   /* Create CRC of the compressed data. We will read this when restoring the
    * cache and use it to check for corruption.
    */
   struct cache_entry_file_data cf_data;
   cf_data.crc32 = util_hash_crc32(compressed_data, compressed_size);
   cf_data.uncompressed_size = dc_job->size;

   if (!blob_write_bytes(cache_blob, &cf_data, sizeof(cf_data)))
      goto fail;

   /* Finally copy the compressed cache blob */
   if (!blob_write_bytes(cache_blob, compressed_data, compressed_size))
      goto fail;

   if (!dc_job->cache->compression_disabled)
      free(compressed_data);

   return true;

 fail:
   if (!dc_job->cache->compression_disabled)
      free(compressed_data);

   return false;
}

#if DETECT_OS_WINDOWS

void
disk_cache_write_item_to_disk(struct disk_cache_put_job *dc_job,
                              char *filename)
{
   HANDLE file = INVALID_HANDLE_VALUE;
   bool remove_tmp = false;
   wchar_t *wfilename = NULL, *wfilename_tmp = NULL;
   FILE_RENAME_INFO *rename_info = NULL;
   struct blob cache_blob;
   blob_init(&cache_blob);

   /* Write to a temporary file to allow for an atomic rename to the
    * final destination filename, (to prevent any readers from seeing
    * a partially written file).
    */
   char *filename_tmp = NULL;
   if (asprintf(&filename_tmp, "%s.tmp", filename) == -1)
      goto done;

   wfilename = utf8_to_wide(filename);
   wfilename_tmp = utf8_to_wide(filename_tmp);
   if (!wfilename || !wfilename_tmp)
      goto done;

   /* The open temporary file is the lock that the flock is elsewhere: it is
    * shared for reading only, so another writer of the same entry, in this
    * process or another, fails to open it and leaves the entry to this one. A
    * temporary file that a writer which died left behind is truncated and
    * reused. DELETE access is for the rename below.
    */
   file = CreateFileW(wfilename_tmp, GENERIC_WRITE | DELETE, FILE_SHARE_READ, NULL,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

   /* Make the two-character subdirectory within the cache as needed. */
   if (file == INVALID_HANDLE_VALUE) {
      if (GetLastError() != ERROR_PATH_NOT_FOUND)
         goto done;

      make_cache_file_directory(dc_job->cache, dc_job->key);

      file = CreateFileW(wfilename_tmp, GENERIC_WRITE | DELETE, FILE_SHARE_READ, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
      if (file == INVALID_HANDLE_VALUE)
         goto done;
   }
   remove_tmp = true;

   /* Now that we hold the temporary file, we can check to see if the
    * destination file already exists. If so, another process won the race
    * between when we saw that the file didn't exist and now. In this case, we
    * don't do anything more, (to ensure the size accounting of the cache
    * doesn't get off).
    */
   if (GetFileAttributesW(wfilename) != INVALID_FILE_ATTRIBUTES)
      goto done;

   /* OK, we're now on the hook to write out a file that we know is
    * not in the cache, and is also not being written out to the cache
    * by some other process.
    */
   if (!create_cache_item_header_and_blob(dc_job, &cache_blob))
      goto done;

   /* Now, finally, write out the contents to the temporary file, then
    * rename them atomically to the destination filename, and also
    * perform an atomic increment of the total cache size.
    */
   if (!write_all(file, cache_blob.data, cache_blob.size))
      goto done;

   /* The rename goes through the open handle, so the file renamed is the one
    * written. ReplaceIfExists stays FALSE: an entry is never replaced, so its
    * size is added once.
    */
   size_t name_size = (wcslen(wfilename) + 1) * sizeof(wchar_t);
   rename_info = calloc(1, sizeof(FILE_RENAME_INFO) + name_size);
   if (!rename_info)
      goto done;

   rename_info->FileNameLength = name_size - sizeof(wchar_t);
   memcpy(rename_info->FileName, wfilename, name_size);
   if (!SetFileInformationByHandle(file, FileRenameInfo, rename_info,
                                   sizeof(FILE_RENAME_INFO) + name_size))
      goto done;
   remove_tmp = false;

   p_atomic_add(&dc_job->cache->size->value, cache_file_size(cache_blob.size));

 done:
   if (file != INVALID_HANDLE_VALUE) {
      if (remove_tmp) {
         FILE_DISPOSITION_INFO disposition = { .DeleteFile = TRUE };
         SetFileInformationByHandle(file, FileDispositionInfo, &disposition,
                                    sizeof(disposition));
      }
      /* This close finally releases the lock, (now that the final file
       * has been renamed into place and the size has been added).
       */
      CloseHandle(file);
   }
   free(rename_info);
   free(wfilename_tmp);
   free(wfilename);
   free(filename_tmp);
   blob_finish(&cache_blob);
}

#else

void
disk_cache_write_item_to_disk(struct disk_cache_put_job *dc_job,
                              char *filename)
{
   int fd = -1, fd_final = -1;
   struct blob cache_blob;
   blob_init(&cache_blob);

   /* Write to a temporary file to allow for an atomic rename to the
    * final destination filename, (to prevent any readers from seeing
    * a partially written file).
    */
   char *filename_tmp = NULL;
   if (asprintf(&filename_tmp, "%s.tmp", filename) == -1)
      goto done;

   fd = open(filename_tmp, O_WRONLY | O_CLOEXEC | O_CREAT, 0644);

   /* Make the two-character subdirectory within the cache as needed. */
   if (fd == -1) {
      if (errno != ENOENT)
         goto done;

      make_cache_file_directory(dc_job->cache, dc_job->key);

      fd = open(filename_tmp, O_WRONLY | O_CLOEXEC | O_CREAT, 0644);
      if (fd == -1)
         goto done;
   }

   /* With the temporary file open, we take an exclusive flock on
    * it. If the flock fails, then another process still has the file
    * open with the flock held. So just let that file be responsible
    * for writing the file.
    */
#ifdef HAVE_FLOCK
   int err = flock(fd, LOCK_EX | LOCK_NB);
#else
   struct flock lock = {
      .l_start = 0,
      .l_len = 0, /* entire file */
      .l_type = F_WRLCK,
      .l_whence = SEEK_SET
   };
   int err = fcntl(fd, F_SETLK, &lock);
#endif
   if (err == -1)
      goto done;

   /* Now that we have the lock on the open temporary file, we can
    * check to see if the destination file already exists. If so,
    * another process won the race between when we saw that the file
    * didn't exist and now. In this case, we don't do anything more,
    * (to ensure the size accounting of the cache doesn't get off).
    */
   fd_final = open(filename, O_RDONLY | O_CLOEXEC);
   if (fd_final != -1) {
      unlink(filename_tmp);
      goto done;
   }

   /* OK, we're now on the hook to write out a file that we know is
    * not in the cache, and is also not being written out to the cache
    * by some other process.
    */
   if (!create_cache_item_header_and_blob(dc_job, &cache_blob)) {
      unlink(filename_tmp);
      goto done;
   }

   /* Now, finally, write out the contents to the temporary file, then
    * rename them atomically to the destination filename, and also
    * perform an atomic increment of the total cache size.
    */
   int ret = write_all(fd, cache_blob.data, cache_blob.size);
   if (ret == -1) {
      unlink(filename_tmp);
      goto done;
   }

   ret = rename(filename_tmp, filename);
   if (ret == -1) {
      unlink(filename_tmp);
      goto done;
   }

   struct stat sb;
   if (stat(filename, &sb) == -1) {
      /* Something went wrong remove the file */
      unlink(filename);
      goto done;
   }

   p_atomic_add(&dc_job->cache->size->value, sb.st_blocks * 512);

 done:
   if (fd_final != -1)
      close(fd_final);
   /* This close finally releases the flock, (now that the final file
    * has been renamed into place and the size has been added).
    */
   if (fd != -1)
      close(fd);
   free(filename_tmp);
   blob_finish(&cache_blob);
}

#endif

#if DETECT_OS_WINDOWS

bool
disk_cache_account_has_default_dir(const void *sid)
{
   static const SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
   static const SID_IDENTIFIER_AUTHORITY entra_authority = { { 0, 0, 0, 0, 0, 12 } };
   const SID *s = sid;

   if (!s || s->Revision != SID_REVISION || s->SubAuthorityCount < 1)
      return false;

   /* A local or domain account: S-1-5-21-... */
   if (memcmp(&s->IdentifierAuthority, &nt_authority, sizeof(nt_authority)) == 0)
      return s->SubAuthority[0] == SECURITY_NT_NON_UNIQUE;

   /* A Microsoft Entra ID account: S-1-12-1-... */
   if (memcmp(&s->IdentifierAuthority, &entra_authority, sizeof(entra_authority)) == 0)
      return s->SubAuthority[0] == 1;

   return false;
}

/* Whether this process gets a cache directory without being given one: only
 * when it runs as a user account, which owns a profile and with it the
 * %LOCALAPPDATA% in its environment, in any session (session 0 included, as
 * for a program started over SSH) and elevated or not.
 *
 * Other accounts load the driver too: the desktop window manager
 * (Window Manager\DWM-n, S-1-5-90-0-n), the user-mode font driver host
 * (Font Driver Host\UMFD-n, S-1-5-96-0-n), LocalSystem, LocalService and
 * NetworkService (S-1-5-18, -19 and -20), and service and application pool
 * identities (S-1-5-80-..., S-1-5-82-...). They are not a person's
 * accounts: a default directory for one of them would hold a cache shared by
 * unrelated system processes, in a profile that is the system's or none at
 * all, and the desktop compositor would write to it. They run without a disk
 * cache unless MESA_SHADER_CACHE_DIR or XDG_CACHE_HOME names one.
 */
static bool
process_has_default_cache_dir(void)
{
   union {
      TOKEN_USER user;
      uint8_t data[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
   } info;
   DWORD size;

   return GetTokenInformation(GetCurrentProcessToken(), TokenUser, &info, sizeof(info), &size) &&
          disk_cache_account_has_default_dir(info.user.User.Sid);
}

/* Determine path for cache based on the first defined name as follows:
 *
 *   $MESA_SHADER_CACHE_DIR/mesa_shader_cache
 *   $XDG_CACHE_HOME/mesa_shader_cache
 *   %LOCALAPPDATA%/mesa_shader_cache, for a user account only
 *
 * If none applies, the cache stays off. Only the multi-file cache has a
 * Windows implementation; disk_cache_type_create() asks for no other type.
 *
 * If the mkdir param is set we create the directory if it doesn't already
 * exist, if it does not exist and the param is false NULL will be returned.
 */
const char *
disk_cache_generate_cache_dir(void *mem_ctx, const char *gpu_name,
                              const char *driver_id,
                              const char *cache_dir_name_custom,
                              enum disk_cache_type cache_type,
                              bool mkdir)
{
   if (cache_type != DISK_CACHE_MULTI_FILE)
      return NULL;

   const char *cache_dir_name = cache_dir_name_custom ? cache_dir_name_custom : CACHE_DIR_NAME;

   const char *path = get_env_utf8(mem_ctx, L"MESA_SHADER_CACHE_DIR");

   if (!path) {
      path = get_env_utf8(mem_ctx, L"MESA_GLSL_CACHE_DIR");
      if (path)
         fprintf(stderr,
                 "*** MESA_GLSL_CACHE_DIR is deprecated; "
                 "use MESA_SHADER_CACHE_DIR instead ***\n");
   }

   if (!path)
      path = get_env_utf8(mem_ctx, L"XDG_CACHE_HOME");

   if (!path && process_has_default_cache_dir())
      path = get_env_utf8(mem_ctx, L"LOCALAPPDATA");

   if (!path)
      return NULL;

   return concatenate_and_mkdir(mem_ctx, path, cache_dir_name, mkdir);
}

#else

/* Determine path for cache based on the first defined name as follows:
 *
 *   $MESA_SHADER_CACHE_DIR/mesa_shader_cache*
 *   $XDG_CACHE_HOME/mesa_shader_cache*
 *   $HOME/.cache/mesa_shader_cache*
 *   <pwd.pw_dir>/.cache/mesa_shader_cache*
 *
 * The directory 'mesa_shader_cache*' is named depending of cache type:
 *  - For DISK_CACHE_MULTI_FILE: mesa_shader_cache
 *  - For DISK_CACHE_SINGLE_FILE: mesa_shader_cache_sf
 *  - For DISK_CACHE_DATABASE: mesa_shader_cache_db
 *
 * If the mkdir param is set we create the directory if it doesn't already
 * exist, if it does not exist and the param is false NULL will be returned.
 */
const char *
disk_cache_generate_cache_dir(void *mem_ctx, const char *gpu_name,
                              const char *driver_id,
                              const char *cache_dir_name_custom,
                              enum disk_cache_type cache_type,
                              bool mkdir)
{

   char *cache_dir_name;

   if (cache_dir_name_custom) {
      cache_dir_name = (char *)cache_dir_name_custom;
   } else {
      cache_dir_name = CACHE_DIR_NAME;
      if (cache_type == DISK_CACHE_SINGLE_FILE)
         cache_dir_name = CACHE_DIR_NAME_SF;
      else if (cache_type == DISK_CACHE_DATABASE)
         cache_dir_name = CACHE_DIR_NAME_DB;
   }

   const char *path = os_get_option_secure("MESA_SHADER_CACHE_DIR");

   if (!path) {
      path = os_get_option_secure("MESA_GLSL_CACHE_DIR");
      if (path)
         fprintf(stderr,
                 "*** MESA_GLSL_CACHE_DIR is deprecated; "
                 "use MESA_SHADER_CACHE_DIR instead ***\n");
   }

   if (path) {
      path = concatenate_and_mkdir(mem_ctx, path, cache_dir_name, mkdir);
      if (!path)
         return NULL;
   }

   if (path == NULL) {
      const char *xdg_cache_home = os_get_option_secure("XDG_CACHE_HOME");

      if (xdg_cache_home) {
         path = concatenate_and_mkdir(mem_ctx, xdg_cache_home, cache_dir_name,
                                      mkdir);
         if (!path)
            return NULL;
      }
   }

   if (!path) {
      const char *home = os_get_option("HOME");

      if (home) {
         path = concatenate_and_mkdir(mem_ctx, home, ".cache", mkdir);
         if (!path)
            return NULL;

         path = concatenate_and_mkdir(mem_ctx, path, cache_dir_name, mkdir);
         if (!path)
            return NULL;
      }
   }

   if (!path) {
      char *buf;
      size_t buf_size;
      struct passwd pwd, *result;

      buf_size = sysconf(_SC_GETPW_R_SIZE_MAX);
      if (buf_size == -1)
         buf_size = 512;

      /* Loop until buf_size is large enough to query the directory */
      while (1) {
         buf = ralloc_size(mem_ctx, buf_size);

         getpwuid_r(getuid(), &pwd, buf, buf_size, &result);
         if (result)
            break;

         if (errno == ERANGE) {
            ralloc_free(buf);
            buf = NULL;
            buf_size *= 2;
         } else {
            return NULL;
         }
      }

      path = concatenate_and_mkdir(mem_ctx, pwd.pw_dir, ".cache", mkdir);
      if (!path)
         return NULL;

      path = concatenate_and_mkdir(mem_ctx, path, cache_dir_name, mkdir);
      if (!path)
         return NULL;
   }

   if (cache_type == DISK_CACHE_SINGLE_FILE) {
      path = concatenate_and_mkdir(mem_ctx, path, driver_id, mkdir);
      if (!path)
         return NULL;

      path = concatenate_and_mkdir(mem_ctx, path, gpu_name, mkdir);
      if (!path)
         return NULL;
   }

   return path;
}

#endif

bool
disk_cache_enabled()
{
   /* If running as a users other than the real user disable cache */
   if (!__normal_user())
      return false;

   /* At user request, disable shader cache entirely.
    * Disk cache is not enabled by default for android, for most
    * applications the EGL layer uses EGL_ANDROID_blob_cache to manage
    * the cache itself, however those that wish to use the cache directly
    * can set `mesa.shader.cache.disable=false` property.
    * Don't forget to also set the shader cache path to something readable
    * and writable by the application via `mesa.shader.cache.dir`.
    */
#if defined(SHADER_CACHE_DISABLE_BY_DEFAULT) || DETECT_OS_ANDROID
   bool disable_by_default = true;
#else
   bool disable_by_default = false;
#endif
   char *envvar_name = "MESA_SHADER_CACHE_DISABLE";
#if !DETECT_OS_ANDROID
   if (!os_get_option(envvar_name)) {
      envvar_name = "MESA_GLSL_CACHE_DISABLE";
      if (os_get_option(envvar_name))
         fprintf(stderr,
                 "*** MESA_GLSL_CACHE_DISABLE is deprecated; "
                 "use MESA_SHADER_CACHE_DISABLE instead ***\n");
   }
#endif

   if (debug_get_bool_option(envvar_name, disable_by_default) ||
       /* MESA_GLSL_DISABLE_IO_OPT must disable the cache to get expected
        * results because it only takes effect on a cache miss. */
       debug_get_bool_option("MESA_GLSL_DISABLE_IO_OPT", false))
      return false;

   return true;
}

void *
disk_cache_load_item_foz(struct disk_cache *cache, const cache_key key,
                         size_t *size)
{
   size_t cache_tem_size = 0;
   void *cache_item = foz_read_entry(&cache->foz_db, key, &cache_tem_size);
   if (!cache_item)
      return NULL;

   uint8_t *uncompressed_data =
       parse_and_validate_cache_item(cache, cache_item, cache_tem_size, size);
   free(cache_item);

   return uncompressed_data;
}

bool
disk_cache_write_item_to_disk_foz(struct disk_cache_put_job *dc_job)
{
   struct blob cache_blob;
   blob_init(&cache_blob);

   if (!create_cache_item_header_and_blob(dc_job, &cache_blob))
      return false;

   bool r = foz_write_entry(&dc_job->cache->foz_db, dc_job->key,
                            cache_blob.data, cache_blob.size);

   blob_finish(&cache_blob);
   return r;
}

bool
disk_cache_load_cache_index_foz(void *mem_ctx, struct disk_cache *cache)
{
   /* Load cache index into a hash map (from fossilise files) */
   return foz_prepare(&cache->foz_db, cache->path);
}

#if DETECT_OS_WINDOWS

void
disk_cache_touch_cache_user_marker(char *path)
{
   char *marker_path = NULL;
   UNUSED int _unused = asprintf(&marker_path, "%s/marker", path);
   if (!marker_path)
      return;

   wchar_t *wmarker_path = utf8_to_wide(marker_path);
   free(marker_path);
   if (!wmarker_path)
      return;

   HANDLE file = CreateFileW(wmarker_path, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
   free(wmarker_path);
   if (file == INVALID_HANDLE_VALUE)
      return;

   /* An existing marker gets the current time once a day, as utime() does. */
   FILE_BASIC_INFO info;
   if (existed && GetFileInformationByHandleEx(file, FileBasicInfo, &info, sizeof(info))) {
      FILETIME now;
      GetSystemTimeAsFileTime(&now);
      int64_t now_ticks = ((int64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;
      if (now_ticks - info.LastWriteTime.QuadPart > 24ll * 60 * 60 * 10000000 /* One day */) {
         FILE_BASIC_INFO touch = { 0 }; /* zero leaves a field as it is */
         touch.LastAccessTime.QuadPart = now_ticks;
         touch.LastWriteTime.QuadPart = now_ticks;
         SetFileInformationByHandle(file, FileBasicInfo, &touch, sizeof(touch));
      }
   }
   CloseHandle(file);
}

bool
disk_cache_mmap_cache_index(void *mem_ctx, struct disk_cache *cache)
{
   HANDLE file = INVALID_HANDLE_VALUE, mapping = NULL;
   wchar_t *wpath = NULL;
   bool mapped = false;

   char *path = ralloc_asprintf(mem_ctx, "%s/index", cache->path);
   if (path == NULL)
      goto path_fail;

   wpath = utf8_to_wide(path);
   if (wpath == NULL)
      goto path_fail;

   file = CreateFileW(wpath, GENERIC_READ | GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   if (file == INVALID_HANDLE_VALUE)
      goto path_fail;

   /* Force the index file to be the expected size. Extending the file
    * allocates its clusters, so a full disk fails here rather than on a
    * write through the mapping.
    */
   size_t size = sizeof(*cache->size) + CACHE_INDEX_MAX_KEYS * CACHE_KEY_SIZE;
   LARGE_INTEGER current;
   if (!GetFileSizeEx(file, &current))
      goto path_fail;

   if ((uint64_t)current.QuadPart != size) {
      FILE_END_OF_FILE_INFO end_of_file = { .EndOfFile.QuadPart = size };
      if (!SetFileInformationByHandle(file, FileEndOfFileInfo, &end_of_file,
                                      sizeof(end_of_file)))
         goto path_fail;
   }

   /* We map this shared so that other processes see updates that we
    * make. The cache size is updated with atomic additions, as elsewhere.
    */
   mapping = CreateFileMappingW(file, NULL, PAGE_READWRITE, 0, 0, NULL);
   if (mapping == NULL)
      goto path_fail;

   cache->index_mmap = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size);
   if (cache->index_mmap == NULL)
      goto path_fail;
   cache->index_mmap_size = size;

   cache->size = (p_atomic_uint64_t *) cache->index_mmap;
   cache->stored_keys = cache->index_mmap + sizeof(uint64_t);
   mapped = true;

path_fail:
   if (mapping != NULL)
      CloseHandle(mapping);
   if (file != INVALID_HANDLE_VALUE)
      CloseHandle(file);
   free(wpath);

   return mapped;
}

void
disk_cache_destroy_mmap(struct disk_cache *cache)
{
   UnmapViewOfFile(cache->index_mmap);
}

#else

void
disk_cache_touch_cache_user_marker(char *path)
{
   char *marker_path = NULL;
   UNUSED int _unused = asprintf(&marker_path, "%s/marker", path);
   if (!marker_path)
      return;

   time_t now = time(NULL);

   struct stat attr;
   if (stat(marker_path, &attr) == -1) {
      int fd = open(marker_path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
      if (fd != -1) {
         close(fd);
      }
   } else if (now - attr.st_mtime > 60 * 60 * 24 /* One day */) {
      (void)utime(marker_path, NULL);
   }
   free(marker_path);
}

bool
disk_cache_mmap_cache_index(void *mem_ctx, struct disk_cache *cache)
{
   int fd = -1;
   bool mapped = false;

   char *path = ralloc_asprintf(mem_ctx, "%s/index", cache->path);
   if (path == NULL)
      goto path_fail;

   fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
   if (fd == -1)
      goto path_fail;

   struct stat sb;
   if (fstat(fd, &sb) == -1)
      goto path_fail;

   /* Force the index file to be the expected size. */
   size_t size = sizeof(*cache->size) + CACHE_INDEX_MAX_KEYS * CACHE_KEY_SIZE;
   if (sb.st_size != size) {
#if HAVE_POSIX_FALLOCATE
      /* posix_fallocate() ensures disk space is allocated otherwise it
       * fails if there is not enough space on the disk.
       */
      int ret = posix_fallocate(fd, 0, size);
      if (ret != 0) {
         if (ret == EOPNOTSUPP) {
            if (ftruncate(fd, size) == -1)
               goto path_fail;
         } else {
            goto path_fail;
         }
      }
#else
      /* ftruncate() allocates disk space lazily. If the disk is full
       * and it is unable to allocate disk space when accessed via
       * mmap, it will crash with a SIGBUS.
       */
      if (ftruncate(fd, size) == -1)
         goto path_fail;
#endif
   }

   /* We map this shared so that other processes see updates that we
    * make.
    *
    * Note: We do use atomic addition to ensure that multiple
    * processes don't scramble the cache size recorded in the
    * index. But we don't use any locking to prevent multiple
    * processes from updating the same entry simultaneously. The idea
    * is that if either result lands entirely in the index, then
    * that's equivalent to a well-ordered write followed by an
    * eviction and a write. On the other hand, if the simultaneous
    * writes result in a corrupt entry, that's not really any
    * different than both entries being evicted, (since within the
    * guarantees of the cryptographic hash, a corrupt entry is
    * unlikely to ever match a real cache key).
    */
   cache->index_mmap = mmap(NULL, size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
   if (cache->index_mmap == MAP_FAILED)
      goto path_fail;
   cache->index_mmap_size = size;

   cache->size = (p_atomic_uint64_t *) cache->index_mmap;
   cache->stored_keys = cache->index_mmap + sizeof(uint64_t);
   mapped = true;

path_fail:
   if (fd != -1)
      close(fd);

   return mapped;
}

void
disk_cache_destroy_mmap(struct disk_cache *cache)
{
   munmap(cache->index_mmap, cache->index_mmap_size);
}

#endif

void *
disk_cache_db_load_item(struct disk_cache *cache, const cache_key key,
                        size_t *size)
{
   size_t cache_tem_size = 0;
   void *cache_item = mesa_cache_db_multipart_read_entry(&cache->cache_db,
                                                         key, &cache_tem_size);
   if (!cache_item)
      return NULL;

   uint8_t *uncompressed_data =
       parse_and_validate_cache_item(cache, cache_item, cache_tem_size, size);
   free(cache_item);

   return uncompressed_data;
}

bool
disk_cache_db_write_item_to_disk(struct disk_cache_put_job *dc_job)
{
   struct blob cache_blob;
   blob_init(&cache_blob);

   if (!create_cache_item_header_and_blob(dc_job, &cache_blob))
      return false;

   bool r = mesa_cache_db_multipart_entry_write(&dc_job->cache->cache_db,
                                                dc_job->key, cache_blob.data,
                                                cache_blob.size);

   blob_finish(&cache_blob);
   return r;
}

bool
disk_cache_db_load_cache_index(void *mem_ctx, struct disk_cache *cache)
{
   return mesa_cache_db_multipart_open(&cache->cache_db, cache->path);
}

#if DETECT_OS_WINDOWS

/* Deletes old multi-file caches, to avoid having two default caches taking up
 * disk space. The database cache that would replace it has no Windows
 * implementation, so the multi-file cache is the only one and stays.
 */
void
disk_cache_delete_old_cache(void)
{
}

#else

static void
delete_dir(const char* path)
{
   DIR *dir = opendir(path);
   if (!dir)
      return;

   struct dirent *p;
   char *entry_path = NULL;

   while ((p = readdir(dir)) != NULL) {
      if (strcmp(p->d_name, ".") == 0 || strcmp(p->d_name, "..") == 0)
         continue;

      UNUSED int _unused = asprintf(&entry_path, "%s/%s", path, p->d_name);
      if (!entry_path)
         continue;

      struct stat st;
      if (stat(entry_path, &st)) {
         free(entry_path);
         continue;
      }
      if (S_ISDIR(st.st_mode))
         delete_dir(entry_path);
      else
         unlink(entry_path);

      free(entry_path);
   }
   closedir(dir);
   rmdir(path);
}

/* Deletes old multi-file caches, to avoid having two default caches taking up disk space. */
void
disk_cache_delete_old_cache(void)
{
   void *ctx = ralloc_context(NULL);
   const char *dirname = disk_cache_generate_cache_dir(ctx, NULL, NULL, NULL,
                                                       DISK_CACHE_MULTI_FILE, false);
   if (!dirname)
      goto finish;

   /* The directory itself doesn't get updated, so use a marker timestamp */
   char *index_path = ralloc_asprintf(ctx, "%s/marker", dirname);

   struct stat attr;
   if (stat(index_path, &attr) == -1)
      goto finish;

   time_t now = time(NULL);

   /* Do not delete anything if the cache has been modified in the past week */
   if (now - attr.st_mtime < 60 * 60 * 24 * 7)
      goto finish;

   delete_dir(dirname);

finish:
   ralloc_free(ctx);
}
#endif

#endif /* ENABLE_SHADER_CACHE */
