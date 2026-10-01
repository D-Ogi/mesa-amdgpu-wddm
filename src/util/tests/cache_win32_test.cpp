/* SPDX-License-Identifier: MIT
 *
 * Tests of the Windows implementation of the multi-file disk cache
 * (disk_cache_os.c), for what cache_test.cpp does not reach there or reaches
 * only within one process:
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
 * - a cache of the database type, such as a driver's custom cache, is a
 *   multi-file cache in its own directory;
 * - the default directory: %LOCALAPPDATA% for a user account only;
 * - the module identity: one for copies of a build, another for a rebuild,
 *   and the loaded build's own when the file at its path is replaced;
 * - the writer threads run at the lowest priority.
 *
 * Every test works in a directory of its own under the current directory.
 */

#include <gtest/gtest.h>

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
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
   }

   void TearDown() override
   {
      os_unset_option("MESA_SHADER_CACHE_DIR");
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

TEST_F(CacheWin32, ProcessesWriteAtOnce)
{
   const unsigned num_children = 4;
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
      fs::path log = dir / ("child-" + std::to_string(i) + ".log");
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
      EXPECT_EQ(code, 0u) << "child " << i << ", output in " << (dir / ("child-" + std::to_string(i) + ".log"));
   }

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
   unset_env(L"CACHE_WIN32_READY");
   unset_env(L"CACHE_WIN32_START");
   CloseHandle(ready);
   CloseHandle(start);
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

TEST_F(CacheWin32, DatabaseTypeIsMultiFile)
{
   /* RADV keeps its built-in shaders in a custom cache of the database type. */
   struct disk_cache *custom = disk_cache_create_custom(gpu_name, driver_id, 0, "custom_cache", 64 * 1024);
   ASSERT_TRUE(custom && !custom->path_init_failed);
   EXPECT_EQ(custom->type, DISK_CACHE_MULTI_FILE);
   EXPECT_EQ(custom->max_size, 64u * 1024);

   std::error_code ec;
   EXPECT_TRUE(fs::equivalent(fs::u8path(custom->path), dir / "custom_cache", ec));

   std::vector<uint8_t> data = make_data(19, 3000);
   cache_key key;
   put(custom, data, key);
   disk_cache_wait_for_idle(custom);
   EXPECT_TRUE(holds(custom, key, data));
   EXPECT_EQ(scan(dir / "custom_cache").entries, 1u);
   EXPECT_EQ(custom->size->value, scan(dir / "custom_cache").size);

   /* The database type asked for by name gets the multi-file cache's directory. */
   os_set_option("MESA_DISK_CACHE_DATABASE", "true", true);
   struct disk_cache *cache = disk_cache_create(gpu_name, driver_id, 0);
   os_unset_option("MESA_DISK_CACHE_DATABASE");
   ASSERT_TRUE(cache && !cache->path_init_failed);
   EXPECT_EQ(cache->type, DISK_CACHE_MULTI_FILE);
   EXPECT_TRUE(fs::equivalent(fs::u8path(cache->path), cache_dir(), ec));
   EXPECT_EQ(disk_cache_get(cache, key, NULL), nullptr) << "a cache of its own";

   disk_cache_destroy(cache);
   disk_cache_destroy(custom);
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
