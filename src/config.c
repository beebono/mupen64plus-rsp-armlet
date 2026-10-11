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

#define SPLIT(x) (((x) >> 16) & 0xffff), (((x) >> 8) & 0xff), ((x) & 0xff)

static ptr_ConfigOpenSection ConfigOpenSection;
static ptr_ConfigDeleteSection ConfigDeleteSection;
static ptr_ConfigSetParameter ConfigSetParameter;
static ptr_ConfigGetParameter ConfigGetParameter;
static ptr_ConfigSetDefaultFloat ConfigSetDefaultFloat;
static ptr_ConfigSetDefaultString ConfigSetDefaultString;
static ptr_ConfigGetParamString ConfigGetParamString;

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
    if (!ConfigOpenSection || !ConfigDeleteSection || !ConfigSetParameter || !ConfigGetParameter ||
        !ConfigSetDefaultFloat || !ConfigSetDefaultString || !ConfigGetParamString)
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
    ConfigSetDefaultString(l_section, CFG_GFX_MODE, "lle",
        "Graphics microcode: lle = every graphics task runs on parallel-rsp's JIT; "
        "hle = tasks whose microcode has a native handler run natively (none yet), the rest run as lle");
    ConfigSetDefaultString(l_section, CFG_GFX_UCODES, "",
        "Comma-separated 64-bit ucode ids (hex, as logged) allowed to run natively in GfxMode=hle; empty = all with a handler");
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

void armlet_config_load(struct armlet_config *cfg)
{
    const char *mode = l_section ? ConfigGetParamString(l_section, CFG_GFX_MODE) : "lle";
    const char *ucodes = l_section ? ConfigGetParamString(l_section, CFG_GFX_UCODES) : "";
    const char *env;

    if ((env = getenv("ARMLET_GFX")) && *env)
        mode = env;
    if ((env = getenv("ARMLET_UCODES")))
        ucodes = env;

    memset(cfg, 0, sizeof(*cfg));
    if (mode && !strcasecmp(mode, "hle"))
        cfg->gfx_mode = ARMLET_GFX_HLE;
    else if (mode && strcasecmp(mode, "lle"))
        armlet_log(M64MSG_WARNING, "unknown GfxMode '%s' (lle or hle); using lle", mode);
    parse_ucodes(cfg, ucodes ? ucodes : "");
}
