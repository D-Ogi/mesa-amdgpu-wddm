/* SPDX-License-Identifier: MIT
 *
 * amdgpu-wddm: a release read of a BC-250 switch gives the value that the lab read gives with an empty user
 * environment (util/bc250_env.h).
 *
 *   bc250_env_test                 starts itself twice, each time in a fresh process with an empty environment:
 *                                  once for a machine with a BC-250 and once for a machine without one
 *   bc250_env_test --child bc250   the machine with a BC-250: the lab flavour applies the DirectMesh preset to
 *                                  the environment, the release flavour sets bc250_env_preset()
 *   bc250_env_test --child other   another adapter: neither flavour applies the preset
 *
 * A child expands every read of bc250_env_sites.h (one per distinct form, name and default of the tree) in both
 * flavours, the lab form against the environment and the release form against the preset state, and compares
 * the two values. It also checks that the lab preset puts exactly the table's value of every preset name into the
 * environment. Any difference fails the test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/os_misc.h"
#include "util/u_debug.h"

#ifdef _WIN32
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#endif

extern char **environ;

static unsigned checks, failures;

static void
fail(const char *name, const char *where, const char *lab, const char *rel)
{
   failures++;
   fprintf(stderr, "FAIL %s (%s): lab %s, release %s\n", name, where, lab, rel);
}

/* One check for each kind of value. A tree without a read of one kind does not use that check. */
UNUSED static void
check_bool(bool lab, bool rel, const char *name, const char *where)
{
   checks++;
   if (lab != rel)
      fail(name, where, lab ? "true" : "false", rel ? "true" : "false");
}

UNUSED static void
check_num(int64_t lab, int64_t rel, const char *name, const char *where)
{
   char a[32], b[32];
   checks++;
   if (lab != rel) {
      snprintf(a, sizeof(a), "%lld", (long long)lab);
      snprintf(b, sizeof(b), "%lld", (long long)rel);
      fail(name, where, a, b);
   }
}

UNUSED static void
check_str(const char *lab, const char *rel, const char *name, const char *where)
{
   checks++;
   if ((lab == NULL) != (rel == NULL) || (lab && strcmp(lab, rel) != 0))
      fail(name, where, lab ? lab : "(null)", rel ? rel : "(null)");
}

/* The cached forms define a function each: one per flavour and entry. */
#define DECL_BC250_ENV_BOOL(n, d)
#define DECL_BC250_ENV_BOOL_P(n, d)
#define DECL_BC250_ENV_NUM(n, d)
#define DECL_BC250_ENV_NUM_P(n, d)
#define DECL_BC250_ENV_STR(n, d)
#define DECL_BC250_ENV_STR_P(n, d)
#define DECL_BC250_GETENV(n, d)
#define DECL_BC250_GETENV_P(n, d)
#define DECL_BC250_OS_OPTION(n, d)
#define DECL_BC250_GET_ONCE_BOOL_OPTION(n, d) \
   BC250_GET_ONCE_BOOL_OPTION_LAB(lab_##n, n, d) BC250_GET_ONCE_BOOL_OPTION_REL(rel_##n, n, d)
#define DECL_BC250_GET_ONCE_BOOL_OPTION_P(n, d) \
   BC250_GET_ONCE_BOOL_OPTION_P_LAB(lab_##n, n, d) BC250_GET_ONCE_BOOL_OPTION_P_REL(rel_##n, n, d)
#define DECL_BC250_GET_ONCE_OPTION(n, d) \
   BC250_GET_ONCE_OPTION_LAB(lab_##n, n, d) BC250_GET_ONCE_OPTION_REL(rel_##n, n, d)

#define BC250_SITE(form, n, d, where, count) DECL_##form(n, d)
#include "bc250_env_sites.h"
#undef BC250_SITE

#define CHECK_BC250_ENV_BOOL(n, d, w) check_bool(BC250_ENV_BOOL_LAB(n, d), BC250_ENV_BOOL_REL(n, d), #n, w)
#define CHECK_BC250_ENV_BOOL_P(n, d, w) check_bool(BC250_ENV_BOOL_P_LAB(n, d), BC250_ENV_BOOL_P_REL(n, d), #n, w)
#define CHECK_BC250_ENV_NUM(n, d, w) check_num(BC250_ENV_NUM_LAB(n, d), BC250_ENV_NUM_REL(n, d), #n, w)
#define CHECK_BC250_ENV_NUM_P(n, d, w) check_num(BC250_ENV_NUM_P_LAB(n, d), BC250_ENV_NUM_P_REL(n, d), #n, w)
#define CHECK_BC250_ENV_STR(n, d, w) check_str(BC250_ENV_STR_LAB(n, d), BC250_ENV_STR_REL(n, d), #n, w)
#define CHECK_BC250_ENV_STR_P(n, d, w) check_str(BC250_ENV_STR_P_LAB(n, d), BC250_ENV_STR_P_REL(n, d), #n, w)
#define CHECK_BC250_GETENV(n, d, w) check_str(BC250_GETENV_LAB(n), BC250_GETENV_REL(n), #n, w)
#define CHECK_BC250_GETENV_P(n, d, w) check_str(BC250_GETENV_P_LAB(n), BC250_GETENV_P_REL(n), #n, w)
#define CHECK_BC250_OS_OPTION(n, d, w) check_str(BC250_OS_OPTION_LAB(n), BC250_OS_OPTION_REL(n), #n, w)
#define CHECK_BC250_GET_ONCE_BOOL_OPTION(n, d, w) \
   check_bool(debug_get_option_lab_##n(), debug_get_option_rel_##n(), #n, w)
#define CHECK_BC250_GET_ONCE_BOOL_OPTION_P(n, d, w) \
   check_bool(debug_get_option_lab_##n(), debug_get_option_rel_##n(), #n, w)
#define CHECK_BC250_GET_ONCE_OPTION(n, d, w) \
   check_str(debug_get_option_lab_##n(), debug_get_option_rel_##n(), #n, w)

/* An environment that names no switch of this driver, in any letter case: what a user who sets nothing has. */
static bool
environment_is_empty(void)
{
   for (char **e = environ; e && *e; e++) {
      char name[256];
      size_t i;
      for (i = 0; (*e)[i] && (*e)[i] != '=' && i + 1 < sizeof(name); i++)
         name[i] = (*e)[i] >= 'a' && (*e)[i] <= 'z' ? (*e)[i] - 32 : (*e)[i];
      name[i] = 0;
      if (strstr(name, "BC250") || strncmp(name, "RADV_", 5) == 0 || strncmp(name, "AMDGPU_WDDM", 11) == 0) {
         fprintf(stderr, "FAIL the environment of the child holds %s\n", name);
         return false;
      }
   }
   return true;
}

static int
child(bool bc250)
{
   if (!environment_is_empty())
      return 1;
   if (bc250) {
      bc250_env_apply_directmesh_preset();
      bc250_env_set_preset();
   }

   /* The lab preset puts the table's value of every preset name into the environment, and only on a BC-250. */
#define CHECK_PRESET(n) check_str(getenv(#n), bc250 ? BC250_PRESET_##n : NULL, #n, "util/bc250_env.h preset");
   BC250_DIRECTMESH_PRESET(CHECK_PRESET)
#undef CHECK_PRESET
   unsigned preset = checks;

#define BC250_SITE(form, n, d, where, count) CHECK_##form(n, d, where);
#include "bc250_env_sites.h"
#undef BC250_SITE

   printf("%s: %u preset names and %u reads, %u differences\n", bc250 ? "BC-250" : "another adapter", preset,
          checks - preset, failures);
   return failures ? 1 : 0;
}

static int
run_child(const char *self, const char *mode)
{
   const char *args[] = {self, "--child", mode, NULL};
   /* A fresh process whose environment holds nothing but what the C runtime needs to start. */
#ifdef _WIN32
   char root[512];
   const char *sr = getenv("SystemRoot");
   snprintf(root, sizeof(root), "SystemRoot=%s", sr ? sr : "C:\\Windows");
   const char *env[] = {root, NULL};
   intptr_t rc = _spawnve(_P_WAIT, self, args, env);
   if (rc < 0) {
      perror("_spawnve");
      return 1;
   }
   return (int)rc;
#else
   char *env[] = {NULL};
   pid_t pid;
   int status;
   if (posix_spawn(&pid, self, NULL, NULL, (char *const *)args, env) != 0) {
      perror("posix_spawn");
      return 1;
   }
   if (waitpid(pid, &status, 0) < 0)
      return 1;
   return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

int
main(int argc, char **argv)
{
   if (argc == 3 && strcmp(argv[1], "--child") == 0)
      return child(strcmp(argv[2], "bc250") == 0);
   int a = run_child(argv[0], "bc250");
   int b = run_child(argv[0], "other");
   printf("bc250_env_test: %s\n", a == 0 && b == 0 ? "PASS" : "FAIL");
   return a == 0 && b == 0 ? 0 : 1;
}
