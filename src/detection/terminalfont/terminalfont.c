#include "terminalfont.h"
#include "common/properties.h"
#include "common/processing.h"
#include "detection/internal.h"
#include "detection/terminalshell/terminalshell.h"

static void detectAlacritty(FFTerminalFontResult* terminalFont)
{
    FF_STRBUF_AUTO_DESTROY fontName = ffStrbufCreate();
    FF_STRBUF_AUTO_DESTROY fontSize = ffStrbufCreate();

    FFpropquery fontQuery[] = {
        {"family:", &fontName},
        {"size:", &fontSize},
    };

    // alacritty parses config files in this order
    ffParsePropFileConfigValues("alacritty/alacritty.yml", 2, fontQuery);
    if(fontName.length == 0 || fontSize.length == 0)
        ffParsePropFileConfigValues("alacritty.yml", 2, fontQuery);
    if(fontName.length == 0 || fontSize.length == 0)
        ffParsePropFileConfigValues(".alacritty.yml", 2, fontQuery);

    //by default alacritty uses its own font called alacritty
    if(fontName.length == 0)
        ffStrbufAppendS(&fontName, "alacritty");

    // the default font size is 11
    if(fontSize.length == 0)
        ffStrbufAppendS(&fontSize, "11");

    ffFontInitValues(&terminalFont->font, fontName.chars, fontSize.chars);
}

FF_MAYBE_UNUSED static void detectTTY(FFTerminalFontResult* terminalFont)
{
    FF_STRBUF_AUTO_DESTROY fontName = ffStrbufCreate();

    ffParsePropFile(FASTFETCH_TARGET_DIR_ETC"/vconsole.conf", "Font =", &fontName);

    if(fontName.length == 0)
    {
        ffStrbufAppendS(&fontName, "VGA default kernel font ");
        ffProcessAppendStdOut(&fontName, (char* const[]){
            "showconsolefont",
            "--info",
            NULL
        });

        ffStrbufTrimRight(&fontName, ' ');
    }

    if(fontName.length > 0)
        ffFontInitCopy(&terminalFont->font, fontName.chars);
    else
        ffStrbufAppendS(&terminalFont->error, "Couldn't find Font in "FASTFETCH_TARGET_DIR_ETC"/vconsole.conf");
}

#if defined(_WIN32) || defined(__linux__)

#include "common/library.h"
#include "common/processing.h"

#include <stdlib.h>
#include <yyjson.h>

static const char* detectWTProfile(yyjson_val* profile, FFstrbuf* name, double* size)
{
    yyjson_val* font = yyjson_obj_get(profile, "font");
    if (!font)
        return "yyjson_obj_get(profile, \"font\"); failed";

    if (!yyjson_is_obj(font))
        return "yyjson_is_obj(font) returns false";

    if (name->length == 0)
    {
        yyjson_val* pface = yyjson_obj_get(font, "face");
        if(yyjson_is_str(pface))
            ffStrbufAppendS(name, unsafe_yyjson_get_str(pface));
    }

    if (*size < 0)
    {
        yyjson_val* psize = yyjson_obj_get(font, "size");
        if (yyjson_is_num(psize))
            *size = yyjson_get_num(psize);
    }
    return NULL;
}

static inline void wrapYyjsonFree(yyjson_doc** doc)
{
    assert(doc);
    if (*doc)
        yyjson_doc_free(*doc);
}

static const char* detectFromWTImpl(FFstrbuf* content, FFstrbuf* name, double* size)
{
    yyjson_doc* __attribute__((__cleanup__(wrapYyjsonFree))) doc = yyjson_read_opts(content->chars, content->length, YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS | YYJSON_READ_ALLOW_INF_AND_NAN, NULL, NULL);
    if (!doc)
        return "Failed to parse WT JSON config file";

    yyjson_val* const root = yyjson_doc_get_root(doc);
    assert(root);

    yyjson_val* profiles = yyjson_obj_get(root, "profiles");
    if (!profiles)
        return "yyjson_obj_get(root, \"profiles\") failed";

    FF_STRBUF_AUTO_DESTROY wtProfileId = ffStrbufCreateS(getenv("WT_PROFILE_ID"));
    ffStrbufTrim(&wtProfileId, '\'');
    if (wtProfileId.length > 0)
    {
        yyjson_val* list = yyjson_obj_get(profiles, "list");
        if (yyjson_is_arr(list))
        {
            yyjson_val* profile;
            size_t idx, max;
            yyjson_arr_foreach(list, idx, max, profile)
            {
                yyjson_val* guid = yyjson_obj_get(profile, "guid");

                if(ffStrbufEqualS(&wtProfileId, yyjson_get_str(guid)))
                {
                    detectWTProfile(profile, name, size);
                    break;
                }
            }
        }
    }

    yyjson_val* defaults = yyjson_obj_get(profiles, "defaults");
    if (defaults)
        detectWTProfile(defaults, name, size);

    if(name->length == 0)
        ffStrbufSetS(name, "Cascadia Mono");
    if(*size < 0)
        *size = 12;
    return NULL;
}

#ifdef _WIN32
    #include "common/io/io.h"

    #include <shlobj.h>
#endif

static void detectFromWindowsTeriminal(const FFstrbuf* terminalExe, FFTerminalFontResult* terminalFont)
{
    //https://learn.microsoft.com/en-us/windows/terminal/install#settings-json-file
    FF_STRBUF_AUTO_DESTROY json = ffStrbufCreate();
    const char* error = NULL;

    #ifdef _WIN32
    if(terminalExe && terminalExe->length > 0 && !ffStrbufEqualS(terminalExe, "Windows Terminal"))
    {
        char jsonPath[MAX_PATH + 1];
        if(SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, jsonPath)))
        {
            size_t remaining = sizeof(jsonPath) - strlen(jsonPath) - 1;
            if(ffStrbufContainIgnCaseS(terminalExe, "_8wekyb3d8bbwe\\"))
            {
                // Microsoft Store version
                if(ffStrbufContainIgnCaseS(terminalExe, ".WindowsTerminalPreview_"))
                {
                    // Preview version
                    strncat(jsonPath, "\\Packages\\Microsoft.WindowsTerminalPreview_8wekyb3d8bbwe\\LocalState\\settings.json", remaining);
                    if(!ffAppendFileBuffer(jsonPath, &json))
                        error = "Error reading Windows Terminal Preview settings JSON file";
                }
                else
                {
                    // Stable version
                    strncat(jsonPath, "\\Packages\\Microsoft.WindowsTerminal_8wekyb3d8bbwe\\LocalState\\settings.json", remaining);
                    if(!ffAppendFileBuffer(jsonPath, &json))
                        error = "Error reading Windows Terminal settings JSON file";
                }
            }
            else
            {
                strncat(jsonPath, "\\Microsoft\\Windows Terminal\\settings.json", remaining);
                if(!ffAppendFileBuffer(jsonPath, &json))
                    error = "Error reading Windows Terminal settings JSON file";
            }
        }
    }
    #else
    FF_UNUSED(terminalExe);
    #endif

    if(!error && json.length == 0)
    {
        error = ffProcessAppendStdOut(&json, (char* const[]) {
            "cmd.exe",
            "/c",
            //print the file content directly, so we don't need to handle the difference of Windows and POSIX path
            "if exist %LOCALAPPDATA%\\Packages\\Microsoft.WindowsTerminal_8wekyb3d8bbwe\\LocalState\\settings.json "
            "( type %LOCALAPPDATA%\\Packages\\Microsoft.WindowsTerminal_8wekyb3d8bbwe\\LocalState\\settings.json ) "
            "else if exist %LOCALAPPDATA%\\Packages\\Microsoft.WindowsTerminalPreview_8wekyb3d8bbwe\\LocalState\\settings.json "
            "( type %LOCALAPPDATA%\\Packages\\Microsoft.WindowsTerminalPreview_8wekyb3d8bbwe\\LocalState\\settings.json ) "
            "else if exist \"%LOCALAPPDATA%\\Microsoft\\Windows Terminal\\settings.json\" "
            "( type %LOCALAPPDATA%\\Microsoft\\Windows Terminal\\settings.json ) "
            "else ( call )",
            NULL
        });
    }

    if(error)
    {
        ffStrbufAppendS(&terminalFont->error, error);
        return;
    }
    ffStrbufTrimRight(&json, '\n');
    if(json.length == 0)
    {
        ffStrbufAppendS(&terminalFont->error, "Cannot find file \"settings.json\"");
        return;
    }

    FF_STRBUF_AUTO_DESTROY name = ffStrbufCreate();
    double size = -1;
    error = detectFromWTImpl(&json, &name, &size);

    if(error)
        ffStrbufAppendS(&terminalFont->error, error);
    else
    {
        char sizeStr[16];
        snprintf(sizeStr, sizeof(sizeStr), "%g", size);
        ffFontInitValues(&terminalFont->font, name.chars, sizeStr);
    }
}



#endif //defined(_WIN32) || defined(__linux__)

FF_MAYBE_UNUSED static bool detectKitty(FFTerminalFontResult* result)
{
    FF_STRBUF_AUTO_DESTROY fontName = ffStrbufCreate();
    FF_STRBUF_AUTO_DESTROY fontSize = ffStrbufCreate();

    FFpropquery fontQuery[] = {
        {"font_family ", &fontName},
        {"font_size ", &fontSize},
    };

    if(!ffParsePropFileConfigValues("kitty/kitty.conf", 2, fontQuery))
        return false;

    if(fontName.length == 0)
        ffStrbufSetS(&fontName, "monospace");
    if(fontSize.length == 0)
        ffStrbufSetS(&fontSize, "11.0");

    ffFontInitValues(&result->font, fontName.chars, fontSize.chars);

    return true;
}

static void detectTerminator(FFTerminalFontResult* result)
{
    FF_STRBUF_AUTO_DESTROY useSystemFont = ffStrbufCreate();
    FF_STRBUF_AUTO_DESTROY fontName = ffStrbufCreate();

    FFpropquery fontQuery[] = {
        {"use_system_font =", &useSystemFont},
        {"font =", &fontName},
    };

    if(!ffParsePropFileConfigValues("terminator/config", 2, fontQuery))
    {
        ffStrbufAppendS(&result->error, "Couldn't read Terminator config file");
        return;
    }

    if(ffStrbufIgnCaseEqualS(&useSystemFont, "True"))
    {
        ffFontInitCopy(&result->font, "System");
        return;
    }

    if(fontName.length == 0)
        ffFontInitValues(&result->font, "mono", "8");
    else
        ffFontInitPango(&result->font, fontName.chars);
}

static bool detectWezterm(FFTerminalFontResult* result)
{
    FF_STRBUF_AUTO_DESTROY fontName = ffStrbufCreate();

    ffStrbufSetS(&result->error, ffProcessAppendStdOut(&fontName, (char* const[]){
        "wezterm",
        "ls-fonts",
        "--text",
        "a",
        NULL
    }));
    if(result->error.length)
        return false;

    //LeftToRight
    // 0 a    \u{61}       x_adv=7  cells=1  glyph=a,180  wezterm.font("JetBrains Mono", {weight="Regular", stretch="Normal", style="Normal"})
    //                                      <built-in>, BuiltIn
    ffStrbufSubstrAfterFirstC(&fontName, '"');
    ffStrbufSubstrBeforeFirstC(&fontName, '"');

    if(!fontName.length)
        return false;

    ffFontInitCopy(&result->font, fontName.chars);
    return true;
}

static bool detectTabby(FFTerminalFontResult* result)
{
    FF_STRBUF_AUTO_DESTROY fontName = ffStrbufCreate();
    FF_STRBUF_AUTO_DESTROY fontSize = ffStrbufCreate();

    FFpropquery fontQuery[] = {
        {"font: ", &fontName},
        {"fontSize: ", &fontSize},
    };

    if(!ffParsePropFileConfigValues("tabby/config.yaml", 2, fontQuery))
        return false;

    if(fontName.length == 0)
        ffStrbufSetS(&fontName, "monospace");
    if(fontSize.length == 0)
        ffStrbufSetS(&fontSize, "14");

    ffFontInitValues(&result->font, fontName.chars, fontSize.chars);

    return true;
}

void ffDetectTerminalFontPlatform(const FFTerminalShellResult* terminalShell, FFTerminalFontResult* terminalFont);

static bool detectTerminalFontCommon(const FFTerminalShellResult* terminalShell, FFTerminalFontResult* terminalFont)
{
    if(ffStrbufStartsWithIgnCaseS(&terminalShell->terminalProcessName, "alacritty"))
        detectAlacritty(terminalFont);
    else if(ffStrbufStartsWithIgnCaseS(&terminalShell->terminalProcessName, "terminator"))
        detectTerminator(terminalFont);
    else if(ffStrbufStartsWithIgnCaseS(&terminalShell->terminalProcessName, "wezterm-gui"))
        detectWezterm(terminalFont);
    else if(ffStrbufStartsWithIgnCaseS(&terminalShell->terminalProcessName, "tabby"))
        detectTabby(terminalFont);

    #ifndef _WIN32
    else if(ffStrbufStartsWithIgnCaseS(&terminalShell->terminalExe, "/dev/pts/"))
        ffStrbufAppendS(&terminalFont->error, "Terminal font detection is not supported on PTS");
    else if(ffStrbufIgnCaseEqualS(&terminalShell->terminalPrettyName, "kitty"))
        detectKitty(terminalFont);
    else if(ffStrbufStartsWithIgnCaseS(&terminalShell->terminalExe, "/dev/tty"))
        detectTTY(terminalFont);
    #endif

    #if defined(_WIN32) || defined(__linux__)
    //Used by both Linux (WSL) and Windows
    else if(ffStrbufIgnCaseEqualS(&terminalShell->terminalProcessName, "Windows Terminal") ||
        ffStrbufIgnCaseEqualS(&terminalShell->terminalProcessName, "WindowsTerminal.exe"))
        detectFromWindowsTeriminal(&terminalShell->terminalExe, terminalFont);
    #endif

    else
        return false;

    return true;
}

bool ffDetectTerminalFont(FFTerminalFontResult* result)
{
    const FFTerminalShellResult* terminalShell = ffDetectTerminalShell();

    if(terminalShell->terminalProcessName.length == 0)
        ffStrbufAppendS(&result->error, "Terminal font needs successful terminal detection");

    else if(!detectTerminalFontCommon(terminalShell, result))
        ffDetectTerminalFontPlatform(terminalShell, result);

    if(result->error.length == 0 && result->font.pretty.length == 0)
        ffStrbufAppendF(&result->error, "Unknown terminal: %s", terminalShell->terminalProcessName.chars);

    return result->error.length == 0;
}
