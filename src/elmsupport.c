#include "include/opl.h"
#include "include/lang.h"
#include "include/gui.h"
#include "include/elmsupport.h"
#include "include/themes.h"
#include "include/system.h"
#include "include/ioman.h"

#include "include/bdmsupport.h"
#include "include/ethsupport.h"
#include "include/hddsupport.h"
#include "include/supportbase.h"

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h> // fileXioMount, fileXioUmount
#include <io_common.h>   // FIO_MT_RDWR

static int elmForceUpdate = 1;
static int elmItemCount = 0;

// forward declaration
static item_list_t elmItemList;

//Struct that holds information about a game
typedef struct ElmGame ElmGame;
struct ElmGame
{
    char title[ISO_GAME_NAME_MAX - GAME_STARTUP_MAX + 1];
    char ID[GAME_STARTUP_MAX + 1];
    char file[ISO_GAME_NAME_MAX + 1 + 50];

    //Eg: smb0:POPS .. smb0:POPS9 - It's where the VCD is
    //Eg: pfs0:POPS/  is where the ELF is and the fake launch path
    char pathFolder[50];
    int sizeMB;
    ElmGame *next;
};

//The pointer to the first game in the list
static ElmGame *elmGameList = NULL;

//The full path to the POPSTARTER.ELF file. Eg: smb0:POPS/POPSTARTER.ELF
char elmPathElfHdd[256];
char elmPathElfBdm[256];
char elmPathElfEth[256];

//Last modified for the folder
static unsigned char elmModifiedPrev[8];

//Frees the entire game list
static void elmGameListFree()
{
    ElmGame *temp = NULL;
    while ((temp = elmGameList) != NULL) {
        elmGameList = elmGameList->next;
        free(temp);
    }
    LOG("elmGameListFree() Complete\n");
}

static void elmGameTitleAppendDevice(ElmGame *tempGame)
{
    if (!strncmp(tempGame->file, "hdd", 3)) {
        strcat(tempGame->title, " [HDD]");
    } else if (!strncmp(tempGame->file, "mass", 4)) {
        strcat(tempGame->title, " [USB]");
    } else {
        strcat(tempGame->title, " [ETH]");
    }
}

static int elmGameDuplicateTitleExists(ElmGame *inGame)
{
    ElmGame *tempGame = inGame->next;
    int count = 0;
    while (tempGame != NULL) {
        if (strcmp(tempGame->title, inGame->title) == 0) {
            elmGameTitleAppendDevice(tempGame);
            count++;
        }
        tempGame = tempGame->next;
    }
    if (count > 0) {
        elmGameTitleAppendDevice(inGame);
    }
    return count;
}

static struct ElmGame *elmGetGameInfo(int id)
{
    struct ElmGame *cur = elmGameList;

    while (id--) {
        cur = cur->next;
    }

    return cur;
}

void elmInit(void)
{
    LOG("ELMSUPPORT Init\n");
    memset(elmModifiedPrev, 0, 8);
    elmForceUpdate = 1;
    configGetInt(configGetByType(CONFIG_OPL), "elm_frames_delay", &elmItemList.delay);
    elmItemList.enabled = 1;
}

item_list_t *elmGetObject(int initOnly)
{
    if (initOnly && !elmItemList.enabled)
        return NULL;
    return &elmItemList;
}

static int elmNeedsUpdate(void)
{
    int update;

    update = 0;
    if (elmForceUpdate) {
        elmForceUpdate = 0;
        update = 1;
    }

    if (oplShouldElmUpdate()) {
        update = 1;
    }

    return update;
}

//Scans for POPSTARTER.ELF and VCD files in the given devPrefix.
static int elmScanVCDs(char *devPrefix)
{
    int count = 0; //Game count
    int i;
    char fullpath[256];
    struct dirent *pdirent;
    struct stat st;
    DIR *pdir;

    LOG("elmScanVCDs()\n");

    char currentPath[30];
    //POPS POPS0 POPS1 ... POPS9
    for (i = -1; i < 10; i++) {
        if (i == -1) {
            snprintf(currentPath, sizeof(currentPath), "%sPOPS/", devPrefix);
        } else {
            snprintf(currentPath, sizeof(currentPath), "%sPOPS%d/", devPrefix, i);
        }
        LOG("currentPath = %s\n", currentPath);

        //Let's open the folder and search for the VCD files.
        if ((pdir = opendir(currentPath)) != NULL) {
            while ((pdirent = readdir(pdir)) != NULL) {
                int filename_len = strlen(pdirent->d_name);
                if (strlen(pdirent->d_name) > ISO_GAME_NAME_MAX)
                    continue; //Skip files that cannot be supported properly.

                if (filename_len >= 4 && strcasecmp(pdirent->d_name + filename_len - 4, ".VCD") == 0) {
                    LOG("VCD Found: %s\n", pdirent->d_name);

                    if ((filename_len >= (GAME_STARTUP_MAX - 1) + 1 + 1 + 4) && (pdirent->d_name[4] == '_') && (pdirent->d_name[8] == '.') && (pdirent->d_name[11] == '.')) { //Game ID found
                        //Create a new game
                        ElmGame *newElmGame = (ElmGame *)malloc(sizeof(ElmGame));
                        if (newElmGame != NULL) {
                            //Eg: SLES_123.04.Game Title.VCD
                            int NameLen = filename_len - GAME_STARTUP_MAX - 4;

                            //Eg: Game Title
                            strncpy(newElmGame->title, &pdirent->d_name[GAME_STARTUP_MAX], NameLen);
                            newElmGame->title[NameLen] = '\0';

                            //Eg: SLES_123.04
                            strncpy(newElmGame->ID, pdirent->d_name, GAME_STARTUP_MAX - 1);
                            newElmGame->ID[GAME_STARTUP_MAX - 1] = '\0';

                            // Get the file info
                            sprintf(fullpath, "%s/%s", currentPath, pdirent->d_name);
                            stat(fullpath, &st);

                            //Eg: smb0:POPS/SLES_123.04.Game Title.VCD
                            snprintf(newElmGame->file, ISO_GAME_NAME_MAX + 1, "%s%s", currentPath, pdirent->d_name);
                            newElmGame->sizeMB = st.st_size >> 20;

                            //Eg: smb0:POPS | smb0:POPS9
                            strncpy(newElmGame->pathFolder, currentPath, sizeof(newElmGame->pathFolder));

                            newElmGame->next = elmGameList;
                            elmGameList = newElmGame;

                            count++;
                            LOG("newElmGame->file = %s\n", newElmGame->file);
                            LOG("newElmGame->pathFolder = %s\n", newElmGame->pathFolder);
                            LOG("newElmGame->title = %s\n", newElmGame->title);
                            LOG("newElmGame->ID = %s\n", newElmGame->ID);
                            LOG("newElmGame->sizeMB = %d\n", newElmGame->sizeMB);
                        } else {
                            break; //Out of memory.
                        }
                    } else {
                        LOG("No ID found in file name!!\n");
                    }
                } else {
                    LOG("Not a .VCD file: %s\n", pdirent->d_name);
                }
            }
            closedir(pdir);
        }
    }
    return count;
}

//Scans partitions named __.POPS* and lists the VCD's inside it
static int elmScanVCDsHDD()
{
    int fd, fd2, MountFD = 0, count = 0;
    const char *mountPoint = "pfs1:";
    char partition[261];
    iox_dirent_t record;

    char scanned[10][9];
    int i;
    int skip = 0;
    int partitionCount = 0;
    if ((fd2 = fileXioDopen("hdd0:")) >= 0) {
        while (fileXioDread(fd2, &record) > 0) {
            skip = 0;
            fileXioUmount(mountPoint);
            if (strncmp(record.name, "__.POPS", 7) == 0) {
                for (i = 0; i < partitionCount; i++) {
                    if (strcmp(scanned[i], record.name) == 0) {
                        LOG("%s was already scanned! Skipping!", record.name);
                        skip = 1;
                        break;
                    }
                }
                if (skip == 1)
                    continue;

                strcpy(scanned[partitionCount], record.name);
                partitionCount++;

                sprintf(partition, "hdd0:%s", record.name);
                LOG("Mounting '%s' into '%s' \n", partition, mountPoint);
                if ((MountFD = fileXioMount(mountPoint, partition, O_RDONLY)) >= 0) {
                    LOG("MOUNTED %s @ %s\n", partition, mountPoint);
                    char mountPointSlash[10];
                    sprintf(mountPointSlash, "%s/", mountPoint);
                    LOG("fileXioDopen(%s)\n", mountPointSlash);
                    if ((fd = fileXioDopen(mountPointSlash)) > 0) {
                        while (fileXioDread(fd, &record) > 0) {
                            int filename_len = strlen(record.name);
                            if (strlen(record.name) > ISO_GAME_NAME_MAX)
                                continue; //Skip files that cannot be supported properly.

                            if (filename_len >= 4 && strcasecmp(record.name + filename_len - 4, ".VCD") == 0) {
                                LOG("VCD Found: %s\n", record.name);
                                if ((filename_len >= (GAME_STARTUP_MAX - 1) + 1 + 1 + 4) && (record.name[4] == '_') && (record.name[8] == '.') && (record.name[11] == '.')) { //Game ID found
                                    //Create a new game
                                    ElmGame *newElmGame = (ElmGame *)malloc(sizeof(ElmGame));
                                    if (newElmGame != NULL) {
                                        newElmGame->next = elmGameList;
                                        elmGameList = newElmGame;

                                        //Eg: SLES_123.04.Game Title.VCD
                                        int NameLen = filename_len - GAME_STARTUP_MAX - 4;

                                        //Eg: Game Title
                                        strncpy(newElmGame->title, &record.name[GAME_STARTUP_MAX], NameLen);
                                        newElmGame->title[NameLen] = '\0';

                                        //Eg: SLES_123.04
                                        strncpy(newElmGame->ID, record.name, GAME_STARTUP_MAX - 1);
                                        newElmGame->ID[GAME_STARTUP_MAX - 1] = '\0';

                                        //Eg: hdd0:__POPS3/SLES_123.04.Game Title.VCD
                                        snprintf(newElmGame->file, ISO_GAME_NAME_MAX + 1, "%s/%s", partition, record.name); //!!!!PARTITION
                                        newElmGame->sizeMB = (record.stat.size >> 20) | (record.stat.hisize << 12);

                                        //Always pfs1:
                                        strcpy(newElmGame->pathFolder, "pfs1:");

                                        count++;
                                        LOG("newElmGame->file = %s\n", newElmGame->file);
                                        LOG("newElmGame->pathFolder = %s\n", newElmGame->pathFolder);
                                        LOG("newElmGame->title = %s\n", newElmGame->title);
                                        LOG("newElmGame->ID = %s\n", newElmGame->ID);
                                        LOG("newElmGame->sizeMB = %d\n", newElmGame->sizeMB);
                                    } else {
                                        break; //Out of memory.
                                    }
                                } else {
                                    LOG("No ID found in file name!!\n");
                                }
                            }
                        }
                        fileXioDclose(fd);
                    }
                    fileXioUmount(mountPoint);
                } else {
                    LOG("MountFD=%d\n", MountFD);
                }
            }
        }
        fileXioDclose(fd2);
    } else {
        LOG("fd2 = %d\n", fd2);
    }


    return count;
}

static int elmUpdateItemList(void)
{
    elmItemCount = 0;

    //Clear game list first.
    if (elmGameList != NULL)
        elmGameListFree();

    //Try HDD
    if (hddGetObject(1)) {
        //Eg: pfs0:POPS/POPSTARTER.ELF
        snprintf(elmPathElfHdd, sizeof(elmPathElfHdd), "%sPOPS/POPSTARTER.ELF", hddGetPrefix());
        LOG("elmPathElfHdd = %s\n", elmPathElfHdd);

        //Check if POPSTARTER.ELF exists in the folder.
        int fdElf = open(elmPathElfHdd, O_RDONLY, 0666);
        if (fdElf >= 0) {
            close(fdElf);
            elmItemCount += elmScanVCDsHDD();
        } else {
            LOG("POPSTARTER.ELF not found at %s", elmPathElfHdd);
        }
    }

    //Try ETH
    if (ethGetObject(1)) {
        //Eg: smb0:POPS/POPSTARTER.ELF
        snprintf(elmPathElfEth, sizeof(elmPathElfEth), "%sPOPS/POPSTARTER.ELF", ethGetPrefix());
        LOG("elmPathElfEth = %s\n", elmPathElfEth);

        //Check if POPSTARTER.ELF exists in the folder.
        int fdElf = open(elmPathElfEth, O_RDONLY, 0666);
        if (fdElf >= 0) {
            close(fdElf);
            elmItemCount += elmScanVCDs(ethGetPrefix());
        } else {
            LOG("POPSTARTER.ELF not found at %s", elmPathElfEth);
        }
    }

    //Try USB
    if (bdmGetObject(1)) {
        //Eg: mass0:POPS/POPSTARTER.ELF
        snprintf(elmPathElfBdm, sizeof(elmPathElfBdm), "%sPOPS/POPSTARTER.ELF", bdmGetBase());
        LOG("elmPathElfUsb = %s\n", elmPathElfBdm);

        //Check if POPSTARTER.ELF exists in the folder.
        int fdElf = open(elmPathElfBdm, O_RDONLY, 0666);
        if (fdElf >= 0) {
            close(fdElf);
            elmItemCount += elmScanVCDs(bdmGetBase());
        } else {
            LOG("POPSTARTER.ELF not found at %s", elmPathElfBdm);
        }
    }

    //Check for duplicates
    if (elmGameList) {
        ElmGame *cur = elmGameList;
        while (cur) {
            elmGameDuplicateTitleExists(cur);
            cur = cur->next;
        }
    }
    return elmItemCount;
}

static int elmGetItemCount(void)
{
    return elmItemCount;
}

static char *elmGetItemName(int id)
{
    ElmGame *cur = elmGetGameInfo(id);
    return cur->title;
}

static int elmGetItemNameLength(int id)
{
    return ISO_GAME_NAME_MAX + 1 - GAME_STARTUP_MAX;
}

static char *elmGetItemStartup(int id)
{
    ElmGame *cur = elmGetGameInfo(id);
    return cur->ID;
}

static void elmDeleteItem(int id)
{
    ElmGame *cur = elmGetGameInfo(id);
    int ret = -1;

    //Check if it's a HDD game.
    if (!strncmp(cur->file, "hdd0:", 5)) {
        int MountFD;
        char partition[10];
        char file[256];

        //Let's get the partition Eg: hdd0:__.POPS
        char *separator = strchr(cur->file, '/');
        strncpy(partition, cur->file, separator - (cur->file));
        partition[separator - (cur->file)] = '\0';

        //And the file path Eg: pfs1:SLES_123.45.Game.VCD
        sprintf(file, "pfs1:%s", separator + 1);

        //Make sure it's unmounted
        fileXioUmount("pfs1:");

        LOG("Mounting '%s' into 'pfs1:'\n", partition);
        if ((MountFD = fileXioMount("pfs1:", partition, FIO_MT_RDWR)) >= 0) {
            ret = fileXioRemove(file);
            LOG("fileXioRemove(%s) = %d\n", file, ret);
            fileXioUmount("pfs1:");
        } else {
            LOG("Failed to mount. Error %d\n", MountFD);
        }
    } else {
        LOG("fileXioRemove()\n", cur->file);
        ret = fileXioRemove(cur->file);
    }
    if (ret != 0) {
        guiMsgBox(_l(_STR_ELM_DELETE_ERROR), 0, NULL);
    } else {
        elmForceUpdate = 1;
    }
}

static void elmRenameItem(int id, char *newName)
{
    ElmGame *cur = elmGetGameInfo(id);
    char newNameFull[256];
    int ret = -1;
    int fd;
    snprintf(newNameFull, sizeof(newNameFull), "%s%s.%s.VCD", cur->pathFolder, cur->ID, newName);
    LOG("elmRenameItem()\n");
    LOG("cur->file = %s\n", cur->file);
    LOG("newNameFull = %s\n", newNameFull);
    //Check if it's a HDD game.
    if (!strncmp(cur->file, "hdd0:", 5)) {
        char partition[10];
        char file[256];

        //Let's get the partition Eg: hdd0:__.POPS
        char *separator = strchr(cur->file, '/');
        strncpy(partition, cur->file, separator - (cur->file));
        partition[separator - (cur->file)] = '\0';

        //And the file path Eg: pfs1:SLES_123.45.Game.VCD
        sprintf(file, "pfs1:%s", separator + 1);

        //Make sure it's unmounted
        fileXioUmount("pfs1:");

        LOG("Mounting '%s' into 'pfs1:'\n", partition);
        if ((fd = fileXioMount("pfs1:", partition, FIO_MT_RDWR)) >= 0) {
            ret = fileXioRename(file, newNameFull);
            LOG("fileXioRename(%s , %s) = %d\n", file, newNameFull, ret);
            fileXioUmount("pfs1:");
        } else {
            LOG("Failed to mount. Error %d\n", fd);
        }
    } else if (!strncmp(cur->file, "mass", 4)) {
        LOG("RENAMING MASS\n");

        if ((fd = open(cur->file, O_RDONLY)) >= 0) {
            ret = fileXioIoctl(fd, USBMASS_IOCTL_RENAME, newNameFull);
            close(fd);
        }
        LOG("fd = %d\n", fd);
        LOG("ret = %d\n", ret);
    } else {
        ret = fileXioRename(cur->file, newNameFull);
        LOG("fileXioRename(%s , %s) = %d\n", cur->file, newNameFull, ret);
    }

    if (ret != 0) {
        guiMsgBox(_l(_STR_ELM_RENAME_ERROR), 0, NULL);
    } else {
        elmForceUpdate = 1;
    }
}

static void elmLaunchItem(int id, config_set_t *configSet)
{
    ElmGame *cur = elmGetGameInfo(id);
    //The path to POPSTARTER.ELF
    char elmPathElf[256];

    //The prefix of the ELF file. Eg: XX./SB./<none>
    char elmElfPrefix[4];

    //Figure out the path to POPSTARTER and the prefix
    if (!strncmp(cur->file, "hdd0", 4)) {
        strcpy(elmPathElf, elmPathElfHdd);
        strcpy(elmElfPrefix, "");
    } else if (!strncmp(cur->file, "mass", 4)) {
        strcpy(elmPathElf, elmPathElfBdm);
        strcpy(elmElfPrefix, "XX.");
    } else {
        strcpy(elmPathElf, elmPathElfEth);
        strcpy(elmElfPrefix, "SB.");
    }

    LOG("elmLaunchItem with %s", elmPathElf);

    int fdElf = open(elmPathElf, O_RDONLY, 0666);
    if (fdElf >= 0) {
        int fdVcd = 0;
        //If we start with hdd0 don't check if the file exists
        if (!strncmp(cur->file, "hdd0", 4)) {
            fdVcd = 1;
        } else {
            fdVcd = open(cur->file, O_RDONLY, 0666);
        }

        if (fdVcd >= 0) {
            void *buffer = NULL;
            int realSize = fileXioLseek(fdElf, 0, SEEK_END);
            fileXioLseek(fdElf, 0, SEEK_SET);

            buffer = malloc(realSize);
            if (!buffer) {
                LOG("Failed allocation of %d bytes", realSize);
            } else {
                fileXioRead(fdElf, buffer, realSize);
                LOG("Loaded POPSTARTER ELF with size = %d\n", realSize);
            }

            close(fdElf);

            char memPath[256];
            sprintf(memPath, "mem:%u", (unsigned int)buffer);

            char *fileOnly = strrchr(cur->file, '/');
            if (!fileOnly)
                fileOnly = strrchr(cur->file, ':');

            fileOnly++;

            fileOnly[strlen(fileOnly) - 4] = '\0';

            LOG("fileOnly= %s\n", fileOnly);
            char params[256];
            sprintf(params, "%s%s%s.ELF", cur->pathFolder, elmElfPrefix, fileOnly);

            LOG("memPath = %s\n", memPath);
            LOG("params = %s\n", params);
            LOG("VCD Path= %s", cur->file);

            int mode = ELM_MODE;

            // Figure out in what device the VCD is at. This is necessary to avoid the device to be unmounted.
            if (strncmp(cur->file, "mass", 4) == 0) {
                mode = BDM_MODE;
            } else if (strncmp(cur->file, "hdd", 3) == 0) {
                mode = HDD_MODE;
            } else if (strncmp(cur->file, "smb", 3) == 0) {
                mode = ETH_MODE;
            }

            if (mode == ELM_MODE) {
                // Failed to detect the device...
                LOG("ELMSUPPORT warning: cannot find mode for path: %s\n", cur->file);
            } else {
                LOG("ELMSUPPORT Mode detected as: ", mode);
            }

            deinit(UNMOUNT_EXCEPTION, mode); // CAREFUL: deinit will call elmCleanUp, so configElm/cur will be freed
            sysExecElfWithParam(memPath, params);
        } else {
            char error[256];
            snprintf(error, sizeof(error), _l(_STR_ELM_LAUNCH_VCD_NOTFOUND), cur->file);
            guiMsgBox(error, 0, NULL);
        }
    } else {
        char error[256];
        snprintf(error, sizeof(error), _l(_STR_ELM_LAUNCH_POPSTARTER_NOTFOUND), elmPathElf);
        guiMsgBox(error, 0, NULL);
    }
}

static config_set_t *elmGetConfig(int id)
{
    config_set_t *config = NULL;
    static item_list_t *listSupport = NULL;
    ElmGame *cur = elmGetGameInfo(id);
    int ret = 0;

    //Search on HDD, SMB, USB for the CFG/GAME.ELF.cfg file.
    //HDD
    if ((listSupport = hddGetObject(1))) {
        char path[256];
#if OPL_IS_DEV_BUILD
            snprintf(path, sizeof(path), "%sCFG-DEV/%s.cfg", hddGetPrefix(), cur->ID));
#else
        snprintf(path, sizeof(path), "%sCFG/%s.cfg", hddGetPrefix(), cur->ID);
#endif
            config = configAlloc(1, NULL, path);
            ret = configRead(config);
    }

    //ETH
    if (ret == 0 && (listSupport = ethGetObject(1))) {
        char path[256];
        if (config != NULL)
            configFree(config);

#if OPL_IS_DEV_BUILD
        snprintf(path, sizeof(path), "%sCFG-DEV/%s.cfg", ethGetPrefix(), cur->ID);
#else
        snprintf(path, sizeof(path), "%sCFG/%s.cfg", ethGetPrefix(), cur->ID);
#endif
        config = configAlloc(1, NULL, path);
        ret = configRead(config);
    }

    //USB
    if (ret == 0 && (listSupport = bdmGetObject(1))) {
        char path[256];
        if (config != NULL)
            configFree(config);

#if OPL_IS_DEV_BUILD
        snprintf(path, sizeof(path), "%sCFG-DEV/%s.cfg", bdmGetPrefix(), cur->ID);
#else
        snprintf(path, sizeof(path), "%sCFG/%s.cfg", bdmGetPrefix(), cur->ID);
#endif
        config = configAlloc(1, NULL, path);
        ret = configRead(config);
    }

    if (ret == 0) { //No config found on previous devices, create one.
        if (config != NULL)
            configFree(config);

        config = configAlloc(1, NULL, NULL);
    }

    configSetStr(config, CONFIG_ITEM_NAME, cur->title);
    configSetStr(config, CONFIG_ITEM_LONGNAME, cur->title);
    configSetStr(config, CONFIG_ITEM_STARTUP, cur->ID);
    configSetInt(config, CONFIG_ITEM_SIZE, cur->sizeMB);
    configSetStr(config, CONFIG_ITEM_FORMAT, "VCD");
    configSetStr(config, CONFIG_ITEM_MEDIA, "PS1");
    return config;
}

static int elmGetImage(char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm)
{
    // Search every device from fastest to slowest (HDD > ETH > USB)
    item_list_t *listSupport = NULL;
    if ((listSupport = hddGetObject(1))) {
        if (listSupport->itemGetImage(folder, isRelative, value, suffix, resultTex, psm) >= 0)
            return 0;
    }

    if ((listSupport = ethGetObject(1))) {
        if (listSupport->itemGetImage(folder, isRelative, value, suffix, resultTex, psm) >= 0)
            return 0;
    }

    if ((listSupport = bdmGetObject(1)))
        return listSupport->itemGetImage(folder, isRelative, value, suffix, resultTex, psm);

    return -1;
}

static int elmGetTextId(void)
{
    return _STR_ELM;
}

static int elmGetIconId(void)
{
    return ELM_ICON;
}

static void elmCleanUp(int exception)
{
    if (elmItemList.enabled) {
        LOG("ELMSUPPORT CleanUp\n");
        elmGameListFree();
    }
}

//This may be called, even if appInit() was not.
static void elmShutdown(void)
{
    if (elmItemList.enabled) {
        LOG("ELMSUPPORT Shutdown\n");

        elmGameListFree();
    }
}

static item_list_t elmItemList = {
    ELM_MODE, -1, 0, MODE_FLAG_NO_COMPAT | MODE_FLAG_NO_UPDATE, MENU_MIN_INACTIVE_FRAMES, ELM_MODE_UPDATE_DELAY, "PS1 Games", &elmGetTextId, NULL, NULL, NULL, &elmInit, &elmNeedsUpdate, &elmUpdateItemList,
    &elmGetItemCount, NULL, &elmGetItemName, &elmGetItemNameLength, &elmGetItemStartup, &elmDeleteItem, &elmRenameItem, &elmLaunchItem,
    &elmGetConfig, &elmGetImage, &elmCleanUp, &elmShutdown, NULL, &elmGetIconId};
