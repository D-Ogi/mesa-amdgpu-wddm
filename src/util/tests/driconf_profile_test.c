/* SPDX-License-Identifier: MIT
 *
 * amdgpu-wddm: host test of the run-time driver configuration files (u_driconf_file.h) on Windows.
 *
 * Built with U_DRICONF_FILE_TEST against the static table of a Windows build tree, so that the files of a test
 * directory are read by the real xmlconfig.c and driParseConfigFiles (src/util/tests/driconf_profile_test.py builds
 * and runs it). Exit 0 when every case passes.
 *   1. the XML subset: what it reads and what it refuses;
 *   2. the access rule on security descriptors;
 *   3. a profile file entry applies to the named executable and not to others, a file overrides the static table,
 *      a later file overrides an earlier one, and a malformed file is refused whole and applies to no application;
 *   4. the real directory checks: a directory that the test user owns is refused.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <sddl.h>

#include "util/driconf.h"
#include "util/u_driconf_file.h"
#include "util/xmlconfig.h"

void driconfProfileTestReload(void);

static int failures;

#define CHECK(cond, ...)                                                                                              \
   do {                                                                                                               \
      if (!(cond)) {                                                                                                  \
         failures++;                                                                                                  \
         printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                  \
         printf(__VA_ARGS__);                                                                                         \
         printf("\n");                                                                                                \
      }                                                                                                               \
   } while (0)

/* ---- 1. the XML subset ---------------------------------------------------------------------------------------- */

static bool
parses(const char *text, unsigned *count, char *error)
{
   struct u_driconf_xml xml;
   bool ok = u_driconf_xml_parse(text, strlen(text), &xml, error, 256);
   if (count)
      *count = xml.count;
   u_driconf_xml_fini(&xml);
   return ok;
}

static void
test_xml(void)
{
   static const char *const good[] = {
      "<driconf/>",
      "\xef\xbb\xbf<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<driconf></driconf>",
      "<?xml version=\"1.0\"?>\n<!-- a comment -->\n<!DOCTYPE driconf [\n  <!ELEMENT driconf (device*)>\n"
      "  <!ATTLIST device driver CDATA #IMPLIED screen CDATA #IMPLIED>\n  <!-- inside -->\n]>\n<driconf/>\n",
      "<driconf>\r\n  <device driver='radv'>\r\n  </device>\r\n</driconf>",
      "<driconf><device driver=\"a&lt;b&amp;&#x41;&#66;&quot;&apos;\"/></driconf>",
   };
   char error[256];
   for (unsigned i = 0; i < sizeof(good) / sizeof(good[0]); i++)
      CHECK(parses(good[i], NULL, error), "good document %u refused: %s", i, error);

   static const char *const bad[] = {
      "",
      "<driconf>",
      "<driconf></device>",
      "<driconf/><driconf/>",
      "<driconf>text</driconf>",
      "<driconf><![CDATA[x]]></driconf>",
      "<driconf><?pi x?></driconf>",
      "<!DOCTYPE driconf [<!ENTITY e \"x\">]><driconf/>",
      "<!DOCTYPE driconf [%pe;]><driconf/>",
      "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><driconf/>",
      "<driconf a=\"1\" a=\"2\"/>",
      "<driconf a=1/>",
      "<driconf a=\"<\"/>",
      "<driconf a=\"&unknown;\"/>",
      "<driconf a=\"&#0;\"/>",
      "<driconf a=\"\xc3\x28\"/>",
      "<driconf a=\"\xed\xa0\x80\"/>",
      "<driconf>\x01</driconf>",
      "<driconf><!-- a -- b --></driconf>",
      "<driconf a=\"1\"b=\"2\"/>",
      "<driconf/>x",
      " <?xml version=\"1.0\"?><driconf/>",
   };
   for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
      CHECK(!parses(bad[i], NULL, error), "bad document %u accepted", i);

   /* The events and the decoded, normalized values. */
   const char *text = "<driconf>\n<device driver=\"a\tb\r\nc &amp; d\"><application name=\"x\" executable=\"g.exe\">"
                      "<option name=\"o\" value=\"1\"/></application></device></driconf>";
   struct u_driconf_xml xml;
   bool ok = u_driconf_xml_parse(text, strlen(text), &xml, error, sizeof(error));
   CHECK(ok && xml.count == 8, "event count %u", xml.count);
   if (ok && xml.count == 8) {
      CHECK(!strcmp(xml.events[1].name, "device") && !strcmp(xml.events[1].attr[1], "a b c & d"),
            "device value \"%s\"", xml.events[1].attr[1]);
      CHECK(xml.events[1].line == 2, "device line %u", xml.events[1].line);
      CHECK(xml.events[3].attr && !strcmp(xml.events[3].name, "option") && !xml.events[4].attr,
            "empty element events");
      CHECK(!xml.events[7].attr && !strcmp(xml.events[7].name, "driconf"), "root end event");
   }
   u_driconf_xml_fini(&xml);

   CHECK(!parses("<driconf>\n\n<device></driconf>", NULL, error) && !strncmp(error, "line 3:", 7),
         "error line: %s", error);

   /* Nesting deeper than the bound. */
   char deep[1024] = "";
   for (unsigned i = 0; i <= U_DRICONF_XML_MAX_DEPTH; i++)
      strcat(deep, "<a>");
   for (unsigned i = 0; i <= U_DRICONF_XML_MAX_DEPTH; i++)
      strcat(deep, "</a>");
   CHECK(!parses(deep, NULL, error), "deep nesting accepted");
}

/* ---- 2. the access rule --------------------------------------------------------------------------------------- */

static int
trusted(const char *sddl, char *why)
{
   PSECURITY_DESCRIPTOR sd = NULL;
   if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl, SDDL_REVISION_1, &sd, NULL))
      return -1;
   bool ok = u_driconf_trusted_descriptor(sd, why, 256);
   LocalFree(sd);
   return ok;
}

static void
test_access(void)
{
   static const struct {
      const char *sddl;
      int want;
   } cases[] = {
      /* The installer's rule: SYSTEM and Administrators full, users and app containers read. */
      {"O:BAG:SYD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)(A;OICI;0x1200a9;;;AC)"
       "(A;OICI;0x1200a9;;;S-1-15-2-2)", 1},
      {"O:SYG:SYD:(A;;FA;;;SY)", 1},
      {"O:S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464G:SYD:(A;;FA;;;BA)", 1},
      /* CREATOR OWNER only as an inherited template, OWNER RIGHTS, a deny entry. */
      {"O:BAG:SYD:(A;OICIIO;FA;;;CO)(A;;FA;;;OW)(D;;FA;;;BU)(A;;FA;;;BA)", 1},
      /* Refused: the owner is a user, users may write, no access list, a user may append (create folders). */
      {"O:BUG:SYD:(A;;FA;;;BA)", 0},
      {"O:BAG:SYD:(A;;FA;;;BA)(A;;FRFW;;;BU)", 0},
      {"O:BAG:SYD:NO_ACCESS_CONTROL", 0},
      {"O:BAG:SYD:(A;CI;0x4;;;BU)", 0},
      {"O:BAG:SYD:(A;;WD;;;WD)", 0},
      {"O:BAG:SYD:(A;;GA;;;AU)", 0},
   };
   char why[256];
   for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      why[0] = 0;
      int got = trusted(cases[i].sddl, why);
      CHECK(got == cases[i].want, "access case %u: got %d (%s)", i, got, why);
   }
}

/* ---- 3. the profile files through driParseConfigFiles --------------------------------------------------------- */

static wchar_t root[MAX_PATH];

static void
write_file(const char *relative, const char *text)
{
   wchar_t path[MAX_PATH], rel[MAX_PATH];
   MultiByteToWideChar(CP_UTF8, 0, relative, -1, rel, MAX_PATH);
   swprintf(path, MAX_PATH, L"%ls\\%ls", root, rel);
   FILE *f = _wfopen(path, L"wb");
   if (!f) {
      CHECK(false, "cannot write %s", relative);
      return;
   }
   fwrite(text, 1, strlen(text), f);
   fclose(f);
}

#define N_OPTS 6
static const char *const names[N_OPTS] = {
   "radv_split_fma", "radv_disable_trunc_coord", "radv_flush_before_query_copy", "radv_zero_vram",
   "radv_test_level", "radv_invariant_geom",
};

static void
query_device(const char *engine, const char *exe, const driConfigFileParseParams *device, int out[N_OPTS])
{
   driOptionDescription desc[] = {
      DRI_CONF_SECTION_DEBUG
      DRI_CONF_OPT_B(radv_split_fma, false, "")
      DRI_CONF_OPT_B(radv_disable_trunc_coord, false, "")
      DRI_CONF_OPT_B(radv_flush_before_query_copy, false, "")
      DRI_CONF_OPT_B(radv_zero_vram, false, "")
      DRI_CONF_OPT_I(radv_test_level, 0, 0, 100, "")
      DRI_CONF_OPT_B(radv_invariant_geom, false, "")
      DRI_CONF_SECTION_END
   };
   driOptionCache info = {0}, cache = {0};
   driParseOptionInfo(&info, desc, sizeof(desc) / sizeof(desc[0]));
   driInjectExecName(exe);
   driConfigFileParseParams params = *device;
   params.driverName = "radv";
   params.applicationName = exe;
   params.engineName = engine;
   driParseConfigFiles(&cache, &info, &params);
   for (unsigned i = 0; i < N_OPTS; i++)
      out[i] = i == 4 ? driQueryOptioni(&cache, names[i]) : driQueryOptionb(&cache, names[i]);
   driDestroyOptionCache(&cache);
   driDestroyOptionInfo(&info);
}

static void
query(const char *engine, const char *exe, int out[N_OPTS])
{
   query_device(engine, exe, &(driConfigFileParseParams){0}, out);
}

/* The <device> attributes of a file act as on Linux (parseDeviceAttr): kernel_driver, device, device_version_regexp
 * and screen each select the device or skip the section. */
static void
expect_device(const driConfigFileParseParams *device, const int want[N_OPTS], const char *what)
{
   int got[N_OPTS];
   query_device("", "devtest.exe", device, got);
   for (unsigned i = 0; i < N_OPTS; i++)
      CHECK(got[i] == want[i], "%s: %s is %d, expected %d", what, names[i], got[i], want[i]);
}

static void
expect(const char *engine, const char *exe, const int want[N_OPTS])
{
   int got[N_OPTS];
   query(engine, exe, got);
   for (unsigned i = 0; i < N_OPTS; i++)
      CHECK(got[i] == want[i], "engine \"%s\" %s: %s is %d, expected %d", engine, exe, names[i], got[i], want[i]);
}

static void
test_files(const wchar_t *base)
{
   swprintf(root, MAX_PATH, L"%ls\\drirc.d", base);
   CreateDirectoryW(base, NULL);
   CreateDirectoryW(root, NULL);

   /* Without files: the static table only. engine vkd3d sets radv_disable_trunc_coord (00-radv-defaults.conf). */
   u_driconf_test_root = base;
   u_driconf_test_trust_all = true;
   driconfProfileTestReload();
   expect("vkd3d", "witcher3.exe", (int[]){0, 1, 0, 0, 0, 0});
   expect("", "game.exe", (int[]){0, 0, 0, 0, 0, 0});

   write_file("10-game.conf",
              "<?xml version=\"1.0\"?>\n"
              "<driconf>\n"
              "  <device driver=\"radv\">\n"
              "    <application name=\"Game\" executable=\"game.exe\">\n"
              "      <option name=\"radv_split_fma\" value=\"true\" />\n"
              "      <option name=\"radv_test_level\" value=\"7\" />\n"
              "      <option name=\"radv_zero_vram\" value=\"yes\" />\n"           /* not a bool: unchanged */
              "      <option name=\"radv_not_an_option\" value=\"true\" />\n"      /* not this driver's: ignored */
              "    </application>\n"
              "    <application name=\"W3\" executable=\"witcher3.exe\">\n"
              "      <option name=\"radv_disable_trunc_coord\" value=\"false\" />\n" /* over the static table */
              "    </application>\n"
              "    <engine engine_name_match=\"^DXVK$\">\n"
              "      <option name=\"radv_invariant_geom\" value=\"true\" />\n"
              "    </engine>\n"
              "  </device>\n"
              "  <device driver=\"zink\">\n"
              "    <application name=\"Game\" executable=\"game.exe\">\n"
              "      <option name=\"radv_flush_before_query_copy\" value=\"true\" />\n"
              "    </application>\n"
              "  </device>\n"
              "</driconf>\n");
   /* The device attributes that the static table drops, read from a file. */
   write_file("15-device.conf",
              "<driconf>\n"
              "  <device driver=\"radv\" kernel_driver=\"amdgpu\"><application name=\"D\" executable=\"devtest.exe\">"
              "<option name=\"radv_split_fma\" value=\"true\"/></application></device>\n"
              "  <device driver=\"radv\" device=\"BC-250\"><application name=\"D\" executable=\"devtest.exe\">"
              "<option name=\"radv_flush_before_query_copy\" value=\"true\"/></application></device>\n"
              "  <device driver=\"radv\" device_version_regexp=\"^4[0-9]$\"><application name=\"D\" "
              "executable=\"devtest.exe\"><option name=\"radv_invariant_geom\" value=\"true\"/></application></device>\n"
              "  <device driver=\"radv\" screen=\"1\"><application name=\"D\" executable=\"devtest.exe\">"
              "<option name=\"radv_zero_vram\" value=\"true\"/></application></device>\n"
              "</driconf>\n");
   /* A later file wins. */
   write_file("20-later.conf",
              "<driconf><device driver=\"radv\"><application name=\"Game\" executable=\"game.exe\">"
              "<option name=\"radv_test_level\" value=\"9\"/></application></device></driconf>");
   /* Refused whole: a section without a selector would apply to every application. */
   write_file("30-everyone.conf",
              "<driconf><device driver=\"radv\"><application name=\"All\">"
              "<option name=\"radv_flush_before_query_copy\" value=\"true\"/></application>"
              "<application name=\"Game\" executable=\"game.exe\"><option name=\"radv_test_level\" value=\"50\"/>"
              "</application></device></driconf>");
   /* Refused whole: a misspelt attribute (upstream would apply the section to every application). */
   write_file("31-typo.conf",
              "<driconf><device driver=\"radv\"><application name=\"Game\" exectuable=\"game.exe\">"
              "<option name=\"radv_flush_before_query_copy\" value=\"true\"/></application></device></driconf>");
   /* Refused whole: broken XML after a valid section. */
   write_file("32-broken.conf",
              "<driconf><device driver=\"radv\"><application name=\"Game\" executable=\"other.exe\">"
              "<option name=\"radv_flush_before_query_copy\" value=\"true\"/></application></device>");
   /* Refused whole: a pattern that this driver cannot match. */
   write_file("33-regex.conf",
              "<driconf><device driver=\"radv\"><engine engine_name_match=\"(\\1)\">"
              "<option name=\"radv_flush_before_query_copy\" value=\"true\"/></engine></device></driconf>");
   /* Refused whole: an option outside a section. */
   write_file("34-outside.conf",
              "<driconf><device driver=\"radv\"><option name=\"radv_flush_before_query_copy\" value=\"true\"/>"
              "</device></driconf>");
   /* Not read: not a .conf name. */
   write_file("40-notes.txt",
              "<driconf><device><application name=\"x\" executable=\"game.exe\">"
              "<option name=\"radv_flush_before_query_copy\" value=\"true\"/></application></device></driconf>");
   write_file("41-old.conf.bak",
              "<driconf><device><application name=\"x\" executable=\"game.exe\">"
              "<option name=\"radv_flush_before_query_copy\" value=\"true\"/></application></device></driconf>");

   driconfProfileTestReload();
   expect("", "game.exe", (int[]){1, 0, 0, 0, 9, 0});
   expect("", "Game.exe", (int[]){0, 0, 0, 0, 0, 0});      /* executable= compares the case, as upstream */
   expect("", "other.exe", (int[]){0, 0, 0, 0, 0, 0});
   expect("vkd3d", "witcher3.exe", (int[]){0, 0, 0, 0, 0, 0});
   expect("vkd3d", "cyberpunk2077.exe", (int[]){0, 1, 0, 0, 0, 0});
   expect("DXVK", "factorio.exe", (int[]){0, 1, 0, 0, 0, 1});
   /* The static DXVK section matches "DXVK" anywhere in the name; the file section matches only "DXVK". */
   expect("DXVK2", "factorio.exe", (int[]){0, 1, 0, 0, 0, 0});
   expect_device(&(driConfigFileParseParams){0}, (int[]){0, 0, 0, 0, 0, 0},
                 "no kernel driver, device name or version, screen 0");
   expect_device(&(driConfigFileParseParams){.kernelDriverName = "amdgpu", .deviceName = "BC-250", .deviceVersion = 42,
                                             .screenNum = 1},
                 (int[]){1, 0, 1, 1, 0, 1}, "every device attribute matches");
   expect_device(&(driConfigFileParseParams){.kernelDriverName = "i915", .deviceName = "other", .deviceVersion = 50,
                                             .screenNum = 1},
                 (int[]){0, 0, 0, 1, 0, 0}, "only the screen matches");

   /* 4. The real checks: this directory belongs to the test user, so it is refused and nothing applies. */
   u_driconf_test_trust_all = false;
   driconfProfileTestReload();
   expect("", "game.exe", (int[]){0, 0, 0, 0, 0, 0});
   u_driconf_test_root = NULL;
}

int
main(int argc, char **argv)
{
   if (argc != 2) {
      printf("usage: driconf_profile_test <empty test directory>\n");
      return 2;
   }
   wchar_t base[MAX_PATH];
   MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, base, MAX_PATH);
   test_xml();
   test_access();
   test_files(base);
   printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
   return failures ? 1 : 0;
}
