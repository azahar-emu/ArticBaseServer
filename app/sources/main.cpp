
#include <stdio.h>
#include <stdlib.h>
#include "sys/stat.h"
#include <string.h>
#include <memory.h>

#include "Main.hpp"
#include "plgldr.h"
#include "BCLIM.hpp"
#include "logo.h"
#include "plugin.h"
#include "3gx.h"

Logger logger;

const char* artic_base_plugin = "/3ds/ArticBase/ArticBase.3gx";

char *strdup(const char *s) {
	char *d = (char*)malloc(strlen(s) + 1);
	if (d == NULL) return NULL;
	strcpy(d, s);
	return d;
}

FILE* fopen_mkdir(const char* name, const char* mode, bool actuallyOpen = true)
{
	char*	_path = strdup(name);
	char    *p;
	FILE*	retfile = NULL;

	errno = 0;
	for (p = _path + 1; *p; p++)
	{
		if (*p == '/')
		{
			*p = '\0';
			if (mkdir(_path, 777) != 0)
				if (errno != EEXIST) goto error;
			*p = '/';
		}
	}
	if (actuallyOpen) retfile = fopen(name, mode);
error:
	free(_path);
	return retfile;
}

bool extractPlugin() {
    u32 expectedVersion = SYSTEM_VERSION(VERSION_MAJOR, VERSION_MINOR, VERSION_REVISION);
    bool plugin_needs_update = true;
    FILE* f = fopen(artic_base_plugin, "r");
    if (f) {
        _3gx_Header header;
        int read = fread(&header, 1, sizeof(header), f);
        if (read == sizeof(header) && header.magic == _3GX_MAGIC) {
            plugin_needs_update = header.version != expectedVersion;
        }
    }
    if (f) fclose(f);

    if (plugin_needs_update) {
        logger.Info("Updating Artic Base plugin file");
        f = fopen_mkdir(artic_base_plugin, "w");
        if (!f) {
            logger.Error("Cannot open plugin file");
            return false;
        }
        int written = fwrite(plugin_ArticBase_3gx, 1, plugin_ArticBase_3gx_size, f);
        fclose(f);
        if (written != plugin_ArticBase_3gx_size) {
            logger.Error("Cannot write plugin file");
            return false;
        }
    }
    return true;
}

bool launchPlugin() {
    
    // Make sure the luma plugins directory exists
    // so that persistent user parameters work
    fopen_mkdir("/luma/plugins/a.txt", "w", false);

	u32 ret = 0;
	PluginLoadParameters plgparam = { 0 };
	u8 isPlgEnabled = 0;

	plgparam.noFlash = false;
	plgparam.pluginMemoryStrategy = PLG_STRATEGY_NONE;
    plgparam.persistent = 1;
	plgparam.lowTitleId = 0;
    strcpy(plgparam.path, artic_base_plugin);

	ret = plgLdrInit();
	if (R_FAILED(ret)) {
        logger.Error("Cannot start plugin loader");
        return false;
    }
    u32 version;
    ret = PLGLDR__GetVersion(&version);
    if (R_FAILED(ret)) {
        logger.Error("Plugin loader error");
        plgLdrExit();
        return false;
    }
    if (version < SYSTEM_VERSION(1,0,2)) {
        logger.Error("Unsupported plugin loader version,");
        logger.Error("please update Luma3DS");
        plgLdrExit();
        return false;
    }
	ret = PLGLDR__IsPluginLoaderEnabled((bool*)&isPlgEnabled);
    if (R_FAILED(ret)) {
        logger.Error("Plugin loader error");
        plgLdrExit();
        return false;
    }
	plgparam.config[0] = isPlgEnabled;
	ret = PLGLDR__SetPluginLoaderState(true);
	if (R_FAILED(ret)) {
        logger.Error("Cannot enable plugin loader");
        plgLdrExit();
        return false;
    }
	ret = PLGLDR__SetPluginLoadParameters(&plgparam);
    plgLdrExit();
	if (R_FAILED(ret)) {
        logger.Error("Plugin loader error");
        return false;
    }
    return true;
}

PrintConsole topScreenConsole, bottomScreenConsole;
int transferedBytes = 0;
void Main() {
    logger.Start();
    logger.debug_enable = true;

    gfxInitDefault();
	consoleInit(GFX_TOP, &topScreenConsole);
    consoleInit(GFX_BOTTOM, &bottomScreenConsole);
    topScreenConsole.bg = 15; topScreenConsole.fg = 0;
    bottomScreenConsole.bg = 15; bottomScreenConsole.fg = 0;

    gfxSetDoubleBuffering(GFX_BOTTOM, false);

    aptSetHomeAllowed(false);

    consoleSelect(&bottomScreenConsole);
    consoleClear();
    consoleSelect(&topScreenConsole);
    consoleClear();

    {
        CTRPluginFramework::BCLIM((void*)__data_logo_bin, __data_logo_bin_size).Render(CTRPluginFramework::Rect<int>((320 - 128) / 2, (240 - 128) / 2, 128, 128));
    }
    logger.Raw(false, "\n             ArticBase v%d.%d.%d\n", VERSION_MAJOR, VERSION_MINOR, VERSION_REVISION);
    logger.Raw(false, "    Press A to launch Artic Base.");
    logger.Raw(false, "    Press L to enable logs at the bottom\n    (useful for broken top screens).");
    logger.Raw(false, "    Press B or START to exit.");
    logger.Raw(true, "");
    logger.Info("Welcome to Artic Base!\n    Check bottom screen for controls.");

    while (aptMainLoop())
	{
		//Scan all the inputs. This should be done once for each frame
		hidScanInput();

		//hidKeysDown returns information about which buttons have been just pressed (and they weren't in the previous frame)
		u32 kDown = hidKeysDown();

        if (kDown & (KEY_B | KEY_START)) {
            break;
        }

        if (kDown & (KEY_L)) {
        logger.BrokenScreen();
	    logger.Info("Logs will be written here.");
        }

        if (kDown & KEY_A) {
            logger.Info("Launching Artic Base");
            bool done = extractPlugin() && launchPlugin();
            if (done) {
                logger.Raw(true, "");
                logger.Info("Done, select a game from the home menu");
                if (logger.GetOverrideState()) logger.Warning("Hold [L] to enable logs on the\n bottom screen !");
                svcSleepThread(3000000000);
                break;
            } else {
                logger.Error("Failed to launch Artic Base");
            }
        }

		// Flush and swap framebuffers
		gfxFlushBuffers();
		gfxSwapBuffers();

		//Wait for VBlank
		gspWaitForVBlank();
	}

    // Flush and swap framebuffers
    gfxFlushBuffers();
    gfxSwapBuffers();

    //Wait for VBlank
    gspWaitForVBlank();

	gfxExit();
    gspLcdExit();
    logger.End();
}

extern "C" {
	int main(int argc, char* argv[]);
}
// Entrypoint, game will starts when you exit this function
int    main(int argc, char* argv[])
{
	Main();
	return 0;
}
