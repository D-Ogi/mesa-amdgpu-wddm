/* SPDX-License-Identifier: MIT
 *
 * amdgpu-wddm: the state of the BC-250 environment helper (bc250_env.h).
 */
#include "bc250_env.h"

#include <string.h>

#include "detect_os.h"
#include "u_atomic.h"

static bool bc250_env_preset_on;

bool
bc250_env_preset(void)
{
   return p_atomic_read_relaxed(&bc250_env_preset_on);
}

void
bc250_env_set_preset(void)
{
   p_atomic_set(&bc250_env_preset_on, true);
}

/* Only the lab build and the host test hold the names: a release build has no lookup to give them to. */
#if defined(AMDGPU_WDDM_LAB_FLAVOUR) || defined(BC250_ENV_TEST)
/* setenv(3) with overwrite == 0. On Windows _putenv_s updates the CRT environment and the process environment, so
 * getenv() and GetEnvironmentVariableA() (os_get_option) both see the value. */
static void
bc250_env_set_default(const char *name, const char *value)
{
   if (getenv(name))
      return;
#if DETECT_OS_WINDOWS
   _putenv_s(name, value);
#else
   setenv(name, value, 0);
#endif
}

void
bc250_env_apply_directmesh_preset(void)
{
#define BC250_ENV_PRESET_ENTRY(n) {#n, BC250_PRESET_##n},
   static const char *const settings[][2] = {BC250_DIRECTMESH_PRESET(BC250_ENV_PRESET_ENTRY)};
#undef BC250_ENV_PRESET_ENTRY
   for (unsigned i = 0; i < sizeof(settings) / sizeof(settings[0]); i++)
      bc250_env_set_default(settings[i][0], settings[i][1]);
}
#endif
