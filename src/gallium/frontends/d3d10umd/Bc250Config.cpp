/* SPDX-License-Identifier: MIT
 *
 * Configuration of the hosted D3D UMD as a registered UMD (see Bc250Config.h).
 */

#include "DriverIncludes.h"

#include "Bc250Config.h"
#include "bc250_adapter_identity.h"
#include "util/bc250_diag.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "advapi32.lib")

static INIT_ONCE bc250_config_once = INIT_ONCE_STATIC_INIT;
static struct Bc250Config bc250_config;

/* A present environment variable that fits the buffer. */
static bool
EnvValue(const char *name, char *out, DWORD size)
{
   DWORD n = GetEnvironmentVariableA(name, out, size);
   return n && n < size;
}

static bool
EnvValueW(const wchar_t *name, wchar_t *out, DWORD chars)
{
   DWORD n = GetEnvironmentVariableW(name, out, chars);
   return n && n < chars;
}

/* The trial environment may turn a default-on mode off with the literal "0". */
static bool
EnvSaysOff(const char *name)
{
   char value[8] = {};
   return EnvValue(name, value, sizeof value) && !strcmp(value, "0");
}

static bool
RegistryDword(const wchar_t *name, DWORD *value)
{
   DWORD size = sizeof(*value);
   return RegGetValueW(HKEY_LOCAL_MACHINE, BC250_HOSTED_UMD_KEY, name, RRF_RT_REG_DWORD,
                       NULL, value, &size) == ERROR_SUCCESS;
}

/* A non-empty REG_SZ that fits; RegGetValueW terminates it. */
static bool
RegistryString(const wchar_t *name, wchar_t *out, DWORD chars)
{
   DWORD size = chars * sizeof(wchar_t);
   out[0] = 0;
   return RegGetValueW(HKEY_LOCAL_MACHINE, BC250_HOSTED_UMD_KEY, name, RRF_RT_REG_SZ,
                       NULL, out, &size) == ERROR_SUCCESS && out[0];
}

/* The directory of this DLL, whatever its registered path. */
static bool
ModuleDirectory(wchar_t *out, DWORD chars)
{
   HMODULE self = NULL;
   if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&bc250_config, &self))
      return false;
   DWORD n = GetModuleFileNameW(self, out, chars);
   if (!n || n >= chars)
      return false;
   wchar_t *slash = wcsrchr(out, L'\\');
   if (!slash)
      return false;
   *slash = 0;
   return true;
}

/* util_dl_open takes an ANSI path (LoadLibraryA): refuse one that does not survive the conversion. */
static HRESULT
AnsiPath(const wchar_t *wide, char *out, size_t size)
{
   BOOL lossy = FALSE;
   int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide, -1, out, (int)size, NULL, &lossy);
   if (!n || lossy) {
      out[0] = 0;
      return HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION);
   }
   return S_OK;
}

static void
ResolveIcd(struct Bc250Config *c)
{
   wchar_t path[MAX_PATH] = {};
   char env[MAX_PATH] = {};
   c->icd_status = E_FAIL;
   if (EnvValue("BC250_HOSTED_ICD", env, sizeof env) && env[0]) {
      /* Trial tooling names the ICD in ANSI already, as DWM050's router did. */
      c->icd_source = "environment";
      memcpy(c->icd_path, env, strlen(env) + 1);
      if (!MultiByteToWideChar(CP_ACP, 0, env, -1, path, MAX_PATH)) {
         c->icd_status = HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION);
         return;
      }
   } else if (RegistryString(L"IcdPath", path, MAX_PATH)) {
      c->icd_source = "registry";
      c->icd_status = AnsiPath(path, c->icd_path, sizeof c->icd_path);
      if (FAILED(c->icd_status)) return;
   } else {
      c->icd_source = "module-directory";
      if (!c->module_directory[0] ||
          _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%ls\\%ls", c->module_directory, BC250_HOSTED_ICD_FILE) < 0) {
         c->icd_status = HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
         return;
      }
      c->icd_status = AnsiPath(path, c->icd_path, sizeof c->icd_path);
      if (FAILED(c->icd_status)) return;
   }
   /* Only a full path: a bare name would be searched for through the process's DLL search order. */
   const DWORD attributes = GetFileAttributesW(path);
   if (path[0] == 0 || path[1] != L':' || path[2] != L'\\') {
      c->icd_status = E_INVALIDARG;
   } else if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
      c->icd_status = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
   } else {
      c->icd_status = S_OK;
   }
}

/* With diagnostics on, stderr (shared with the ICD through the UCRT) goes to a per-process file. */
static void
OpenDiagnosticsLog(struct Bc250Config *c)
{
   wchar_t directory[MAX_PATH] = {};
   if (!EnvValueW(L"BC250_UMD_DIAG_DIR", directory, MAX_PATH) &&
       !RegistryString(L"DiagnosticsDirectory", directory, MAX_PATH)) {
      if (!c->module_directory[0] ||
          _snwprintf_s(directory, MAX_PATH, _TRUNCATE, L"%ls\\logs", c->module_directory) < 0)
         return;
   }
   wchar_t exe[MAX_PATH] = {};
   DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
   const wchar_t *base = n && n < MAX_PATH ? wcsrchr(exe, L'\\') : NULL;
   base = base ? base + 1 : L"process";
   if (_snwprintf_s(c->log_path, MAX_PATH, _TRUNCATE, L"%ls\\bc250d3d_zink-%ls-%lu.log",
                    directory, base, GetCurrentProcessId()) < 0) {
      c->log_path[0] = 0;
      return;
   }
   /* Shared access on purpose (as DWM050's router): a reader may follow the file while DWM runs. The UCRT
    * default stderr mode is kept, so every completed line reaches the file before a crash. */
#pragma warning(suppress: 4996)
   if (!_wfreopen(c->log_path, L"a", stderr)) {
      OutputDebugStringW(L"bc250d3d_zink: diagnostics log could not be opened\n");
      c->log_path[0] = 0;
   }
}

static BOOL CALLBACK
ResolveConfig(PINIT_ONCE, PVOID, PVOID *)
{
   struct Bc250Config *c = &bc250_config;
   c->hosted_render = !EnvSaysOff("BC250_HOSTED_RENDER");
   c->runtime_probe = !EnvSaysOff("BC250_D3D_RUNTIME_PROBE");
   if (!ModuleDirectory(c->module_directory, MAX_PATH))
      c->module_directory[0] = 0;

   char env[8] = {};
   DWORD registry = 0;
   if (EnvValue("BC250_UMD_DIAG", env, sizeof env))
      c->diagnostics = !strcmp(env, "1");
   else if (RegistryDword(L"Diagnostics", &registry))
      c->diagnostics = registry != 0;
   /* Full entrypoint tracing implies diagnostics: without them it would have nowhere to go. */
   if (EnvValue("BC250_UMD_VERBOSE", env, sizeof env) && !strcmp(env, "1"))
      c->diagnostics = true;

   bc250_diag_enabled = c->diagnostics;
   if (c->diagnostics)
      OpenDiagnosticsLog(c);

   ResolveIcd(c);
   BC250_DIAG("BC250 hosted UMD config pid=%lu hosted_render=%u runtime_probe=%u icd_source=%s icd=%s icd_status=%08lx\n",
              GetCurrentProcessId(), c->hosted_render, c->runtime_probe,
              c->icd_source ? c->icd_source : "none", c->icd_path, (unsigned long)c->icd_status);
   if (FAILED(c->icd_status))
      BC250_ERROR("BC250 hosted UMD: ICD unusable (source %s, status %08lx)\n",
                  c->icd_source ? c->icd_source : "none", (unsigned long)c->icd_status);
   return TRUE;
}

const struct Bc250Config *
Bc250GetConfig(void)
{
   InitOnceExecuteOnce(&bc250_config_once, ResolveConfig, NULL, NULL);
   return &bc250_config;
}

int
Bc250UmdVerbose(void)
{
   /* Benign race: every thread computes the same value. */
   static volatile LONG verbose = -1;
   if (verbose < 0) {
      char value[8] = {};
      verbose = Bc250GetConfig()->diagnostics && EnvValue("BC250_UMD_VERBOSE", value, sizeof value) &&
                !strcmp(value, "1");
   }
   return verbose;
}

/* Strict 1-16 hex digits, nonzero. */
static bool
ParseLuid(const char *text, UINT64 *luid)
{
   size_t length = strlen(text);
   if (!length || length > 16)
      return false;
   for (size_t i = 0; i < length; i++) {
      const char ch = text[i];
      if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')))
         return false;
   }
   errno = 0;
   *luid = _strtoui64(text, NULL, 16);
   return !errno && *luid;
}

HRESULT
Bc250QueryAdapterLuid(const void *args, UINT64 *luid)
{
   const D3D10DDIARG_OPENADAPTER *open = (const D3D10DDIARG_OPENADAPTER *)args;
   *luid = 0;

   /* The KMD writes the trailer only into a buffer of the extended size, and a KMD without it writes the
    * legacy prefix and succeeds: the buffer is zeroed and every header field is required (the contract's
    * rule, bc250_adapter_identity.h). */
   UINT64 identity = 0;
   HRESULT status = E_NOINTERFACE;
   const D3DDDI_ADAPTERCALLBACKS *callbacks = open ? open->pAdapterCallbacks : NULL;
   if (callbacks && callbacks->pfnQueryAdapterInfoCb) {
      unsigned char data[BC250_ADAPTER_CAPS_BYTES] = {};
      D3DDDICB_QUERYADAPTERINFO query = {};
      query.pPrivateDriverData = data;
      query.PrivateDriverDataSize = sizeof(data);
      status = callbacks->pfnQueryAdapterInfoCb(open->hRTAdapter.handle, &query);
      if (SUCCEEDED(status)) {
         struct bc250_adapter_identity trailer;
         memcpy(&trailer, data + BC250_ADAPTER_IDENTITY_OFFSET, sizeof(trailer));
         if (trailer.magic == BC250_ADAPTER_IDENTITY_MAGIC &&
             trailer.version == BC250_ADAPTER_IDENTITY_VERSION &&
             trailer.size == sizeof(trailer) && !trailer.reserved) {
            /* LUID.HighPart's bit pattern above LowPart: the in-memory LUID the ICD compares with memcmp. */
            identity = ((UINT64)trailer.luid_high << 32) | trailer.luid_low;
            if (!identity)
               status = E_UNEXPECTED;
         } else {
            status = E_NOINTERFACE;
         }
      }
   }

   char text[32] = {};
   UINT64 environment = 0;
   const bool haveEnvironment = EnvValue("BC250_D3D_ZINK_LUID", text, sizeof text);
   if (haveEnvironment && !ParseLuid(text, &environment)) {
      BC250_ERROR("BC250 hosted UMD: invalid BC250_D3D_ZINK_LUID\n");
      return E_INVALIDARG;
   }
   if (identity) {
      if (haveEnvironment && environment != identity) {
         BC250_ERROR("BC250 hosted UMD: BC250_D3D_ZINK_LUID %llx is not the adapter %llx\n",
                     (unsigned long long)environment, (unsigned long long)identity);
         return E_INVALIDARG;
      }
      *luid = identity;
      BC250_DIAG("BC250 hosted UMD adapter luid=%016llx source=identity\n", (unsigned long long)identity);
      return S_OK;
   }
   if (haveEnvironment) {
      *luid = environment;
      BC250_DIAG("BC250 hosted UMD adapter luid=%016llx source=environment identity_status=%08lx\n",
                 (unsigned long long)environment, (unsigned long)status);
      return S_OK;
   }
   BC250_ERROR("BC250 hosted UMD: no adapter identity (status %08lx)\n", (unsigned long)status);
   return FAILED(status) ? status : E_FAIL;
}
