#pragma once

// This file will be included in "fastfetch.h", do NOT put unnecessary things here

#include "common/option.h"

typedef struct FFTitleOptions
{
    const char* moduleName;
    FFModuleArgs moduleArgs;

    bool fqdn;
    FFstrbuf colorUser;
    FFstrbuf colorAt;
    FFstrbuf colorHost;
} FFTitleOptions;
