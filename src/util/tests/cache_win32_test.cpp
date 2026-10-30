/* SPDX-License-Identifier: MIT
 *
 * Tests of the Windows implementation of the disk cache (disk_cache_os.c and
 * the single-file port in fossilize_db.c), for what cache_test.cpp does not
 * reach there or reaches only within one process.
 *
 * The multi-file cache (the lab flavour's choice under
 * MESA_DISK_CACHE_MULTI_FILE=true, which the CacheWin32 fixture sets):
 *
 * - two cache instances share entries and the size in the index;
 * - the size in the index equals the entries on the disk after writes,
 *   removals and evictions, and eviction holds the cache to
 *   MESA_SHADER_CACHE_MAX_SIZE;
 * - several processes put the same entries at once: each entry is written
 *   once, reads back whole and is counted once, and no temporary file stays;
 * - the temporary file of an entry is its write lock while a writer holds it,
 *   and no lock once the writer is gone;
 * - an entry that does not read back is deleted, so the next put writes it;
 * - a directory without write access leaves the cache off;
 * - a cache directory whose name the ANSI code page cannot hold;
 * - the default directory: %LOCALAPPDATA% for a user account only;
 * - the module identity: one for copies of a build, another for a rebuild,
 *   and the loaded build's own when the file at its path is replaced;
 * - the writer threads run at the lowest priority.
 *
 * The single-file cache, the type the driver chooses itself (amdgpu-wddm
 * BD-216, CacheWin32SingleFile):
 *
 * - it is the type without a variable, in mesa_shader_cache_sf/<gpu name>,
 *   also in a directory whose name the ANSI code page cannot hold;
 * - a cache of the database type, such as a driver's custom cache, is a
 *   single-file cache in its own directory;
 * - any number of puts and gets opens its two files once per instance, where
 *   the multi-file cache opens files for each entry: an antivirus product's
 *   real-time scanner scans a file at each open;
 * - several processes put the same entries at once into a database that
 *   another process holds open: each entry is written once, and the open
 *   instance reads them all;
 * - threads of one instance put and get at once;
 * - the size bound: no entry past MESA_SHADER_CACHE_MAX_SIZE, and a full
 *   database emptied at the next start when no other instance has it open.
 *
 * Every test works in a directory of its own under the current directory.
 */

#include <gtest/gtest.h>

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "util/disk_cache.h"
#include "util/disk_cache_os.h"
#include "util/mesa-blake3.h"
#include "util/os_misc.h"

namespace fs = std::filesystem;

namespace {

const char *const gpu_name = "win32_test";
const char *const driver_id = "make_check";

void
set_env(const wchar_t *name, const std::wstring &value)
{
   SetEnvironmentVariableW(name, value.c_str());
}

void
unset_env(const wchar_t *name)
{
   SetEnvironmentVariableW(name, NULL);
}

std::wstring
get_env(const wchar_t *name)
{
   std::vector<wchar_t> buf(32768);
   DWORD len = GetEnvironmentVariableW(name, buf.data(), (DWORD)buf.size());
   return len && len < buf.size() ? std::wstring(buf.data(), len) : std::wstring();
}

/* Incompressible data, the same for the same seed in every process. */
std::vector<uint8_t>
make_data(uint32_t seed, size_t size)
{
   std::vector<uint8_t> data(size);
   uint32_t x = seed * 2654435761u + 1;
   for (uint8_t &byte : data) {
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      byte = x >> 24;
   }
   return data;
}

void
put(struct disk_cache *cache, const std::vector<uint8_t> &data, cache_key key)
{
   disk_cache_compute_key(cache, data.data(), data.size(), key);
   disk_cache_put(cache, key, data.data(), data.size(), NULL);
}

::testing::AssertionResult
holds(struct disk_cache *cache, const cache_key key, const std::vector<uint8_t> &data)
{
   size_t size = 0;
   void *result = disk_cache_get(cache, key, &size);
   if (!result)
      return ::testing::AssertionFailure() << "entry missing";

   bool same = size == data.size() && memcmp(result, data.data(), size) == 0;
   free(result);
   if (!same)
      return ::testing::AssertionFailure() << "entry differs (" << size << " bytes)";

   return ::testing::AssertionSuccess();
}

/* The size the index counts for an entry file of this length. */
uint64_t
counted_size(uint64_t length)
{
   return (length + 4095) & ~(uint64_t)4095;
}

struct dir_stats {
   unsigned entries = 0;
   unsigned tmp_files = 0;
   uint64_t size = 0;
};

dir_stats
scan(const fs::path &cache_dir)
{
   dir_stats stats;
   std::error_code ec;
   for (const fs::directory_entry &sub : fs::directory_iterator(cache_dir, ec)) {
      if (!sub.is_directory(ec) || sub.path().filename().wstring().size() != 2)
         continue;

      for (const fs::directory_entry &file : fs::directory_iterator(sub.path(), ec)) {
         if (file.path().extension() == L".tmp") {
            stats.tmp_files++;
         } else {
            stats.entries++;
            stats.size += counted_size(file.file_size(ec));
         }
      }
   }
   return stats;
}

fs::path
entry_path(struct disk_cache *cache, const cache_key key)
{
   char *filename = disk_cache_get_cache_filename(cache, key);
   fs::path path = filename ? fs::u8path(filename) : fs::path();
   free(filename);
   return path;
}

std::wstring
current_user_sid()
{
   union {
      TOKEN_USER user;
      uint8_t data[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
   } info;
   DWORD size;
   wchar_t *sid = NULL;
   if (!GetTokenInformation(GetCurrentProcessToken(), TokenUser, &info, sizeof(info), &size) ||
       !ConvertSidToStringSidW(info.user.User.Sid, &sid))
      return std::wstring();

   std::wstring result(sid);
   LocalFree(sid);
   return result;
}

/* Gives the directory, and through inheritance everything in it, a protected
 * DACL that grants the current user these rights and nobody anything else.
 */
bool
set_dacl(const fs::path &path, const wchar_t *rights)
{
   std::wstring sddl = std::wstring(L"D:P(A;OICI;") + rights + L";;;" + current_user_sid() + L")";
   PSECURITY_DESCRIPTOR sd = NULL;
   if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, NULL))
      return false;

   BOOL present = FALSE, defaulted = FALSE;
   PACL dacl = NULL;
   DWORD err = ERROR_INVALID_SECURITY_DESCR;
   if (GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present) {
      err = SetNamedSecurityInfoW((LPWSTR)path.c_str(), SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                  NULL, NULL, dacl, NULL);
   }
   LocalFree(sd);
   return err == ERROR_SUCCESS;
}

class CacheWin32 : public ::testing::Test {
protected:
   fs::path dir;

   void SetUp() override
   {
      dir = fs::current_path() / "cache-win32-tmp" /
            ::testing::UnitTest::GetInstance()->current_test_info()->name();
      std::error_code ec;
      fs::remove_all(dir, ec);
      fs::create_directories(dir, ec);
      ASSERT_FALSE(ec) << dir << ": " << ec.message();

      for (const char *name : { "MESA_SHADER_CACHE_DISABLE", "MESA_SHADER_CACHE_MAX_SIZE",
                                "MESA_DISK_CACHE_SINGLE_FILE", "MESA_DISK_CACHE_DATABASE",
                                "MESA_DISK_CACHE_MULTI_FILE", "MESA_DISK_CACHE_COMBINE_RW_WITH_RO_FOZ",
                                "MESA_GLSL_CACHE_DIR", "XDG_CACHE_HOME" })
         os_unset_option(name);
      set_env(L"MESA_SHADER_CACHE_DIR", dir.wstring());
      /* The driver chooses the single-file cache; these tests are of the
       * multi-file one, which the lab flavour gives on this variable.
       */
      os_set_option("MESA_DISK_CACHE_MULTI_FILE", "true", true);
   }

   void TearDown() override
   {
      os_unset_option("MESA_SHADER_CACHE_DIR");
      os_unset_option("MESA_DISK_CACHE_MULTI_FILE");
      if (!HasFailure()) {
         std::error_code ec;
         fs::remove_all(dir, ec);
      }
   }

   fs::path cache_dir() const
   {
      return dir / CACHE_DIR_NAME;
   }
};

/* The single-file cache: the type without MESA_DISK_CACHE_MULTI_FILE. */
class CacheWin32SingleFile : public CacheWin32 {
protected:
   void SetUp() override
   {
      CacheWin32::SetUp();
      os_unset_option("MESA_DISK_CACHE_MULTI_FILE");
   }

   fs::path sf_dir(const char *name = gpu_name) const
   {
      return dir / CACHE_DIR_NAME_SF / name;
   }
};

/* The size of a single-file database's files: the magic and version at the
 * start, and for each entry its hash in hex, a payload header and, in the
 * index, the entry's offset (fossilize_db.c).
 */
const uint64_t foz_magic_size = 16;
const uint64_t foz_index_record_size = FOSSILIZE_BLOB_HASH_LENGTH + sizeof(struct foz_payload_header) + 8;

uint64_t
size_on_disk(const fs::path &path)
{
   std::error_code ec;
   uint64_t size = fs::file_size(path, ec);
   return ec ? 0 : size;
}

/* Counts the files this process opens under one directory: the CreateFileW
 * and CreateFile2 calls of this executable for a path there. Mesa's util
 * library and the static CRT are linked into the executable, so their calls go
 * through its import address table, which the counter patches once.
 */
namespace open_counter {

std::atomic<unsigned> opens { 0 };
std::mutex dir_mutex;
std::wstring dir; /* lower case, with backslashes */
std::vector<std::wstring> first_paths; /* the first opens, for a failure message */

decltype(&CreateFileW) real_create_file_w;
decltype(&CreateFile2) real_create_file_2;

std::wstring
normalized(std::wstring path)
{
   for (wchar_t &c : path)
      c = c == L'/' ? L'\\' : (wchar_t)std::towlower(c);
   return path;
}

void
count(const wchar_t *name)
{
   if (!name)
      return;

   std::lock_guard<std::mutex> lock(dir_mutex);
   if (!dir.empty() && normalized(name).find(dir) != std::wstring::npos) {
      if (opens++ < 8)
         first_paths.push_back(name);
   }
}

/* The first opens since start(), one per line. */
std::string
paths()
{
   std::lock_guard<std::mutex> lock(dir_mutex);
   std::string text;
   for (const std::wstring &path : first_paths)
      text += fs::path(path).u8string() + "\n";
   return text;
}

HANDLE WINAPI
create_file_w(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition,
              DWORD flags, HANDLE template_file)
{
   count(name);
   return real_create_file_w(name, access, share, security, disposition, flags, template_file);
}

HANDLE WINAPI
create_file_2(LPCWSTR name, DWORD access, DWORD share, DWORD disposition,
              LPCREATEFILE2_EXTENDED_PARAMETERS parameters)
{
   count(name);
   return real_create_file_2(name, access, share, disposition, parameters);
}

/* Points each import of this executable that resolves to one of targets at
 * hook. Returns the number of import slots changed.
 */
unsigned
patch(const std::vector<void *> &targets, void *hook)
{
   uint8_t *base = (uint8_t *)GetModuleHandleW(NULL);
   const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
   const IMAGE_DATA_DIRECTORY &imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
   if (!imports.VirtualAddress)
      return 0;

   unsigned patched = 0;
   for (const IMAGE_IMPORT_DESCRIPTOR *module = (const IMAGE_IMPORT_DESCRIPTOR *)(base + imports.VirtualAddress);
        module->Name; module++) {
      for (IMAGE_THUNK_DATA *slot = (IMAGE_THUNK_DATA *)(base + module->FirstThunk); slot->u1.Function; slot++) {
         if (std::find(targets.begin(), targets.end(), (void *)slot->u1.Function) == targets.end())
            continue;

         DWORD protection;
         if (!VirtualProtect(&slot->u1.Function, sizeof(slot->u1.Function), PAGE_READWRITE, &protection))
            continue;
         slot->u1.Function = (ULONG_PTR)hook;
         VirtualProtect(&slot->u1.Function, sizeof(slot->u1.Function), protection, &protection);
         patched++;
      }
   }
   return patched;
}

/* Returns false if no import was found, so that the counter would see nothing. */
bool
install()
{
   static std::once_flag once;
   static bool installed = false;

   std::call_once(once, [] {
      std::vector<void *> create_file_w_targets, create_file_2_targets;
      for (const wchar_t *module : { L"kernel32.dll", L"kernelbase.dll" }) {
         HMODULE handle = GetModuleHandleW(module);
         if (!handle)
            continue;
         create_file_w_targets.push_back((void *)GetProcAddress(handle, "CreateFileW"));
         create_file_2_targets.push_back((void *)GetProcAddress(handle, "CreateFile2"));
      }
      real_create_file_w = (decltype(&CreateFileW))create_file_w_targets.front();
      real_create_file_2 = (decltype(&CreateFile2))create_file_2_targets.front();

      unsigned patched = patch(create_file_w_targets, (void *)create_file_w);
      patched += patch(create_file_2_targets, (void *)create_file_2);
      installed = patched > 0;
   });
   return installed;
}

/* Counts from zero the opens under path. */
void
start(const fs::path &path)
{
   std::lock_guard<std::mutex> lock(dir_mutex);
   dir = normalized(fs::absolute(path).wstring());
   opens = 0;
   first_paths.clear();
}

unsigned
stop()
{
   std::lock_guard<std::mutex> lock(dir_mutex);
   dir.clear();
   return opens.load();
}

} /* namespace open_counter */

const unsigned num_shared = 200;
const unsigned num_unique = 50;

std::vector<uint8_t>
shared_data(unsigned i)
{
   return make_data(0x5000 + i, 1000 + (i * 37) % 7000);
}

std::vector<uint8_t>
unique_data(unsigned child, unsigned i)
{
   return make_data(0x10000 * (child + 1) + i, 1000 + (i * 53) % 7000);
}

/* A module's identity, as RADV hashes it into its cache UUID. */
std::array<uint8_t, BLAKE3_OUT_LEN>
identity_of(const void *address)
{
   blake3_hasher hasher;
   _mesa_blake3_init(&hasher);
   EXPECT_TRUE(disk_cache_get_function_identifier((void *)address, &hasher));

   std::array<uint8_t, BLAKE3_OUT_LEN> id;
   _mesa_blake3_final(&hasher, id.data());
   return id;
}

/* Copies a PE file and changes in the copy what a rebuild changes: the link
 * time stamp, or the GUID of the PDB it names.
 */
bool
copy_as_rebuilt(const fs::path &from, const fs::path &to, bool pdb_guid)
{
   std::ifstream in(from, std::ios::binary);
   std::vector<char> image((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
   if (image.size() < sizeof(IMAGE_DOS_HEADER))
      return false;

   IMAGE_DOS_HEADER dos;
   memcpy(&dos, image.data(), sizeof(dos));
   IMAGE_NT_HEADERS nt;
   if ((size_t)dos.e_lfanew + sizeof(nt) > image.size())
      return false;
   memcpy(&nt, image.data() + dos.e_lfanew, sizeof(nt));

   if (!pdb_guid) {
      image[dos.e_lfanew + offsetof(IMAGE_NT_HEADERS, FileHeader.TimeDateStamp)] ^= 1;
   } else {
      /* The debug directory's RVA, to a file offset through the section table. */
      const IMAGE_DATA_DIRECTORY &debug = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
      size_t sections = dos.e_lfanew + offsetof(IMAGE_NT_HEADERS, OptionalHeader) +
                        nt.FileHeader.SizeOfOptionalHeader;
      size_t debug_offset = 0;
      for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; i++) {
         IMAGE_SECTION_HEADER section;
         memcpy(&section, image.data() + sections + i * sizeof(section), sizeof(section));
         if (debug.VirtualAddress >= section.VirtualAddress &&
             debug.VirtualAddress < section.VirtualAddress + section.SizeOfRawData)
            debug_offset = debug.VirtualAddress - section.VirtualAddress + section.PointerToRawData;
      }

      bool patched = false;
      for (unsigned i = 0; debug_offset && i < debug.Size / sizeof(IMAGE_DEBUG_DIRECTORY); i++) {
         IMAGE_DEBUG_DIRECTORY entry;
         memcpy(&entry, image.data() + debug_offset + i * sizeof(entry), sizeof(entry));
         if (entry.Type == IMAGE_DEBUG_TYPE_CODEVIEW &&
             memcmp(image.data() + entry.PointerToRawData, "RSDS", 4) == 0) {
            image[entry.PointerToRawData + 4] ^= 1;
            patched = true;
         }
      }
      if (!patched)
         return false;
   }

   std::ofstream out(to, std::ios::binary | std::ios::trunc);
   out.write(image.data(), image.size());
   return out.good();
}

} /* namespace */

TEST_F(CacheWin32, TwoInstancesShareEntriesAndSize)
{
   struct disk_cache *a = disk_cache_create(gpu_name, driver_id, 0);
   struct disk_cache *b = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_TRUE(a && !a->path_init_failed && b && !b->path_init_failed);
   EXPECT_EQ(a->max_size, 1024ull * 1024 * 1024) << "upstream's default limit";

   std::vector<uint8_t> data = make_data(1, 3000);
   cache_key key;
   put(a, data, key);
   disk_cache_wait_for_idle(a);

   EXPECT_TRUE(holds(b, key, data));
   EXPECT_GT(a->size->value, 0u);
   EXPECT_EQ(a->size->value, b->size->value);
   EXPECT_EQ(a->size->value, scan(cache_dir()).size);

   disk_cache_destroy(a);
   disk_cache_destroy(b);
}

TEST_F(CacheWin32, SizeMatchesEntries)
{
   struct disk_cache *cache = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_TRUE(cache && !cache->path_init_failed);

   std::vector<std::vector<uint8_t>> data;
   std::vector<std::array<uint8_t, CACHE_KEY_SIZE>> keys(20);
   for (unsigned i = 0; i < keys.size(); i++) {
      data.push_back(make_data(100 + i, 100 + i * 1000));
      put(cache, data[i], keys[i].data());
   }
   disk_cache_wait_for_idle(cache);

   dir_stats stats = scan(cache_dir());
   EXPECT_EQ(stats.entries, 20u);
   EXPECT_EQ(stats.tmp_files, 0u);
   EXPECT_EQ(cache->size->value, stats.size);

   for (unsigned i = 0; i < 5; i++)
      disk_cache_remove(cache, keys[i].data());

   stats = scan(cache_dir());
   EXPECT_EQ(stats.entries, 15u);
   EXPECT_EQ(cache->size->value, stats.size);

   /* Putting all of them again writes the five removed ones and skips the rest. */
   for (unsigned i = 0; i < keys.size(); i++)
      disk_cache_put(cache, keys[i].data(), data[i].data(), data[i].size(), NULL);
   disk_cache_wait_for_idle(cache);

   stats = scan(cache_dir());
   EXPECT_EQ(stats.entries, 20u);
   EXPECT_EQ(stats.tmp_files, 0u);
   EXPECT_EQ(cache->size->value, stats.size);
   for (unsigned i = 0; i < keys.size(); i++)
      EXPECT_TRUE(holds(cache, keys[i].data(), data[i])) << "entry " << i;

   disk_cache_destroy(cache);
}

TEST_F(CacheWin32, EvictionHoldsMaxSize)
{
   os_set_option("MESA_SHADER_CACHE_MAX_SIZE", "256K", true);
   struct disk_cache *cache = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_TRUE(cache && !cache->path_init_failed);
   EXPECT_EQ(cache->max_size, 256u * 1024);

   uint64_t largest = 0;
   std::vector<uint8_t> data;
   cache_key key;
   for (unsigned i = 0; i < 200; i++) {
      data = make_data(1000 + i, 6000);
      put(cache, data, key);
      disk_cache_wait_for_idle(cache);
      largest = std::max<uint64_t>(largest, cache->size->value);
   }

   dir_stats stats = scan(cache_dir());
   EXPECT_EQ(cache->size->value, stats.size);
   EXPECT_EQ(stats.tmp_files, 0u);
   EXPECT_LT(stats.entries, 200u);
   /* Evictions make room for an entry's data before it is compressed and
    * written, so the cache can pass the limit by what the entry takes on the
    * disk beyond that: here less than one 8 KiB entry.
    */
   EXPECT_LE(largest, cache->max_size + 8192);
   EXPECT_TRUE(holds(cache, key, data)) << "the last entry";

   disk_cache_destroy(cache);
   os_unset_option("MESA_SHADER_CACHE_MAX_SIZE");
}

const unsigned num_writers = 4;

/* Starts num_writers processes of CacheWin32Child.DISABLED_Writer, which open
 * the cache the environment gives, lets them put their entries at once and
 * waits until each has ended with success. Their output goes to log_dir.
 */
void
run_writers(const fs::path &log_dir)
{
   const unsigned num_children = num_writers;
   const std::wstring suffix = std::to_wstring(GetCurrentProcessId());
   const std::wstring ready_name = L"Local\\mesa-cache-win32-ready-" + suffix;
   const std::wstring start_name = L"Local\\mesa-cache-win32-start-" + suffix;
   HANDLE ready = CreateSemaphoreW(NULL, 0, num_children, ready_name.c_str());
   HANDLE start = CreateEventW(NULL, TRUE, FALSE, start_name.c_str());
   ASSERT_TRUE(ready && start);
   set_env(L"CACHE_WIN32_READY", ready_name);
   set_env(L"CACHE_WIN32_START", start_name);

   wchar_t exe[MAX_PATH];
   DWORD exe_len = GetModuleFileNameW(NULL, exe, MAX_PATH);
   ASSERT_TRUE(exe_len > 0 && exe_len < MAX_PATH);

   struct children {
      std::vector<HANDLE> processes;
      ~children()
      {
         for (HANDLE process : processes) {
            if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT)
               TerminateProcess(process, 1);
            CloseHandle(process);
         }
      }
   } children;

   for (unsigned i = 0; i < num_children; i++) {
      set_env(L"CACHE_WIN32_CHILD", std::to_wstring(i));

      SECURITY_ATTRIBUTES inherit = { sizeof(inherit), NULL, TRUE };
      fs::path log = log_dir / ("child-" + std::to_string(i) + ".log");
      HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
      ASSERT_NE(out, INVALID_HANDLE_VALUE);

      STARTUPINFOW si = {};
      si.cb = sizeof(si);
      si.dwFlags = STARTF_USESTDHANDLES;
      si.hStdOutput = out;
      si.hStdError = out;
      std::wstring cmd = L"\"" + std::wstring(exe) +
                         L"\" --gtest_filter=CacheWin32Child.DISABLED_Writer --gtest_also_run_disabled_tests";
      PROCESS_INFORMATION pi;
      BOOL created = CreateProcessW(exe, cmd.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
      CloseHandle(out);
      ASSERT_TRUE(created) << "CreateProcess: error " << GetLastError();

      CloseHandle(pi.hThread);
      children.processes.push_back(pi.hProcess);
   }
   unset_env(L"CACHE_WIN32_CHILD");

   for (unsigned i = 0; i < num_children; i++)
      ASSERT_EQ(WaitForSingleObject(ready, 60000), WAIT_OBJECT_0) << "a child did not open its cache";
   SetEvent(start);

   DWORD waited = WaitForMultipleObjects(num_children, children.processes.data(), TRUE, 120000);
   ASSERT_LT(waited, WAIT_OBJECT_0 + num_children) << "the children did not finish";

   for (unsigned i = 0; i < num_children; i++) {
      DWORD code = 1;
      GetExitCodeProcess(children.processes[i], &code);
      EXPECT_EQ(code, 0u) << "child " << i << ", output in " << (log_dir / ("child-" + std::to_string(i) + ".log"));
   }

   unset_env(L"CACHE_WIN32_READY");
   unset_env(L"CACHE_WIN32_START");
   CloseHandle(ready);
   CloseHandle(start);
}

TEST_F(CacheWin32, ProcessesWriteAtOnce)
{
   const unsigned num_children = num_writers;
   run_writers(dir);
   ASSERT_FALSE(HasFailure());

   struct disk_cache *cache = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_TRUE(cache && !cache->path_init_failed);

   cache_key key;
   for (unsigned i = 0; i < num_shared; i++) {
      std::vector<uint8_t> data = shared_data(i);
      disk_cache_compute_key(cache, data.data(), data.size(), key);
      EXPECT_TRUE(holds(cache, key, data)) << "shared entry " << i;
   }
   for (unsigned child = 0; child < num_children; child++) {
      for (unsigned i = 0; i < num_unique; i++) {
         std::vector<uint8_t> data = unique_data(child, i);
         disk_cache_compute_key(cache, data.data(), data.size(), key);
         EXPECT_TRUE(holds(cache, key, data)) << "child " << child << " entry " << i;
      }
   }

   dir_stats stats = scan(cache_dir());
   EXPECT_EQ(stats.entries, num_shared + num_children * num_unique);
   EXPECT_EQ(stats.tmp_files, 0u);
   EXPECT_EQ(cache->size->value, stats.size) << "each entry counted once";

   disk_cache_destroy(cache);
}

/* A writer process of ProcessesWriteAtOnce, which starts it with the cache
 * directory, its index and the names of a semaphore and an event in its
 * environment. Its own entries it checks itself; the shared ones may still be
 * in another writer's hands when it is done.
 */
TEST(CacheWin32Child, DISABLED_Writer)
{
   std::wstring child = get_env(L"CACHE_WIN32_CHILD");
   if (child.empty())
      GTEST_SKIP() << "started by CacheWin32.ProcessesWriteAtOnce";
   unsigned index = wcstoul(child.c_str(), NULL, 10);

   struct disk_cache *cache = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_TRUE(cache && !cache->path_init_failed);

   HANDLE ready = OpenSemaphoreW(SEMAPHORE_MODIFY_STATE, FALSE, get_env(L"CACHE_WIN32_READY").c_str());
   HANDLE start = OpenEventW(SYNCHRONIZE, FALSE, get_env(L"CACHE_WIN32_START").c_str());
   ASSERT_TRUE(ready && start);
   ReleaseSemaphore(ready, 1, NULL);
   ASSERT_EQ(WaitForSingleObject(start, 60000), WAIT_OBJECT_0);

   /* Half the writers take the shared entries backwards, to meet the others
    * in the middle as well as at the ends.
    */
   cache_key key;
   for (unsigned n = 0; n < num_shared; n++) {
      put(cache, shared_data(index % 2 ? num_shared - 1 - n : n), key);
      if (n < num_unique)
         put(cache, unique_data(index, n), key);
   }
   disk_cache_wait_for_idle(cache);

   for (unsigned i = 0; i < num_unique; i++) {
      std::vector<uint8_t> data = unique_data(index, i);
      disk_cache_compute_key(cache, data.data(), data.size(), key);
      EXPECT_TRUE(holds(cache, key, data)) << "entry " << i;
   }

   disk_cache_destroy(cache);
   CloseHandle(ready);
   CloseHandle(start);
}

TEST_F(CacheWin32, TemporaryFileIsTheWriteLock)
{
   struct disk_cache *cache = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_TRUE(cache && !cache->path_init_failed);

   std::vector<uint8_t> data = make_data(7, 5000);
   cache_key key;
   disk_cache_compute_key(cache, data.data(), data.size(), key);
   fs::path entry = entry_path(cache, key);
   fs::path tmp = entry;
   tmp += L".tmp";
   std::error_code ec;
   fs::create_directories(entry.parent_path(), ec);

   /* Another writer holds the temporary file: this one leaves the entry to it. */
   HANDLE held = CreateFileW(tmp.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
   ASSERT_NE(held, INVALID_HANDLE_VALUE);
   disk_cache_put(cache, key, data.data(), data.size(), NULL);
   disk_cache_wait_for_idle(cache);
   EXPECT_FALSE(fs::exists(entry, ec));
   EXPECT_EQ(cache->size->value, 0u);

   /* The writer died halfway: its temporary file stays and is no lock any more. */
   DWORD written;
   WriteFile(held, "partial", 7, &written, NULL);
   CloseHandle(held);
   disk_cache_put(cache, key, data.data(), data.size(), NULL);
   disk_cache_wait_for_idle(cache);

   EXPECT_TRUE(holds(cache, key, data));
   EXPECT_FALSE(fs::exists(tmp, ec));
   EXPECT_EQ(cache->size->value, scan(cache_dir()).size);

   disk_cache_destroy(cache);
}

TEST_F(CacheWin32, UnreadableEntryIsDeleted)
{
   struct disk_cache *cache = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_TRUE(cache && !cache->path_init_failed);

   std::vector<uint8_t> data = make_data(11, 5000);
   cache_key key;
   put(cache, data, key);
   disk_cache_wait_for_idle(cache);
   fs::path entry = entry_path(cache, key);
   ASSERT_TRUE(holds(cache, key, data));

   /* A damaged payload: its CRC no longer matches. */
   {
      std::fstream file(entry, std::ios::binary | std::ios::in | std::ios::out);
      file.seekg(-1, std::ios::end);
      char last = (char)file.get();
      file.seekp(-1, std::ios::end);
      file.put((char)(last ^ 0xff));
   }
   std::error_code ec;
   EXPECT_EQ(disk_cache_get(cache, key, NULL), nullptr);
   EXPECT_FALSE(fs::exists(entry, ec));
   EXPECT_EQ(cache->size->value, 0u);

   disk_cache_put(cache, key, data.data(), data.size(), NULL);
   disk_cache_wait_for_idle(cache);
   EXPECT_TRUE(holds(cache, key, data)) << "written again";
   EXPECT_EQ(cache->size->value, scan(cache_dir()).size);

   /* An entry cut to nothing goes as well. Its counted size, taken from the
    * length it has now, leaves the difference in the index.
    */
   fs::resize_file(entry, 0, ec);
   EXPECT_EQ(disk_cache_get(cache, key, NULL), nullptr);
   EXPECT_FALSE(fs::exists(entry, ec));

   disk_cache_put(cache, key, data.data(), data.size(), NULL);
   disk_cache_wait_for_idle(cache);
   EXPECT_TRUE(holds(cache, key, data)) << "written again";

   disk_cache_destroy(cache);
}

TEST_F(CacheWin32, DirectoryWithoutWriteAccessLeavesCacheOff)
{
   std::error_code ec;
   std::vector<uint8_t> data = make_data(13, 2000);
   cache_key key;

   /* The cache directory cannot be made. */
   fs::path read_only = dir / "read-only";
   fs::create_directories(read_only, ec);
   ASSERT_TRUE(set_dacl(read_only, L"FRFX"));
   set_env(L"MESA_SHADER_CACHE_DIR", read_only.wstring());

   struct disk_cache *cache = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_NE(cache, nullptr);
   EXPECT_TRUE(cache->path_init_failed);
   EXPECT_EQ(cache->type, DISK_CACHE_NONE);
   put(cache, data, key);
   disk_cache_wait_for_idle(cache);
   EXPECT_EQ(disk_cache_get(cache, key, NULL), nullptr);
   EXPECT_FALSE(fs::exists(read_only / CACHE_DIR_NAME, ec));
   disk_cache_destroy(cache);

   /* A cache that loses write access keeps its entries, unused: as on POSIX,
    * an index that does not open for writing leaves the cache off.
    */
   fs::path was_writable = dir / "was-writable";
   set_env(L"MESA_SHADER_CACHE_DIR", was_writable.wstring());
   cache = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_TRUE(cache && !cache->path_init_failed);
   put(cache, data, key);
   disk_cache_wait_for_idle(cache);
   ASSERT_TRUE(holds(cache, key, data));
   disk_cache_destroy(cache);

   ASSERT_TRUE(set_dacl(was_writable, L"FRFX"));
   cache = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_NE(cache, nullptr);
   EXPECT_TRUE(cache->path_init_failed);
   EXPECT_EQ(disk_cache_get(cache, key, NULL), nullptr);
   disk_cache_put(cache, key, data.data(), data.size(), NULL);
   disk_cache_wait_for_idle(cache);
   disk_cache_destroy(cache);

   EXPECT_TRUE(set_dacl(read_only, L"FA"));
   EXPECT_TRUE(set_dacl(was_writable, L"FA"));
}

TEST_F(CacheWin32, DirectoryNameOutsideAnsiCodePage)
{
   /* "zażółć-キャッシュ": the Polish letters fit code page 1250, the Japanese
    * ones no single-byte code page.
    */
   fs::path named = dir / L"zażółć-キャッシュ";
   set_env(L"MESA_SHADER_CACHE_DIR", named.wstring());

   struct disk_cache *cache = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_TRUE(cache && !cache->path_init_failed);

   std::vector<uint8_t> data = make_data(17, 2000);
   cache_key key;
   put(cache, data, key);
   disk_cache_wait_for_idle(cache);
   EXPECT_TRUE(holds(cache, key, data));

   std::error_code ec;
   EXPECT_TRUE(fs::is_directory(named / CACHE_DIR_NAME, ec));
   EXPECT_TRUE(fs::equivalent(fs::u8path(cache->path), named / CACHE_DIR_NAME, ec));
   EXPECT_EQ(scan(named / CACHE_DIR_NAME).entries, 1u);

   disk_cache_destroy(cache);
}

TEST_F(CacheWin32SingleFile, DatabaseTypeIsSingleFile)
{
   /* RADV keeps its built-in shaders in a custom cache of the database type. */
   struct disk_cache *custom = disk_cache_create_custom(gpu_name, driver_id, 0, "custom_cache", 64 * 1024);
   ASSERT_TRUE(custom && !custom->path_init_failed);
   EXPECT_EQ(custom->type, DISK_CACHE_SINGLE_FILE);
   EXPECT_EQ(custom->max_size, 64u * 1024);
   EXPECT_EQ(custom->foz_db.max_size, 64u * 1024) << "the bound of the database";

   std::error_code ec;
   EXPECT_TRUE(fs::equivalent(fs::u8path(custom->path), dir / "custom_cache", ec));

   std::vector<uint8_t> data = make_data(19, 3000);
   cache_key key;
   put(custom, data, key);
   disk_cache_wait_for_idle(custom);
   EXPECT_TRUE(holds(custom, key, data));
   EXPECT_EQ(size_on_disk(dir / "custom_cache" / "foz_cache_idx.foz"), foz_magic_size + foz_index_record_size);

   /* The variables of the other types choose nothing on Windows: the
    * database type asked for by name is the default single-file cache, a
    * cache of its own.
    */
   os_set_option("MESA_DISK_CACHE_DATABASE", "true", true);
   struct disk_cache *cache = disk_cache_create(gpu_name, driver_id, 0);
   os_unset_option("MESA_DISK_CACHE_DATABASE");
   ASSERT_TRUE(cache && !cache->path_init_failed);
   EXPECT_EQ(cache->type, DISK_CACHE_SINGLE_FILE);
   EXPECT_TRUE(fs::equivalent(fs::u8path(cache->path), sf_dir(), ec));
   EXPECT_EQ(disk_cache_get(cache, key, NULL), nullptr) << "a cache of its own";
   disk_cache_destroy(cache);

   os_set_option("MESA_DISK_CACHE_SINGLE_FILE", "false", true);
   cache = disk_cache_create(gpu_name, driver_id, 0);
   os_unset_option("MESA_DISK_CACHE_SINGLE_FILE");
   ASSERT_TRUE(cache && !cache->path_init_failed);
   EXPECT_EQ(cache->type, DISK_CACHE_SINGLE_FILE) << "MESA_DISK_CACHE_SINGLE_FILE=false";
   disk_cache_destroy(cache);

   disk_cache_destroy(custom);
}

TEST_F(CacheWin32SingleFile, IsTheDefault)
{
   struct disk_cache *cache = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_TRUE(cache && !cache->path_init_failed);
   EXPECT_EQ(cache->type, DISK_CACHE_SINGLE_FILE);
   EXPECT_EQ(cache->foz_db.max_size, 1024ull * 1024 * 1024) << "upstream's default limit, as the bound";

   std::error_code ec;
   EXPECT_TRUE(fs::equivalent(fs::u8path(cache->path), sf_dir(), ec)) << cache->path;
   EXPECT_TRUE(fs::is_regular_file(sf_dir() / "foz_cache.foz", ec));
   EXPECT_TRUE(fs::is_regular_file(sf_dir() / "foz_cache_idx.foz", ec));

   std::vector<uint8_t> data = make_data(21, 5000);
   cache_key key;
   put(cache, data, key);
   disk_cache_wait_for_idle(cache);
   EXPECT_TRUE(holds(cache, key, data));
   disk_cache_destroy(cache);

   /* No multi-file cache beside it. */
   EXPECT_FALSE(fs::exists(dir / CACHE_DIR_NAME, ec));

   /* The GPU name is a directory name: what a file name cannot hold becomes '_'. */
   cache = disk_cache_create("AMD Radeon (TM) Graphics/x:1", driver_id, 0);
   ASSERT_TRUE(cache && !cache->path_init_failed);
   EXPECT_TRUE(fs::equivalent(fs::u8path(cache->path), sf_dir("AMD_Radeon__TM__Graphics_x_1"), ec))
      << cache->path;
   disk_cache_destroy(cache);

   /* The default directory, as for the multi-file cache. */
   const std::wstring saved = get_env(L"LOCALAPPDATA");
   fs::path local_app_data = dir / "LocalAppData";
   fs::create_directories(local_app_data, ec);
   unset_env(L"MESA_SHADER_CACHE_DIR");
   set_env(L"LOCALAPPDATA", local_app_data.wstring());
   cache = disk_cache_create(gpu_name, driver_id, 0);
   if (!saved.empty())
      set_env(L"LOCALAPPDATA", saved);
   else
      unset_env(L"LOCALAPPDATA");
   ASSERT_TRUE(cache && !cache->path_init_failed);
   EXPECT_TRUE(fs::equivalent(fs::u8path(cache->path), local_app_data / CACHE_DIR_NAME_SF / gpu_name, ec))
      << cache->path;
   disk_cache_destroy(cache);

   /* A directory name that no single-byte code page holds. */
   fs::path named = dir / L"シェーダー-šéδ";
   set_env(L"MESA_SHADER_CACHE_DIR", named.wstring());
   cache = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_TRUE(cache && !cache->path_init_failed);
   put(cache, data, key);
   disk_cache_wait_for_idle(cache);
   EXPECT_TRUE(holds(cache, key, data));
   disk_cache_destroy(cache);
   EXPECT_EQ(size_on_disk(named / CACHE_DIR_NAME_SF / gpu_name / "foz_cache_idx.foz"),
             foz_magic_size + foz_index_record_size);
}

/* amdgpu-wddm BD-216: the cost that the single-file cache removes. A
 * multi-file cache opens a file for each entry it reads or writes; Microsoft
 * Defender scans a file at each open (about 7 ms each on the BC-250, 7179
 * opens at a CS2 start). The single-file cache opens its files when an
 * instance starts and none after, for any number of entries.
 */
TEST_F(CacheWin32SingleFile, OpensItsFilesOncePerInstance)
{
   ASSERT_TRUE(open_counter::install()) << "no CreateFileW import in this executable to count";

   /* The counter sees the CRT's opens. */
   open_counter::start(dir);
   FILE *probe = _wfopen((dir / "probe").c_str(), L"wb");
   ASSERT_NE(probe, nullptr);
   fclose(probe);
   ASSERT_EQ(open_counter::stop(), 1u) << "the counter's positive control";

   const unsigned num_entries = 500;
   std::vector<std::vector<uint8_t>> entries;
   for (unsigned i = 0; i < num_entries; i++)
      entries.push_back(make_data(0x7000 + i, 2000 + (i * 41) % 6000));

   /* What N compiles do: N misses, N puts, and N hits after them. */
   std::string sf_paths;
   auto workload = [&](unsigned *first_instance, unsigned *second_instance, const fs::path &counted) {
      cache_key key;
      open_counter::start(counted);
      struct disk_cache *cache = disk_cache_create(gpu_name, driver_id, 0);
      ASSERT_TRUE(cache && !cache->path_init_failed);
      for (const std::vector<uint8_t> &data : entries) {
         disk_cache_compute_key(cache, data.data(), data.size(), key);
         EXPECT_EQ(disk_cache_get(cache, key, NULL), nullptr);
         disk_cache_put(cache, key, data.data(), data.size(), NULL);
      }
      disk_cache_wait_for_idle(cache);
      for (const std::vector<uint8_t> &data : entries) {
         disk_cache_compute_key(cache, data.data(), data.size(), key);
         EXPECT_TRUE(holds(cache, key, data));
      }
      disk_cache_destroy(cache);
      sf_paths = open_counter::paths();
      *first_instance = open_counter::stop();

      /* The next start of the application: N hits. */
      open_counter::start(counted);
      cache = disk_cache_create(gpu_name, driver_id, 0);
      ASSERT_TRUE(cache && !cache->path_init_failed);
      for (const std::vector<uint8_t> &data : entries) {
         disk_cache_compute_key(cache, data.data(), data.size(), key);
         EXPECT_TRUE(holds(cache, key, data));
      }
      disk_cache_destroy(cache);
      *second_instance = open_counter::stop();
   };

   unsigned sf_first = 0, sf_second = 0;
   workload(&sf_first, &sf_second, dir / CACHE_DIR_NAME_SF);
   ASSERT_FALSE(HasFailure());
   const std::string sf_first_paths = sf_paths;

   os_set_option("MESA_DISK_CACHE_MULTI_FILE", "true", true);
   unsigned mf_first = 0, mf_second = 0;
   workload(&mf_first, &mf_second, dir / CACHE_DIR_NAME);
   os_unset_option("MESA_DISK_CACHE_MULTI_FILE");

   printf("BD-216 file opens for %u entries: single-file %u then %u, multi-file %u then %u\n", num_entries,
          sf_first, sf_second, mf_first, mf_second);
   RecordProperty("single_file_opens", std::to_string(sf_first) + "," + std::to_string(sf_second));
   RecordProperty("multi_file_opens", std::to_string(mf_first) + "," + std::to_string(mf_second));

   /* The database's two files, and the size index that disk_cache_type_create() maps for a cache of any type. */
   EXPECT_EQ(sf_first, 3u) << "foz_cache.foz, foz_cache_idx.foz and index, once; opened:\n" << sf_first_paths;
   EXPECT_EQ(sf_second, 3u) << "foz_cache.foz, foz_cache_idx.foz and index, once";
   EXPECT_GE(mf_first, 2 * num_entries) << "the control: a file for each put and each hit";
   EXPECT_GE(mf_second, num_entries) << "the control: a file for each hit";
}

TEST_F(CacheWin32SingleFile, ProcessesWriteAtOnceIntoAnOpenDatabase)
{
   /* This instance stays open while the writers put their entries; it reads
    * what they wrote through the index it already holds.
    */
   struct disk_cache *held = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_TRUE(held && !held->path_init_failed);
   ASSERT_EQ(held->type, DISK_CACHE_SINGLE_FILE);

   run_writers(dir);
   ASSERT_FALSE(HasFailure());

   struct disk_cache *fresh = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_TRUE(fresh && !fresh->path_init_failed);

   cache_key key;
   for (struct disk_cache *cache : { held, fresh }) {
      const char *which = cache == held ? "held instance" : "new instance";
      for (unsigned i = 0; i < num_shared; i++) {
         std::vector<uint8_t> data = shared_data(i);
         disk_cache_compute_key(cache, data.data(), data.size(), key);
         EXPECT_TRUE(holds(cache, key, data)) << which << ", shared entry " << i;
      }
      for (unsigned child = 0; child < num_writers; child++) {
         for (unsigned i = 0; i < num_unique; i++) {
            std::vector<uint8_t> data = unique_data(child, i);
            disk_cache_compute_key(cache, data.data(), data.size(), key);
            EXPECT_TRUE(holds(cache, key, data)) << which << ", child " << child << " entry " << i;
         }
      }
   }

   EXPECT_EQ(size_on_disk(sf_dir() / "foz_cache_idx.foz"),
             foz_magic_size + (num_shared + num_writers * num_unique) * foz_index_record_size)
      << "each entry written once";

   disk_cache_destroy(fresh);
   disk_cache_destroy(held);
}

TEST_F(CacheWin32SingleFile, ThreadsOfOneInstance)
{
   struct disk_cache *cache = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_TRUE(cache && !cache->path_init_failed);

   const unsigned num_threads = 8, per_thread = 40;
   std::atomic<unsigned> missing { 0 };
   std::atomic<unsigned> arrived { 0 };
   std::vector<std::thread> threads;
   for (unsigned t = 0; t < num_threads; t++) {
      threads.emplace_back([&, t] {
         cache_key key;
         for (unsigned i = 0; i < per_thread; i++)
            put(cache, make_data(0x9000 + t * per_thread + i, 1500 + i * 97), key);
         disk_cache_wait_for_idle(cache);

         /* Everyone's entries, once all have put theirs. */
         arrived++;
         while (arrived.load() < num_threads)
            std::this_thread::yield();
         disk_cache_wait_for_idle(cache);
         for (unsigned j = 0; j < num_threads * per_thread; j++) {
            std::vector<uint8_t> data = make_data(0x9000 + j, 1500 + (j % per_thread) * 97);
            disk_cache_compute_key(cache, data.data(), data.size(), key);
            if (!holds(cache, key, data))
               missing++;
         }
      });
   }
   for (std::thread &thread : threads)
      thread.join();

   EXPECT_EQ(missing.load(), 0u);
   EXPECT_EQ(size_on_disk(sf_dir() / "foz_cache_idx.foz"),
             foz_magic_size + num_threads * per_thread * foz_index_record_size);
   disk_cache_destroy(cache);
}

TEST_F(CacheWin32SingleFile, SizeBound)
{
   os_set_option("MESA_SHADER_CACHE_MAX_SIZE", "1M", true);
   const uint64_t bound = 1024 * 1024;
   const size_t entry_size = 64 * 1024;
   const unsigned num_entries = 40;

   struct disk_cache *a = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_TRUE(a && !a->path_init_failed);
   ASSERT_EQ(a->foz_db.max_size, bound);

   cache_key key;
   std::vector<std::array<uint8_t, CACHE_KEY_SIZE>> keys;
   for (unsigned i = 0; i < num_entries; i++) {
      put(a, make_data(0xA000 + i, entry_size), key);
      keys.emplace_back();
      memcpy(keys.back().data(), key, CACHE_KEY_SIZE);
   }
   disk_cache_wait_for_idle(a);

   unsigned stored = 0;
   for (unsigned i = 0; i < num_entries; i++) {
      if (holds(a, keys[i].data(), make_data(0xA000 + i, entry_size)))
         stored++;
   }
   EXPECT_GT(a->foz_db.writes_refused_full, 0u);
   EXPECT_EQ(stored + a->foz_db.writes_refused_full, num_entries);
   EXPECT_GE(stored, 10u) << "the bound holds about 15 of these entries";
   EXPECT_LE(size_on_disk(sf_dir() / "foz_cache.foz"), bound);
   std::error_code ec;
   EXPECT_TRUE(fs::exists(sf_dir() / "foz_cache.foz.full", ec)) << "the marker of a full database";
   EXPECT_TRUE(holds(a, keys[0].data(), make_data(0xA000, entry_size))) << "the first entries stay";

   /* A full database that another instance has open stays as it is. */
   struct disk_cache *b = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_TRUE(b && !b->path_init_failed);
   EXPECT_TRUE(holds(b, keys[0].data(), make_data(0xA000, entry_size))) << "not emptied while in use";
   disk_cache_destroy(b);
   disk_cache_destroy(a);

   /* No instance has it open: the next start empties it. */
   struct disk_cache *c = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_TRUE(c && !c->path_init_failed);
   EXPECT_EQ(disk_cache_get(c, keys[0].data(), NULL), nullptr) << "emptied at the start";
   EXPECT_EQ(size_on_disk(sf_dir() / "foz_cache_idx.foz"), foz_magic_size);
   EXPECT_EQ(size_on_disk(sf_dir() / "foz_cache.foz"), foz_magic_size);
   EXPECT_FALSE(fs::exists(sf_dir() / "foz_cache.foz.full", ec)) << "no marker after the reset";

   std::vector<uint8_t> data = make_data(0xB000, entry_size);
   put(c, data, key);
   disk_cache_wait_for_idle(c);
   EXPECT_TRUE(holds(c, key, data)) << "entries again after the reset";
   EXPECT_EQ(c->foz_db.writes_refused_full, 0u);
   disk_cache_destroy(c);

   os_unset_option("MESA_SHADER_CACHE_MAX_SIZE");
}

TEST_F(CacheWin32, DefaultDirectoryForUserAccountsOnly)
{
   const std::wstring saved = get_env(L"LOCALAPPDATA");
   fs::path local_app_data = dir / "LocalAppData";
   std::error_code ec;
   fs::create_directories(local_app_data, ec);
   unset_env(L"MESA_SHADER_CACHE_DIR");

   /* This test runs as a user account, which gets %LOCALAPPDATA%. */
   set_env(L"LOCALAPPDATA", local_app_data.wstring());
   struct disk_cache *cache = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_TRUE(cache && !cache->path_init_failed);
   EXPECT_TRUE(fs::equivalent(fs::u8path(cache->path), local_app_data / CACHE_DIR_NAME, ec));
   disk_cache_destroy(cache);

   /* Without %LOCALAPPDATA% the cache is off, unless XDG_CACHE_HOME names a directory. */
   unset_env(L"LOCALAPPDATA");
   cache = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_NE(cache, nullptr);
   EXPECT_TRUE(cache->path_init_failed);
   disk_cache_destroy(cache);

   set_env(L"XDG_CACHE_HOME", (dir / "xdg").wstring());
   cache = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_TRUE(cache && !cache->path_init_failed);
   EXPECT_TRUE(fs::equivalent(fs::u8path(cache->path), dir / "xdg" / CACHE_DIR_NAME, ec));
   disk_cache_destroy(cache);
   unset_env(L"XDG_CACHE_HOME");

   if (!saved.empty())
      set_env(L"LOCALAPPDATA", saved);

   /* The accounts that get a default directory, and those that get none. */
   const struct {
      const wchar_t *sid;
      bool has_default_dir;
   } accounts[] = {
      { L"S-1-5-21-1004336348-1177238915-682003330-1001", true }, /* a local or domain user */
      { L"S-1-12-1-1234567890-1234567890-1234567890-1234567890", true }, /* a Microsoft Entra ID user */
      { L"S-1-5-18", false },          /* LocalSystem */
      { L"S-1-5-19", false },          /* LocalService */
      { L"S-1-5-20", false },          /* NetworkService */
      { L"S-1-5-90-0-1", false },      /* Window Manager\DWM-1 */
      { L"S-1-5-96-0-1", false },      /* Font Driver Host\UMFD-1 */
      { L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", false }, /* NT SERVICE\TrustedInstaller */
      { L"S-1-5-82-1036420768-1044797643-1061213386-2937092688-4282445334", false }, /* an IIS application pool */
      { L"S-1-1-0", false },           /* Everyone */
   };
   for (const auto &account : accounts) {
      PSID sid = NULL;
      ASSERT_TRUE(ConvertStringSidToSidW(account.sid, &sid));
      EXPECT_EQ(disk_cache_account_has_default_dir(sid), account.has_default_dir)
         << fs::path(account.sid);
      LocalFree(sid);
   }
}

TEST_F(CacheWin32, ModuleIdentity)
{
   const std::array<uint8_t, BLAKE3_OUT_LEN> self = identity_of((const void *)disk_cache_create);
   EXPECT_EQ(identity_of((const void *)identity_of), self) << "one module, one identity";

   wchar_t exe[MAX_PATH];
   DWORD exe_len = GetModuleFileNameW(NULL, exe, MAX_PATH);
   ASSERT_TRUE(exe_len > 0 && exe_len < MAX_PATH);

   /* Copies of this executable, mapped as images without running anything. */
   fs::path copy = dir / "copy.exe";
   fs::path new_stamp = dir / "new-stamp.exe";
   fs::path new_pdb = dir / "new-pdb.exe";
   std::error_code ec;
   ASSERT_TRUE(fs::copy_file(exe, copy, ec)) << ec.message();
   ASSERT_TRUE(copy_as_rebuilt(exe, new_stamp, false));
   ASSERT_TRUE(copy_as_rebuilt(exe, new_pdb, true));

   HMODULE same = LoadLibraryExW(copy.c_str(), NULL, DONT_RESOLVE_DLL_REFERENCES);
   HMODULE stamp = LoadLibraryExW(new_stamp.c_str(), NULL, DONT_RESOLVE_DLL_REFERENCES);
   HMODULE pdb = LoadLibraryExW(new_pdb.c_str(), NULL, DONT_RESOLVE_DLL_REFERENCES);
   ASSERT_TRUE(same && stamp && pdb) << "LoadLibraryEx: error " << GetLastError();
   ASSERT_NE(same, GetModuleHandleW(NULL));

   const uint8_t *in_same = (const uint8_t *)same + 0x10;
   EXPECT_EQ(identity_of(in_same), self) << "a copy of the same build";
   EXPECT_NE(identity_of((const uint8_t *)stamp + 0x10), self) << "a new link time stamp";
   EXPECT_NE(identity_of((const uint8_t *)pdb + 0x10), self) << "a new PDB GUID";

   /* A driver in use is replaced by renaming it aside and putting the next
    * build at its path. The loaded build keeps its own identity.
    */
   fs::path held = dir / "copy.exe.held";
   fs::rename(copy, held, ec);
   ASSERT_FALSE(ec) << "renaming a loaded image: " << ec.message();
   ASSERT_TRUE(fs::copy_file(new_stamp, copy, ec)) << ec.message();
   EXPECT_EQ(identity_of(in_same), self) << "after its file was replaced";

   FreeLibrary(same);
   FreeLibrary(stamp);
   FreeLibrary(pdb);
}

TEST_F(CacheWin32, WriterThreadsRunAtLowestPriority)
{
   struct disk_cache *cache = disk_cache_create(gpu_name, driver_id, 0);
   ASSERT_TRUE(cache && !cache->path_init_failed);

   cache_key key;
   for (unsigned i = 0; i < 64; i++)
      put(cache, make_data(2000 + i, 20000), key);
   /* The finish barrier runs a job on every thread the queue has started. */
   disk_cache_wait_for_idle(cache);

   ASSERT_GT(cache->cache_queue.num_threads, 0u);
   for (unsigned i = 0; i < cache->cache_queue.num_threads; i++)
      EXPECT_EQ(GetThreadPriority(cache->cache_queue.threads[i].handle), THREAD_PRIORITY_LOWEST)
         << "thread " << i;

   disk_cache_destroy(cache);
}
