#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <wups.h>
#include <wups/config_api.h>

#include <coreinit/debug.h>
#include <coreinit/launch.h>
#include <coreinit/thread.h>

WUPS_PLUGIN_NAME("Wii U Server Selector");
WUPS_PLUGIN_DESCRIPTION("Automatically select and switch server environments");
WUPS_PLUGIN_VERSION("v1.0.2.2");
WUPS_PLUGIN_AUTHOR("hadley557");
WUPS_PLUGIN_LICENSE("GPLv2");

WUPS_USE_WUT_DEVOPTAB();

#define SERVERS_DIR "/vol/external01/wiiu/servers"
#define MODULES_DIR "/vol/external01/wiiu/environments/aroma/modules"
#define PLUGINS_DIR "/vol/external01/wiiu/environments/aroma/plugins"

struct ServerInfo {
    std::string name;
    std::string path;
};

struct ManagedFile {
    std::string name;
    std::string targetPath;
};

struct ServerActionItem {
    std::string displayName;
    std::string serverPath;
    bool isCurrent = false;
    bool selectable = false;
};

static constexpr size_t COPY_BUFFER_SIZE  = 32 * 1024;
static constexpr size_t WORKER_STACK_SIZE = 16 * 1024;

static OSThread workerThread;
alignas(8) static uint8_t workerStack[WORKER_STACK_SIZE];
static std::string pendingServerPath;
static std::atomic<bool> installInProgress{false};

static std::string JoinPath(const std::string& dir, const char* name) {
    return dir + "/" + name;
}

// Returns the destination path for a managed file, or false if the file is not a server file.
static bool GetManagedTargetPath(const char* filename, std::string& outPath) {
    const size_t length = std::strlen(filename);
    if (length <= 4) return false;

    const char* extension = filename + length - 4;
    if (std::strcmp(extension, ".wms") == 0) {
        outPath = JoinPath(MODULES_DIR, filename);
    } else if (std::strcmp(extension, ".wps") == 0) {
        outPath = JoinPath(PLUGINS_DIR, filename);
    } else {
        return false;
    }
    return true;
}

static std::vector<ManagedFile> ListManagedFiles(const std::string& dirPath) {
    std::vector<ManagedFile> files;

    DIR* dir = opendir(dirPath.c_str());
    if (!dir) return files;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        ManagedFile file;
        if (!GetManagedTargetPath(entry->d_name, file.targetPath)) continue;
        file.name = entry->d_name;
        files.push_back(std::move(file));
    }

    closedir(dir);
    return files;
}

static std::vector<ServerInfo> ScanServers() {
    std::vector<ServerInfo> servers;

    DIR* dir = opendir(SERVERS_DIR);
    if (!dir) return servers;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        const std::string path = JoinPath(SERVERS_DIR, entry->d_name);
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        servers.push_back({name, path});
    }

    closedir(dir);

    std::sort(servers.begin(), servers.end(), [](const ServerInfo& a, const ServerInfo& b) {
        return a.name < b.name;
    });
    return servers;
}

static bool IsServerActive(const std::string& serverPath) {
    const auto files = ListManagedFiles(serverPath);
    if (files.empty()) return false;

    // The installed file must exist and match the size of the server file.
    for (const auto& file : files) {
        struct stat sourceStat;
        struct stat targetStat;

        if (stat(JoinPath(serverPath, file.name.c_str()).c_str(), &sourceStat) != 0 ||
            stat(file.targetPath.c_str(), &targetStat) != 0 ||
            sourceStat.st_size != targetStat.st_size) {
            return false;
        }
    }
    return true;
}

static bool CopyFile(const std::string& source, const std::string& destination) {
    const int sourceFd = open(source.c_str(), O_RDONLY);
    if (sourceFd < 0) return false;

    const int destinationFd = open(destination.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (destinationFd < 0) {
        close(sourceFd);
        return false;
    }

    bool success = true;
    std::vector<char> buffer(COPY_BUFFER_SIZE);

    for (;;) {
        const ssize_t bytesRead = read(sourceFd, buffer.data(), buffer.size());
        if (bytesRead < 0) {
            success = false;
            break;
        }
        if (bytesRead == 0) break;

        const char* data = buffer.data();
        ssize_t remaining = bytesRead;
        while (remaining > 0) {
            const ssize_t bytesWritten = write(destinationFd, data, remaining);
            if (bytesWritten < 0) {
                success = false;
                break;
            }
            data += bytesWritten;
            remaining -= bytesWritten;
        }
        if (!success) break;
    }

    if (success && fsync(destinationFd) != 0) {
        success = false;
    }

    close(sourceFd);
    close(destinationFd);

    if (!success) {
        unlink(destination.c_str());
    }
    return success;
}

static void UninstallAllServers() {
    // Remove files installed by any server, but only files that still exist in a server folder.
    for (const auto& server : ScanServers()) {
        for (const auto& file : ListManagedFiles(server.path)) {
            unlink(file.targetPath.c_str());
        }
    }
}

static bool InstallServer(const std::string& serverPath) {
    const auto files = ListManagedFiles(serverPath);
    if (files.empty()) return false;

    UninstallAllServers();

    for (const auto& file : files) {
        if (!CopyFile(JoinPath(serverPath, file.name.c_str()), file.targetPath)) {
            return false;
        }
    }
    return true;
}

static int WorkerThreadProc(int argc, const char** argv) {
    if (!InstallServer(pendingServerPath)) {
        OSReport("[WiiUServerSelector] Failed to install server files\n");
        installInProgress = false;
        return 0;
    }

    // Give the menu a moment to close before rebooting.
    OSSleepTicks(OSMillisecondsToTicks(1500));
    OSLaunchTitlev(OS_TITLE_ID_REBOOT, 0, nullptr);

    // Reboot did not happen, so allow selecting again.
    installInProgress = false;
    return 0;
}

static void WriteDisplayText(char* outBuf, int32_t outSize, const char* text) {
    if (outBuf == nullptr || outSize <= 0) return;
    snprintf(outBuf, outSize, "%s", text);
}

static int32_t ServerAction_getCurrentValueDisplay(void* context, char* outBuf, int32_t outSize) {
    const auto* item = static_cast<const ServerActionItem*>(context);
    if (!item->selectable) {
        WriteDisplayText(outBuf, outSize, "");
    } else {
        WriteDisplayText(outBuf, outSize, item->isCurrent ? "Current" : "Select");
    }
    return 0;
}

static int32_t ServerAction_getCurrentValueSelectedDisplay(void* context, char* outBuf, int32_t outSize) {
    const auto* item = static_cast<const ServerActionItem*>(context);
    WriteDisplayText(outBuf, outSize, (item->selectable && !item->isCurrent) ? "Press A" : "");
    return 0;
}

static void ServerAction_onInput(void* context, WUPSConfigSimplePadData input) {
    const auto* item = static_cast<const ServerActionItem*>(context);
    if (!item->selectable || item->isCurrent) return;
    if (!(input.buttons_d & WUPS_CONFIG_BUTTON_A)) return;
    if (installInProgress.exchange(true)) return;

    pendingServerPath = item->serverPath;
    if (!OSCreateThread(&workerThread, WorkerThreadProc, 0, nullptr,
                        workerStack + sizeof(workerStack), sizeof(workerStack), 16,
                        OS_THREAD_ATTRIB_DETACHED)) {
        OSReport("[WiiUServerSelector] Failed to create worker thread\n");
        installInProgress = false;
        return;
    }

    OSResumeThread(&workerThread);
}

static void ServerAction_onDelete(void* context) {
    delete static_cast<ServerActionItem*>(context);
}

static WUPSConfigAPIStatus ServerAction_Create(const char* displayName,
                                               const std::string& serverPath,
                                               bool isCurrent,
                                               bool selectable,
                                               WUPSConfigItemHandle* outHandle) {
    if (displayName == nullptr || outHandle == nullptr) {
        return WUPSCONFIG_API_RESULT_INVALID_ARGUMENT;
    }

    auto* item = new (std::nothrow) ServerActionItem();
    if (item == nullptr) return WUPSCONFIG_API_RESULT_OUT_OF_MEMORY;

    item->displayName = displayName;
    item->serverPath = serverPath;
    item->isCurrent = isCurrent;
    item->selectable = selectable;

    WUPSConfigAPIItemCallbacksV2 callbacks = {};
    callbacks.getCurrentValueDisplay = &ServerAction_getCurrentValueDisplay;
    callbacks.getCurrentValueSelectedDisplay = &ServerAction_getCurrentValueSelectedDisplay;
    callbacks.onInput = &ServerAction_onInput;
    callbacks.onDelete = &ServerAction_onDelete;

    WUPSConfigAPIItemOptionsV2 options = {};
    options.displayName = item->displayName.c_str();
    options.context = item;
    options.callbacks = callbacks;

    const WUPSConfigAPIStatus status = WUPSConfigAPI_Item_Create(options, outHandle);
    if (status != WUPSCONFIG_API_RESULT_SUCCESS) {
        delete item;
    }
    return status;
}

static void AddServerItem(WUPSConfigCategoryHandle parent,
                          const char* displayName,
                          const std::string& serverPath,
                          bool isCurrent,
                          bool selectable) {
    WUPSConfigItemHandle handle;
    if (ServerAction_Create(displayName, serverPath, isCurrent, selectable, &handle) !=
        WUPSCONFIG_API_RESULT_SUCCESS) {
        return;
    }

    if (WUPSConfigAPI_Category_AddItem(parent, handle) != WUPSCONFIG_API_RESULT_SUCCESS) {
        WUPSConfigAPI_Item_Destroy(handle);
    }
}

WUPSConfigAPICallbackStatus ConfigMenuOpenedCallback(WUPSConfigCategoryHandle rootHandle) {
    const auto servers = ScanServers();
    if (servers.empty()) {
        AddServerItem(rootHandle, "No servers found", "", false, false);
        return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
    }

    for (const auto& server : servers) {
        AddServerItem(rootHandle, server.name.c_str(), server.path, IsServerActive(server.path), true);
    }
    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

void ConfigMenuClosedCallback() {
}

INITIALIZE_PLUGIN() {
    const WUPSConfigAPIOptionsV1 options = {.name = "Wii U Server Selector"};
    if (WUPSConfigAPI_Init(options, ConfigMenuOpenedCallback, ConfigMenuClosedCallback) !=
        WUPSCONFIG_API_RESULT_SUCCESS) {
        OSReport("[WiiUServerSelector] Failed to initialize config menu\n");
    }
}