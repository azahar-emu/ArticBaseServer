#include "Main.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "ArticProtocolServer.hpp"
#include "ArticFunctions.hpp"
#include "CTRPluginFramework/Clock.hpp"
#include "plgldr.h"

#include "BCLIM.hpp"

#include "logo.h"

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x400000

Logger logger;
static bool should_run = true;
static int listen_fd = -1;
static int accept_fd = -1;
static ArticProtocolServer* articBase = nullptr;
static bool wasControllerMode = false;
static bool everControllerMode = false;
static bool reloadBottomText = false;

int transferedBytes = 0;
bool isControllerMode = false;

extern "C" {
    #include "csvc.h"
}

void    DisableSleep(void)
{
	u8  reg;

	if (R_FAILED(mcuHwcInit()))
		return;

	if (R_FAILED(MCUHWC_ReadRegister(0x18, &reg, 1)))
		return;

	reg |= 0x60; ///< Disable home btn & shell state events
	MCUHWC_WriteRegister(0x18, &reg, 1);
	mcuHwcExit();
}

void    EnableSleep(void)
{
	u8  reg;

	if (R_FAILED(mcuHwcInit()))
		return;

	if (R_FAILED(MCUHWC_ReadRegister(0x18, &reg, 1)))
		return;

	reg &= ~0x60; ///< Enable home btn & shell state events
	MCUHWC_WriteRegister(0x18, &reg, 1);
	mcuHwcExit();
}

void Start(void* arg) {
    int res;
    void* SOC_buffer = memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if ((res = socInit((u32*)SOC_buffer, SOC_BUFFERSIZE)) != 0) {
        free(SOC_buffer);
        logger.Error("Server: Cannot initialize sockets");
        return;
    }
    while (should_run) {
        svcSleepThread(500000000);
        struct sockaddr_in servaddr = {0};
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) {
            logger.Error("Server: Cannot create socket");
            continue;
        }

        if (!ArticProtocolServer::SetNonBlock(listen_fd, true)) {
            logger.Error("Server:: Failed to set non-block");
            close(listen_fd);
            listen_fd = -1;
            continue;
        }

        servaddr.sin_family      = AF_INET;
        servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
        servaddr.sin_port        = htons(SERVER_PORT);
        res = bind(listen_fd, (struct sockaddr *) &servaddr, sizeof(servaddr));
        if (res < 0) {
            logger.Error("Server: Failed to bind() to port %d", SERVER_PORT);
            close(listen_fd);
            listen_fd = -1;
            continue;
        }

        res = listen(listen_fd, 1);
        if (res < 0) {
            if (should_run) {
                logger.Error("Server: Failed to listen()");
            }
            close(listen_fd);
            listen_fd = -1;
            continue;
        }
        
        struct in_addr host_id;
        host_id.s_addr = gethostid();
        logger.Info("Server: Listening on: %s:%d", inet_ntoa(host_id), SERVER_PORT);

        struct sockaddr_in peeraddr = {0};
        socklen_t peeraddr_len = sizeof(peeraddr);
        bool error = false;
        while (true) {
            accept_fd = accept(listen_fd, (struct sockaddr *) &peeraddr, &peeraddr_len);
            if (accept_fd < 0 || peeraddr_len == 0) {
                if (errno == EWOULDBLOCK && should_run) {
                    svcSleepThread(10000000);
                    continue;
                }
                if (should_run) {
                    logger.Error("Server: Failed to accept()");
                }
                error = true;
                break;
            }
            break;
        }
        if (listen_fd >= 0) {
            close(listen_fd);
            listen_fd = -1;
        }
        if (error)
            continue;

        logger.Info("Server: Connected: %s:%d", inet_ntoa(peeraddr.sin_addr), ntohs(peeraddr.sin_port));

        if (!ArticProtocolServer::SetNonBlock(accept_fd, true)) {
            logger.Error("Server: Failed to set non-block");
            shutdown(accept_fd, SHUT_RDWR);
            close(accept_fd);
            accept_fd = -1;
            continue;
        }
        
        articBase = new ArticProtocolServer(accept_fd);
        articBase->Serve();
        accept_fd = -1;
        delete articBase;
        articBase = nullptr;
        logger.Info("Server: Disconnected");

        for (auto it = ArticFunctions::destructFunctions.begin(); it != ArticFunctions::destructFunctions.end(); it++) {
            (*it)();
        }
        isControllerMode = false;
        everControllerMode = false;
    }
    socExit();
    free(SOC_buffer);
}

PrintConsole topScreenConsole, bottomScreenConsole;

void Main() {
    logger.Start();


    plgLdrInit();
    bool prevPluginState = ((PluginHeader*)0x07000000)->config[0] != 0;
    PLGLDR__SetPluginLoaderState(prevPluginState);
    PLGLDR__ClearPluginLoadParameters();
    plgLdrExit();

    gspLcdInit();

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

    auto print_bottom_info = [](bool controller_mode) {
        if (controller_mode) {
            logger.Raw(false,   "\n            ArticBase v%d.%d.%d\n\n"
                                "        Artic Controller enabled       \n"
                                "                                       \n"
                                "  - X + START + SELECT: Exit           \n"
                                "    Artic Controller mode              \n"
                                "                                        "
                                , VERSION_MAJOR, VERSION_MINOR, VERSION_REVISION);
        } else {
            if (everControllerMode) {
                logger.Raw(false,   "\n            ArticBase v%d.%d.%d\n\n"
                                "  - A:      Enter sleep                \n"
                                "  - X:      Show debug log             \n"
                                "  - Y:      Restart server             \n"
                                "  - START:  Exit                       \n"
                                "  - SELECT: Enter Artic controller mode "
                                , VERSION_MAJOR, VERSION_MINOR, VERSION_REVISION);
            } else {
                logger.Raw(false,   "\n            ArticBase v%d.%d.%d\n\n"
                                "  - A:      Enter sleep                \n"
                                "  - X:      Show debug log             \n"
                                "  - Y:      Restart server             \n"
                                "  - START:  Exit                       \n"
                                "                                        "
                                , VERSION_MAJOR, VERSION_MINOR, VERSION_REVISION);
            }
            
        }
    };

	hidScanInput();
    u32 kH = hidKeysHeld();
    if (kH & KEY_L) {
        logger.BrokenScreen();
        logger.Info("\n\n\n\n\n\n\n\n   Bottom screen mode.");
    }


    print_bottom_info(false);
    logger.Raw(true, "");

    bool setupCorrect = true;
    for (auto it = ArticFunctions::setupFunctions.begin(); it != ArticFunctions::setupFunctions.end(); it++) {
        setupCorrect = (*it)() && setupCorrect;
    }

    Thread serverThread = nullptr;
    if (!setupCorrect) {
        logger.Error("Server: Setup failed");
    } else {
        s32 prio = 0;
	    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        serverThread = threadCreate(Start, nullptr, 0x1000, prio-1, -2, false);
    }

    CTRPluginFramework::Clock clock;
    bool sleeping = false;
    
    DisableSleep();
    
    while (aptMainLoop())
	{
        if (isControllerMode != wasControllerMode || reloadBottomText) {
            reloadBottomText = false;
            everControllerMode |= isControllerMode;
            if (isControllerMode) {
                logger.Info("Server: Controller mode enabled");
            } else {
                logger.Info("Server: Controller mode disabled");
            }
            print_bottom_info(isControllerMode);
            wasControllerMode = isControllerMode;
        }
        
		hidScanInput();

		u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();

        if (((kDown & ~KEY_SELECT) != 0) && sleeping && !isControllerMode) {
            sleeping = false;
            GSPLCD_PowerOnBacklight(3);
            continue;
        }

        if ((kDown & KEY_A) && !sleeping && !isControllerMode) {
            sleeping = true;
            GSPLCD_PowerOffBacklight(3);
        }

		if ((kDown & KEY_START) && !isControllerMode) {
            logger.Info("Server: Exiting");
            break;
        }

        if ((kDown & KEY_SELECT) && !isControllerMode && everControllerMode) {
            isControllerMode = true;
        }

        if ((kDown & KEY_Y) && !isControllerMode) {
            if (articBase) {
                logger.Info("Server: Restarting");
                articBase->QueryStop();
            } else {
                logger.Debug("Server: Not started yet.");
            }
        }

        if ((kDown & KEY_X) && !isControllerMode) {
            if (logger.debug_enable) {
                logger.Info("Server: Debug log disabled");
                logger.debug_enable = false;
            } else {
                logger.Info("Server: Debug log enabled");
                logger.debug_enable = true;
            }
        }

        if (((kHeld & (KEY_X | KEY_START | KEY_SELECT)) == (KEY_X | KEY_START | KEY_SELECT)) && isControllerMode) {
            isControllerMode = false;
        }

        if (clock.HasTimePassed(CTRPluginFramework::Seconds(1)))
        {
            if (articBase) {
                CTRPluginFramework::Time t = clock.GetElapsedTime();
                float bytes = transferedBytes / t.AsSeconds();
                transferedBytes = 0;
                float value = (bytes >= 1000 * 1000) ? (bytes / 1000.f * 1000.f) : (bytes / 1000.f);
                const char* unit = (bytes >= 1000 * 1000) ? "MB/s" : "KB/s";
                logger.Traffic("          Traffic: %.02f %s     \n", value, unit);
                clock.Restart();
            } else {
                logger.Traffic("                                  \n");
            }
        }

		// Flush and swap framebuffers
		gfxFlushBuffers();
		gfxSwapBuffers();

		//Wait for VBlank
		gspWaitForVBlank();
	}

    should_run = false;
    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
    if (articBase) {
        articBase->QueryStop();
    }

    if (serverThread) {
        threadJoin(serverThread, U64_MAX);
        threadFree(serverThread);
    }

    EnableSleep();

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
