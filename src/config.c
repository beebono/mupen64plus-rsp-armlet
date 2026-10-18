/*
 * arMLEt - config.c
 *
 * The core-API lookup and the section-version handling follow mupen64plus-rsp-hle's plugin.c.
 *
 * Copyright (C) 2026 arMLEt contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define M64P_PLUGIN_PROTOTYPES 1
#include "m64p_common.h"
#include "m64p_config.h"
#include "m64p_frontend.h"

#include "config.h"

#define CONFIG_API_VERSION 0x020100
#define CONFIG_PARAM_VERSION 1.00f

#define CFG_VERSION "Version"
#define CFG_GFX_MODE "GfxMode"
#define CFG_GFX_UCODES "GfxHleUcodes"
#define CFG_GFX_NATIVE "GfxNative"
#define CFG_GFX_NATIVE_ROUTINES "GfxNativeRoutines"
#define CFG_RDP_COALESCE "RdpCoalesce"
#define CFG_RDP_COALESCE_BYTES "RdpCoalesceBytes"
#define CFG_GFX_PROBE "GfxProbe"
#define CFG_GFX_PROBE_NEAR "GfxProbeNear"
#define CFG_GFX_PROBE_LOOSE_WORDS "GfxProbeLooseWords"
#define CFG_GFX_PROBE_LOOSE_SHADE "GfxProbeLooseShade"
#define CFG_UCODE_CACHE "GfxUcodeCache"
#define CFG_LATCH_POLICY "GfxLatchPolicy"

#define SPLIT(x) (((x) >> 16) & 0xffff), (((x) >> 8) & 0xff), ((x) & 0xff)

static ptr_ConfigOpenSection ConfigOpenSection;
static ptr_ConfigDeleteSection ConfigDeleteSection;
static ptr_ConfigSetParameter ConfigSetParameter;
static ptr_ConfigGetParameter ConfigGetParameter;
static ptr_ConfigSetDefaultFloat ConfigSetDefaultFloat;
static ptr_ConfigSetDefaultString ConfigSetDefaultString;
static ptr_ConfigSetDefaultInt ConfigSetDefaultInt;
static ptr_ConfigGetParamString ConfigGetParamString;
static ptr_ConfigGetParamInt ConfigGetParamInt;
static ptr_ConfigGetUserConfigPath ConfigGetUserConfigPath;

static m64p_handle l_section;

m64p_error armlet_config_startup(m64p_dynlib_handle core)
{
    ptr_CoreGetAPIVersions get_versions;
    int cfg_api, dbg_api, vidext_api;
    float version = 0.0f;

    get_versions = (ptr_CoreGetAPIVersions)dlsym(core, "CoreGetAPIVersions");
    if (!get_versions) {
        armlet_log(M64MSG_ERROR, "core emulator broken; no CoreGetAPIVersions() function found");
        return M64ERR_INCOMPATIBLE;
    }
    get_versions(&cfg_api, &dbg_api, &vidext_api, NULL);
    if ((cfg_api & 0xffff0000) != (CONFIG_API_VERSION & 0xffff0000)) {
        armlet_log(M64MSG_ERROR, "core Config API (v%i.%i.%i) incompatible with plugin (v%i.%i.%i)",
                   SPLIT(cfg_api), SPLIT(CONFIG_API_VERSION));
        return M64ERR_INCOMPATIBLE;
    }

    ConfigOpenSection = (ptr_ConfigOpenSection)dlsym(core, "ConfigOpenSection");
    ConfigDeleteSection = (ptr_ConfigDeleteSection)dlsym(core, "ConfigDeleteSection");
    ConfigSetParameter = (ptr_ConfigSetParameter)dlsym(core, "ConfigSetParameter");
    ConfigGetParameter = (ptr_ConfigGetParameter)dlsym(core, "ConfigGetParameter");
    ConfigSetDefaultFloat = (ptr_ConfigSetDefaultFloat)dlsym(core, "ConfigSetDefaultFloat");
    ConfigSetDefaultString = (ptr_ConfigSetDefaultString)dlsym(core, "ConfigSetDefaultString");
    ConfigGetParamString = (ptr_ConfigGetParamString)dlsym(core, "ConfigGetParamString");
    ConfigSetDefaultInt = (ptr_ConfigSetDefaultInt)dlsym(core, "ConfigSetDefaultInt");
    ConfigGetParamInt = (ptr_ConfigGetParamInt)dlsym(core, "ConfigGetParamInt");
    ConfigGetUserConfigPath = (ptr_ConfigGetUserConfigPath)dlsym(core, "ConfigGetUserConfigPath");     /* (optional: the cache path falls back to $XDG_CONFIG_HOME/mupen64plus) */
    if (!ConfigOpenSection || !ConfigDeleteSection || !ConfigSetParameter || !ConfigGetParameter ||
        !ConfigSetDefaultFloat || !ConfigSetDefaultString || !ConfigGetParamString || !ConfigSetDefaultInt || !ConfigGetParamInt)
        return M64ERR_INCOMPATIBLE;

    if (ConfigOpenSection(ARMLET_CONFIG_SECTION, &l_section) != M64ERR_SUCCESS) {
        armlet_log(M64MSG_ERROR, "couldn't open config section '" ARMLET_CONFIG_SECTION "'");
        return M64ERR_INPUT_NOT_FOUND;
    }
    if (ConfigGetParameter(l_section, CFG_VERSION, M64TYPE_FLOAT, &version, sizeof(float)) != M64ERR_SUCCESS) {
        armlet_log(M64MSG_WARNING, "no version number in '" ARMLET_CONFIG_SECTION "' config section; setting defaults");
        ConfigDeleteSection(ARMLET_CONFIG_SECTION);
        ConfigOpenSection(ARMLET_CONFIG_SECTION, &l_section);
    } else if ((int)version != (int)CONFIG_PARAM_VERSION) {
        armlet_log(M64MSG_WARNING, "incompatible version %.2f in '" ARMLET_CONFIG_SECTION "' config section (current %.2f); setting defaults",
                   version, CONFIG_PARAM_VERSION);
        ConfigDeleteSection(ARMLET_CONFIG_SECTION);
        ConfigOpenSection(ARMLET_CONFIG_SECTION, &l_section);
    } else if (CONFIG_PARAM_VERSION - version >= 0.0001f) {
        float v = CONFIG_PARAM_VERSION;
        ConfigSetParameter(l_section, CFG_VERSION, M64TYPE_FLOAT, &v);
        armlet_log(M64MSG_INFO, "updating parameter set version in '" ARMLET_CONFIG_SECTION "' config section to %.2f", v);
    }

    ConfigSetDefaultFloat(l_section, CFG_VERSION, CONFIG_PARAM_VERSION, "arMLEt config parameter version number");
    ConfigSetDefaultString(l_section, CFG_GFX_MODE, "hle",
        "Graphics microcode: lle = every graphics task runs on parallel-rsp's JIT; "
        "hle = tasks whose microcode has a native task-level handler (src/hle) run natively, the rest run as lle");
    ConfigSetDefaultString(l_section, CFG_GFX_UCODES, "",
        "Comma-separated 64-bit ucode ids (hex, as logged) allowed to run natively in GfxMode=hle; empty = all with a handler");
    ConfigSetDefaultString(l_section, CFG_GFX_NATIVE, "off",
        "Native microcode routines (vertex transform, triangle setup) in place of the JIT's code: off, on, or verify "
        "(runs the JIT as well and compares; for testing)");
    ConfigSetDefaultString(l_section, CFG_GFX_NATIVE_ROUTINES, "",
        "Comma-separated routine names or families (f3dex2, f3dex2.vtx ...) that may run natively; -name excludes; empty = all");
    ConfigSetDefaultString(l_section, CFG_RDP_COALESCE, "auto",
        "Coalesce the video plugin's ProcessRdpList calls of graphics ucode (exact, see docs): auto = on when the video plugin is foir, on, off");
    ConfigSetDefaultInt(l_section, CFG_RDP_COALESCE_BYTES, ARMLET_COALESCE_BYTES_DEFAULT,
        "Bytes of pending RDP command list at which a coalesced list is handed to the video plugin");
    ConfigSetDefaultString(l_section, CFG_GFX_PROBE, "on",
        "GfxMode=hle only: derive the build record of a graphics ucode that has no table entry and probe it with canned display lists against the JIT: off, on, "
        "loose (on, and a NONE derivation within GfxProbeLooseWords is probed and latched too, with mismatches graded; see docs/design.md, The loose tier)");
    ConfigSetDefaultInt(l_section, CFG_GFX_PROBE_LOOSE_WORDS, 16,
        "GfxProbe=loose: unexplained plus missing words a NONE derivation may have and still be probed");
    ConfigSetDefaultInt(l_section, CFG_GFX_PROBE_LOOSE_SHADE, 8,
        "GfxProbe=loose: colour levels a latched loose build's shade coefficients and vertex colours may differ from the JIT's by without a strike");
    ConfigSetDefaultString(l_section, CFG_GFX_PROBE_NEAR, "strict",
        "Probe of a NEAR derivation: strict (the canned tasks must come out identical) or tolerant (shade coefficients and vertex colours may differ by a couple of levels)");
    ConfigSetDefaultString(l_section, CFG_UCODE_CACHE, "",
        "File of what the probe learned (armlet-ucode.cache in the core's config directory when empty; off = none)");
    ConfigSetDefaultString(l_section, CFG_LATCH_POLICY, "",
        "GfxProbe: when a derived build runs natively (empty = the defaults): evidence=24,distinct=8,near=1.75,strikes=2,sample=32,sample_last=256,decay=64,queue=4,fingerprints=1024,loose=2.5,default=3,repeat=0.05,override=4,resume=128 (see docs/options.md)");
    return M64ERR_SUCCESS;
}

void armlet_config_shutdown(void)
{
    l_section = NULL;
}

static void parse_ucodes(struct armlet_config *cfg, const char *s)
{
    cfg->n_allow = 0;
    while (*s) {
        char *end;
        uint64_t id;
        while (*s && !isxdigit((unsigned char)*s))
            s++;
        if (!*s)
            break;
        id = strtoull(s, &end, 16);
        if (cfg->n_allow < ARMLET_MAX_ALLOW)
            cfg->allow[cfg->n_allow++] = id;
        else
            armlet_log(M64MSG_WARNING, "more than %d ucode ids in GfxHleUcodes, ignoring the rest", ARMLET_MAX_ALLOW);
        s = end;
    }
}

/* the core's user config directory (--configdir), or $XDG_CONFIG_HOME/mupen64plus, or $HOME/.config/mupen64plus; "" when none of them is known */
const char *armlet_config_user_path(void)
{
    static char path[512];
    const char *p = ConfigGetUserConfigPath ? ConfigGetUserConfigPath() : NULL, *x, *h;
    if (p && *p) {
        snprintf(path, sizeof(path), "%s", p);
    } else if ((x = getenv("XDG_CONFIG_HOME")) && *x) {
        snprintf(path, sizeof(path), "%s/mupen64plus/", x);
    } else if ((h = getenv("HOME")) && *h) {
        snprintf(path, sizeof(path), "%s/.config/mupen64plus/", h);
    } else
        path[0] = 0;
    return path;
}

void armlet_config_load(struct armlet_config *cfg)
{
    const char *mode = l_section ? ConfigGetParamString(l_section, CFG_GFX_MODE) : "hle";
    const char *ucodes = l_section ? ConfigGetParamString(l_section, CFG_GFX_UCODES) : "";
    const char *native = l_section ? ConfigGetParamString(l_section, CFG_GFX_NATIVE) : "off";
    const char *nroutines = l_section ? ConfigGetParamString(l_section, CFG_GFX_NATIVE_ROUTINES) : "";
    const char *probe = l_section ? ConfigGetParamString(l_section, CFG_GFX_PROBE) : "on";
    const char *pnear = l_section ? ConfigGetParamString(l_section, CFG_GFX_PROBE_NEAR) : "strict";
    int lwords = l_section ? ConfigGetParamInt(l_section, CFG_GFX_PROBE_LOOSE_WORDS) : 16;
    int lshade = l_section ? ConfigGetParamInt(l_section, CFG_GFX_PROBE_LOOSE_SHADE) : 8;
    const char *ucache = l_section ? ConfigGetParamString(l_section, CFG_UCODE_CACHE) : "";
    const char *lpol = l_section ? ConfigGetParamString(l_section, CFG_LATCH_POLICY) : "";
    const char *coalesce = l_section ? ConfigGetParamString(l_section, CFG_RDP_COALESCE) : "auto";
    int bytes = l_section ? ConfigGetParamInt(l_section, CFG_RDP_COALESCE_BYTES) : ARMLET_COALESCE_BYTES_DEFAULT;
    const char *env;

    if ((env = getenv("ARMLET_GFX")) && *env)
        mode = env;
    if ((env = getenv("ARMLET_RDP_COALESCE")) && *env)
        coalesce = env;
    if ((env = getenv("ARMLET_RDP_COALESCE_BYTES")) && *env)
        bytes = atoi(env);
    if ((env = getenv("ARMLET_UCODES")))
        ucodes = env;
    if ((env = getenv("ARMLET_NATIVE")) && *env)
        native = env;
    if ((env = getenv("ARMLET_NATIVE_ROUTINES")))
        nroutines = env;
    if ((env = getenv("ARMLET_PROBE")) && *env)
        probe = env;
    if ((env = getenv("ARMLET_PROBE_NEAR")) && *env)
        pnear = env;
    if ((env = getenv("ARMLET_PROBE_LOOSE_WORDS")) && *env)
        lwords = atoi(env);
    if ((env = getenv("ARMLET_PROBE_LOOSE_SHADE")) && *env)
        lshade = atoi(env);
    if ((env = getenv("ARMLET_UCODE_CACHE")))
        ucache = env;
    if ((env = getenv("ARMLET_LATCH_POLICY")))
        lpol = env;

    memset(cfg, 0, sizeof(*cfg));
    if (mode && !strcasecmp(mode, "hle"))
        cfg->gfx_mode = ARMLET_GFX_HLE;
    else if (mode && strcasecmp(mode, "lle"))
        armlet_log(M64MSG_WARNING, "unknown GfxMode '%s' (lle or hle); using lle", mode);
    parse_ucodes(cfg, ucodes ? ucodes : "");

    cfg->native_mode = ARMLET_NATIVE_OFF;
    if (native && (!strcasecmp(native, "on") || !strcmp(native, "1") || !strcasecmp(native, "true")))
        cfg->native_mode = ARMLET_NATIVE_ON;
    else if (native && !strcasecmp(native, "verify"))
        cfg->native_mode = ARMLET_NATIVE_VERIFY;
    else if (native && strcasecmp(native, "off") && strcmp(native, "0") && strcasecmp(native, "false"))
        armlet_log(M64MSG_WARNING, "unknown GfxNative '%s' (off, on or verify); using off", native);
    if (nroutines)
        snprintf(cfg->native_routines, sizeof(cfg->native_routines), "%s", nroutines);

    if (probe && (!strcasecmp(probe, "on") || !strcmp(probe, "1") || !strcasecmp(probe, "true")))
        cfg->probe = 1;
    else if (probe && !strcasecmp(probe, "loose"))
        cfg->probe = cfg->probe_loose = 1;
    else if (probe && strcasecmp(probe, "off") && strcmp(probe, "0") && strcasecmp(probe, "false") && *probe)
        armlet_log(M64MSG_WARNING, "unknown GfxProbe '%s' (off, on or loose); using off", probe);
    cfg->probe_loose_words = lwords < 0 ? 0 : lwords > 4096 ? 4096 : (unsigned)lwords;
    cfg->probe_loose_shade = lshade < 2 ? 2 : lshade > 255 ? 255 : (unsigned)lshade;
    cfg->probe_tol = pnear && !strcasecmp(pnear, "tolerant");
    if (pnear && strcasecmp(pnear, "strict") && strcasecmp(pnear, "tolerant") && *pnear)
        armlet_log(M64MSG_WARNING, "unknown GfxProbeNear '%s' (strict or tolerant); using strict", pnear);
    cfg->probe_latch = (env = getenv("ARMLET_PROBE_LATCH")) && *env && *env != '0';
    cfg->probe_force = (env = getenv("ARMLET_PROBE_FORCE")) && *env && *env != '0';
    cfg->probe_loose_force = cfg->probe_loose && (env = getenv("ARMLET_PROBE_LOOSE_FORCE")) && *env && *env != '0';
    cfg->probe_default = cfg->probe_loose && !((env = getenv("ARMLET_PROBE_DEFAULT")) && *env && *env == '0');
    cfg->probe_none_x = (env = getenv("ARMLET_PROBE_NONE_X")) && *env ? (unsigned)atoi(env) : 0;
    cfg->probe_seeds = (env = getenv("ARMLET_PROBE_SEEDS")) && *env ? (unsigned)atoi(env) : 0;
    if (cfg->probe_seeds > 40)
        cfg->probe_seeds = 40;
    latch_cfg_default(&cfg->latch);
    {
        const char *bad = NULL;
        if (lpol && *lpol && latch_cfg_parse(&cfg->latch, lpol, &bad))
            armlet_log(M64MSG_WARNING, "GfxLatchPolicy: cannot read '%.40s' (keys: evidence, distinct, near, strikes, sample, sample_last, decay, queue, fingerprints, loose, default, repeat, override, resume)", bad ? bad : lpol);
    }
    if (ucache && *ucache)
        snprintf(cfg->ucode_cache, sizeof(cfg->ucode_cache), "%s", ucache);
    else if (cfg->probe) {
        const char *d = armlet_config_user_path();
        if (*d)
            snprintf(cfg->ucode_cache, sizeof(cfg->ucode_cache), "%s%sarmlet-ucode.cache", d, d[strlen(d) - 1] == '/' ? "" : "/");
    }

    cfg->rdp_coalesce_mode = ARMLET_COALESCE_AUTO;
    if (coalesce && (!strcasecmp(coalesce, "off") || !strcmp(coalesce, "0") || !strcasecmp(coalesce, "false")))
        cfg->rdp_coalesce_mode = ARMLET_COALESCE_OFF;
    else if (coalesce && (!strcasecmp(coalesce, "on") || !strcmp(coalesce, "1") || !strcasecmp(coalesce, "true")))
        cfg->rdp_coalesce_mode = ARMLET_COALESCE_ON;
    else if (coalesce && strcasecmp(coalesce, "auto"))
        armlet_log(M64MSG_WARNING, "unknown RdpCoalesce '%s' (auto, on or off); using auto", coalesce);
    if (bytes < 8 || bytes > 0x100000) {
        armlet_log(M64MSG_WARNING, "RdpCoalesceBytes %d out of range (8 to 1048576); using %d", bytes, ARMLET_COALESCE_BYTES_DEFAULT);
        bytes = ARMLET_COALESCE_BYTES_DEFAULT;
    }
    cfg->rdp_coalesce_bytes = (unsigned)bytes;
}
