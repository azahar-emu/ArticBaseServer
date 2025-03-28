#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "ArticFunctions.hpp"
#include "ArticFunctionsPrivate.hpp"
#include "Main.hpp"
#include "amExtension.hpp"
#include "fsExtension.hpp"
#include "hidExtension.hpp"
#include "CTRPluginFramework/CTRPluginFramework.hpp"
#include "CTRPluginFramework/Clock.hpp"

extern bool isControllerMode;

namespace ArticFunctions {

    ExHeader_Info lastAppExheader;
    std::map<u64, HandleType> openHandles;
    CTRPluginFramework::Mutex amMutex;
    CTRPluginFramework::Mutex cfgMutex;

    void Process_GetTitleID(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        ArticProtocolCommon::Buffer* tid_buffer = mi.ReserveResultBuffer(0, sizeof(u64));
        if (!tid_buffer) {
            return;
        }
        s64 out;
        svcGetProcessInfo(&out, CUR_PROCESS_HANDLE, 0x10001);

        memcpy(tid_buffer->data, &out, sizeof(s64));

        mi.FinishGood(0);
    }

    void Process_GetProductInfo(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        ArticProtocolCommon::Buffer* prod_code_buffer = mi.ReserveResultBuffer(0, sizeof(FS_ProductInfo));
        if (!prod_code_buffer) {
            return;
        }
        
        u32 pid;
        Result res = svcGetProcessId(&pid, CUR_PROCESS_HANDLE);
        if (R_SUCCEEDED(res)) res = FSUSER_GetProductInfo((FS_ProductInfo*)prod_code_buffer->data, pid);
        if (R_FAILED(res)) {
            logger.Error("Process_GetProductInfo: 0x%08X", res);
            mi.FinishInternalError();
            return;
        }

        mi.FinishGood(0);
    }

    void Process_GetExheader(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        ArticProtocolCommon::Buffer* exheader_buf = mi.ReserveResultBuffer(0, sizeof(lastAppExheader));
        if (!exheader_buf) {
            return;
        }
        memcpy(exheader_buf->data, &lastAppExheader, exheader_buf->bufferSize);

        mi.FinishGood(0);
    }

    void Process_ReadCode(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s32 offset, size;

        if (good) good = mi.GetParameterS32(offset);
        if (good) good = mi.GetParameterS32(size);
        if (good) good = mi.FinishInputParameters();
        if (!good) return;

        s64 out;
        if (R_FAILED(svcGetProcessInfo(&out, CUR_PROCESS_HANDLE, 0x10005))) {
            mi.FinishInternalError();
            return;
        }
        u8* start_addr = reinterpret_cast<u8*>(out);

        ArticProtocolCommon::Buffer* code_buf = mi.ReserveResultBuffer(0, size);
        if (!code_buf) {
            return;
        }
        memcpy(code_buf->data, start_addr + offset, size);

        mi.FinishGood(0);
    }

    static void _Process_ReadExefs(ArticProtocolServer::MethodInterface& mi, const char* section) {
        bool good = true;

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        // Set up FS_Path structures
        u8 path[0xC] = {0};
        u32* type = (u32*)path;
        char* name = (char*)(path + sizeof(u32));
        
        *type = 0x2; // ExeFS
        strcpy(name, section); // Icon

        FS_Path archPath = { PATH_EMPTY, 1, "" };
        FS_Path filePath = { PATH_BINARY, sizeof(path), path };

        // Open the RomFS file and mount it
        Handle fd = 0;
        Result rc = FSUSER_OpenFileDirectly(&fd, ARCHIVE_ROMFS, archPath, filePath, FS_OPEN_READ, 0);
        if (R_FAILED(rc)) {
            mi.FinishGood(rc);
            return;
        }

        u64 file_size;
        rc = FSFILE_GetSize(fd, &file_size);
        if (R_FAILED(rc)) {
            FSFILE_Close(fd);
            mi.FinishGood(rc);
            return;
        }

        ArticProtocolCommon::Buffer* icon_buf = mi.ReserveResultBuffer(0, static_cast<size_t>(file_size));
        if (!icon_buf) {
            FSFILE_Close(fd);
            return;
        }

        u32 bytes_read;
        rc = FSFILE_Read(fd, &bytes_read, 0, icon_buf->data, icon_buf->bufferSize);
        if (R_FAILED(rc)) {
            FSFILE_Close(fd);
            mi.ResizeLastResultBuffer(icon_buf, 0);
            mi.FinishGood(rc);
            return;
        }

        mi.ResizeLastResultBuffer(icon_buf, bytes_read);
        FSFILE_Close(fd);

        mi.FinishGood(0);
    }

    void Process_ReadIcon(ArticProtocolServer::MethodInterface& mi) {
        _Process_ReadExefs(mi, "icon");
    }

    void Process_ReadBanner(ArticProtocolServer::MethodInterface& mi) {
        _Process_ReadExefs(mi, "banner");
    }

    void Process_ReadLogo(ArticProtocolServer::MethodInterface& mi) {
        _Process_ReadExefs(mi, "logo");
    }

    bool GetFSPath(ArticProtocolServer::MethodInterface& mi, FS_Path& path) {
        void* pathPtr; size_t pathSize;
        
        if (!mi.GetParameterBuffer(pathPtr, pathSize))
            return false;

        path.type = reinterpret_cast<FS_Path*>(pathPtr)->type;
        path.size = reinterpret_cast<FS_Path*>(pathPtr)->size;
        if (pathSize < 8 || path.size != pathSize - 0x8) {
            mi.FinishInternalError();
            return false;
        }

        path.data = (u8*)pathPtr + 0x8;
        return true;
    }

    void FSUSER_OpenFileDirectly_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        s32 archiveID;
        FS_Path archPath;
        FS_Path filePath;
        s32 openFlags;
        s32 attributes;

        if (good) good = mi.GetParameterS32(archiveID);
        if (good) good = GetFSPath(mi, archPath);
        if (good) good = GetFSPath(mi, filePath);
        if (good) good = mi.GetParameterS32(openFlags);
        if (good) good = mi.GetParameterS32(attributes);
        
        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Handle out;
        Result res = FSUSER_OpenFileDirectly(&out, (FS_ArchiveID)archiveID, archPath, filePath, openFlags, attributes);

        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        ArticProtocolCommon::Buffer* handle_buf = mi.ReserveResultBuffer(0, sizeof(Handle));
        if (!handle_buf) {
            FSFILE_Close(out);
            return;
        }

        *reinterpret_cast<Handle*>(handle_buf->data) = out;
        openHandles[(u64)out] = HandleType::FILE;

        mi.FinishGood(res);
    }

    void FSUSER_OpenArchive_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        s32 archiveID;
        FS_Path archPath;

        if (good) good = mi.GetParameterS32(archiveID);
        if (good) good = GetFSPath(mi, archPath);
        
        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        FS_Archive out;
        Result res = FSUSER_OpenArchive(&out, (FS_ArchiveID)archiveID, archPath);

        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        ArticProtocolCommon::Buffer* handle_buf = mi.ReserveResultBuffer(0, sizeof(FS_Archive));
        if (!handle_buf) {
            FSUSER_CloseArchive(out);
            return;
        }

        *reinterpret_cast<FS_Archive*>(handle_buf->data) = out;
        openHandles[(u64)out] = HandleType::ARCHIVE;

        mi.FinishGood(res);
    }

    void FSUSER_CloseArchive_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        FS_Archive archive;

        if (good) good = mi.GetParameterS64(*reinterpret_cast<s64*>(&archive));
        
        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Result res = FSUSER_CloseArchive(archive);
        openHandles.erase((u64)archive);

        mi.FinishGood(res);
    }

    void FSUSER_OpenFile_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        FS_Archive archive;
        FS_Path filePath;
        s32 openFlags;
        s32 attributes;

        if (good) good = mi.GetParameterS64(*reinterpret_cast<s64*>(&archive));
        if (good) good = GetFSPath(mi, filePath);
        if (good) good = mi.GetParameterS32(openFlags);
        if (good) good = mi.GetParameterS32(attributes);
        
        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Handle out;
        Result res = FSUSER_OpenFile(&out, archive, filePath, openFlags, attributes);

        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        ArticProtocolCommon::Buffer* handle_buf = mi.ReserveResultBuffer(0, sizeof(Handle));
        if (!handle_buf) {
            FSFILE_Close(out);
            return;
        }

        *reinterpret_cast<Handle*>(handle_buf->data) = out;

        // Citra always asks for the size after opening a file, provided it here.
        u64 fileSize;
        Result res2 = FSFILE_GetSize(out, &fileSize);
        if (R_SUCCEEDED(res2)) {
            ArticProtocolCommon::Buffer* size_buf = mi.ReserveResultBuffer(1, sizeof(u64));
            if (!size_buf) {
                FSFILE_Close(out);
                return;
            }

            *reinterpret_cast<u64*>(size_buf->data) = fileSize;
        }
        openHandles[(u64)out] = HandleType::FILE;

        mi.FinishGood(res);
    }

    void FSUSER_CreateFile_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        FS_Archive archive;
        FS_Path filePath;
        s32 attributes;
        s64 fileSize;

        if (good) good = mi.GetParameterS64(*reinterpret_cast<s64*>(&archive));
        if (good) good = GetFSPath(mi, filePath);
        if (good) good = mi.GetParameterS32(attributes);
        if (good) good = mi.GetParameterS64(fileSize);
        
        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Handle out;
        Result res = FSUSER_CreateFile(archive, filePath, attributes, fileSize);

        mi.FinishGood(res);
    }

    void FSUSER_DeleteFile_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        FS_Archive archive;
        FS_Path filePath;

        if (good) good = mi.GetParameterS64(*reinterpret_cast<s64*>(&archive));
        if (good) good = GetFSPath(mi, filePath);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Result res = FSUSER_DeleteFile(archive, filePath);

        mi.FinishGood(res);
    }

    void FSUSER_RenameFile_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        FS_Archive srcarchive;
        FS_Path srcfilePath;
        FS_Archive dstarchive;
        FS_Path dstfilePath;

        if (good) good = mi.GetParameterS64(*reinterpret_cast<s64*>(&srcarchive));
        if (good) good = GetFSPath(mi, srcfilePath);
        if (good) good = mi.GetParameterS64(*reinterpret_cast<s64*>(&dstarchive));
        if (good) good = GetFSPath(mi, dstfilePath);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Result res = FSUSER_RenameFile(srcarchive, srcfilePath, dstarchive, dstfilePath);

        mi.FinishGood(res);
    }

    void FSUSER_OpenDirectory_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        FS_Archive archive;
        FS_Path dirPath;

        if (good) good = mi.GetParameterS64(*reinterpret_cast<s64*>(&archive));
        if (good) good = GetFSPath(mi, dirPath);
        
        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Handle out;
        Result res = FSUSER_OpenDirectory(&out, archive, dirPath);

        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        ArticProtocolCommon::Buffer* handle_buf = mi.ReserveResultBuffer(0, sizeof(Handle));
        if (!handle_buf) {
            FSDIR_Close(out);
            return;
        }

        *reinterpret_cast<Handle*>(handle_buf->data) = out;
        openHandles[(u64)out] = HandleType::DIR;

        mi.FinishGood(res);
    }

    void FSUSER_CreateDirectory_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        FS_Archive archive;
        FS_Path dirPath;
        s32 attributes;

        if (good) good = mi.GetParameterS64(*reinterpret_cast<s64*>(&archive));
        if (good) good = GetFSPath(mi, dirPath);
        if (good) good = mi.GetParameterS32(attributes);
        
        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Handle out;
        Result res = FSUSER_CreateDirectory(archive, dirPath, attributes);

        mi.FinishGood(res);
    }

    void FSUSER_DeleteDirectory_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        FS_Archive archive;
        FS_Path dirPath;

        if (good) good = mi.GetParameterS64(*reinterpret_cast<s64*>(&archive));
        if (good) good = GetFSPath(mi, dirPath);
        
        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Handle out;
        Result res = FSUSER_DeleteDirectory(archive, dirPath);

        mi.FinishGood(res);
    }

    void FSUSER_DeleteDirectoryRecursively_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        FS_Archive archive;
        FS_Path dirPath;

        if (good) good = mi.GetParameterS64(*reinterpret_cast<s64*>(&archive));
        if (good) good = GetFSPath(mi, dirPath);
        
        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Handle out;
        Result res = FSUSER_DeleteDirectoryRecursively(archive, dirPath);

        mi.FinishGood(res);
    }

    void FSUSER_RenameDirectory_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        FS_Archive srcarchive;
        FS_Path srcdirPath;
        FS_Archive dstarchive;
        FS_Path dstdirPath;

        if (good) good = mi.GetParameterS64(*reinterpret_cast<s64*>(&srcarchive));
        if (good) good = GetFSPath(mi, srcdirPath);
        if (good) good = mi.GetParameterS64(*reinterpret_cast<s64*>(&dstarchive));
        if (good) good = GetFSPath(mi, dstdirPath);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Result res = FSUSER_RenameDirectory(srcarchive, srcdirPath, dstarchive, dstdirPath);

        mi.FinishGood(res);
    }

    void FSUSER_ControlArchive_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        FS_Archive archive;
        FS_ArchiveAction action;
        void* input; size_t inputSize;
        s32 outputSize;

        if (good) good = mi.GetParameterS64(*reinterpret_cast<s64*>(&archive));
        if (good) good = mi.GetParameterS32(*reinterpret_cast<s32*>(&action));
        if (good) good = mi.GetParameterBuffer(input, inputSize);
        if (good) good = mi.GetParameterS32(outputSize);
        
        if (good) good = mi.FinishInputParameters();

        if (outputSize > 0x1000) {
            mi.FinishInternalError();
            return;
        }

        // Cannot use output buffer while using input at the same time, need to allocate
        void* output = malloc(outputSize); 

        Result res = FSUSER_ControlArchive(archive, action, input, inputSize, output, outputSize);

        ArticProtocolCommon::Buffer* out_buf = mi.ReserveResultBuffer(0, outputSize);
        if (!out_buf) {
            free(output);
            return;
        }

        memcpy(out_buf->data, output, outputSize);
        mi.FinishGood(res);
        free(output);
    }

    void FSUSER_GetFreeBytes_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        FS_Archive archive;

        if (good) good = mi.GetParameterS64(*reinterpret_cast<s64*>(&archive));

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        u64 freeBytes;
        Result res = FSUSER_GetFreeBytes(&freeBytes, archive);
        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        ArticProtocolCommon::Buffer* size_buf = mi.ReserveResultBuffer(0, sizeof(u64));
        if (!size_buf) {
            return;
        }

        *reinterpret_cast<u64*>(size_buf->data) = freeBytes;
        mi.FinishGood(res);
    }

    void FSUSER_GetFormatInfo_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        s32 archiveID;
        FS_Path path;

        if (good) good = mi.GetParameterS32(archiveID);
        if (good) good = GetFSPath(mi, path);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        u32 totalSize; u32 directories; u32 files; bool duplicateData;
        Result res = FSUSER_GetFormatInfo(&totalSize, &directories, &files, &duplicateData, (FS_ArchiveID)archiveID, path);

        struct {
            u32 total_size;
            u32 number_directories;
            u32 number_files;
            u8 duplicate_data;
        } archive_format_info;
        static_assert(sizeof(archive_format_info) == 16);

        archive_format_info.total_size = totalSize;
        archive_format_info.number_directories = directories;
        archive_format_info.number_files = files;
        archive_format_info.duplicate_data = duplicateData;

        ArticProtocolCommon::Buffer* format_info_buf = mi.ReserveResultBuffer(0, sizeof(archive_format_info));
        if (!format_info_buf) {
            return;
        }

        memcpy(format_info_buf->data, &archive_format_info, sizeof(archive_format_info));        
        mi.FinishGood(res);
    }

    void FSUSER_FormatSaveData_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        s32 archiveID;
        FS_Path path;
        s32 blocks; s32 directories; s32 files; s32 directoryBuckets; s32 fileBuckets; bool duplicateData;

        if (good) good = mi.GetParameterS32(archiveID);
        if (good) good = GetFSPath(mi, path);
        if (good) good = mi.GetParameterS32(blocks);
        if (good) good = mi.GetParameterS32(directories);
        if (good) good = mi.GetParameterS32(files);
        if (good) good = mi.GetParameterS32(directoryBuckets);
        if (good) good = mi.GetParameterS32(fileBuckets);
        if (good) good = mi.GetParameterS8(*reinterpret_cast<s8*>(&duplicateData));

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Result res = FSUSER_FormatSaveData((FS_ArchiveID)archiveID, path, blocks, directories, files, directoryBuckets, fileBuckets, duplicateData);

        mi.FinishGood(res);
    }

    void FSUSER_ObsoletedSetSaveDataSecureValue_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        s64 secure_value;
        s32 slot;
        s32 title_id;
        s8 title_variation;

        if (good) good = mi.GetParameterS64(secure_value);
        if (good) good = mi.GetParameterS32(slot);
        if (good) good = mi.GetParameterS32(title_id);
        if (good) good = mi.GetParameterS8(title_variation);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Result res = FSUSER_SetSaveDataSecureValue((u64)secure_value, (FS_SecureValueSlot)slot, title_id, title_variation);

        mi.FinishGood(res);
    }

    void FSUSER_ObsoletedGetSaveDataSecureValue_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        
        s32 slot;
        s32 title_id;
        s8 title_variation;

        if (good) good = mi.GetParameterS32(slot);
        if (good) good = mi.GetParameterS32(title_id);
        if (good) good = mi.GetParameterS8(title_variation);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        bool exists; u64 secure_value;
        Result res = FSUSER_GetSaveDataSecureValue(&exists, &secure_value, (FS_SecureValueSlot)slot, title_id, title_variation);

        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        struct {
            bool exists;
            u64 secure_value;
        } secure_value_result;
        static_assert(sizeof(secure_value_result) == 0x10);

        ArticProtocolCommon::Buffer* sec_val_buf = mi.ReserveResultBuffer(0, sizeof(secure_value_result));
        if (!sec_val_buf) {
            return;
        }

        secure_value_result.exists = exists;
        secure_value_result.secure_value = secure_value;
        memcpy(sec_val_buf->data, &secure_value_result, sizeof(secure_value_result));
        mi.FinishGood(res);
    }

    void FSUSER_ControlSecureSave_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        s32 action;
        void* input; size_t inputSize;
        s32 outputSize;

        if (good) good = mi.GetParameterS32(action);
        if (good) good = mi.GetParameterBuffer(input, inputSize);
        if (good) good = mi.GetParameterS32(outputSize);
        
        if (good) good = mi.FinishInputParameters();

        if (outputSize > 0x1000) {
            mi.FinishInternalError();
            return;
        }

        // Cannot use output buffer while using input at the same time, need to allocate
        void* output = malloc(outputSize); 

        Result res = FSUSER_ControlSecureSave((FS_SecureSaveAction)action, input, inputSize, output, outputSize);

        ArticProtocolCommon::Buffer* out_buf = mi.ReserveResultBuffer(0, outputSize);
        if (!out_buf) {
            free(output);
            return;
        }

        memcpy(out_buf->data, output, outputSize);
        mi.FinishGood(res);
        free(output);
    }

    void FSUSER_SetSaveDataSecureValue_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        FS_Archive archive;
        s32 slot;
        s64 secure_value;
        s8 flush;

        if (good) good = mi.GetParameterS64(*reinterpret_cast<s64*>(&archive));
        if (good) good = mi.GetParameterS32(slot);
        if (good) good = mi.GetParameterS64(secure_value);
        if (good) good = mi.GetParameterS8(flush);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Result res = FSUSER_NewSetSaveDataSecureValue(archive, (u64)secure_value, (FS_SecureValueSlot)slot, flush != 0);

        mi.FinishGood(res);
    }

    void FSUSER_GetSaveDataSecureValue_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        
        FS_Archive archive;
        s32 slot;

        if (good) good = mi.GetParameterS64(*reinterpret_cast<s64*>(&archive));
        if (good) good = mi.GetParameterS32(slot);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        bool exists, isGamecard; u64 secure_value;
        Result res = FSUSER_NewGetSaveDataSecureValue(&exists, &isGamecard, &secure_value, archive, (FS_SecureValueSlot)slot);

        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        struct {
            bool exists;
            bool isGamecard;
            u64 secure_value;
        } secure_value_result;
        static_assert(sizeof(secure_value_result) == 0x10);

        ArticProtocolCommon::Buffer* sec_val_buf = mi.ReserveResultBuffer(0, sizeof(secure_value_result));
        if (!sec_val_buf) {
            return;
        }

        secure_value_result.exists = exists;
        secure_value_result.isGamecard = isGamecard;
        secure_value_result.secure_value = secure_value;
        memcpy(sec_val_buf->data, &secure_value_result, sizeof(secure_value_result));
        mi.FinishGood(res);
    }

    void FSUSER_SetThisSaveDataSecureValue_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        s32 slot;
        s64 secure_value;

        if (good) good = mi.GetParameterS32(slot);
        if (good) good = mi.GetParameterS64(secure_value);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Result res = FSUSER_SetThisSaveDataSecureValue((u64)secure_value, (FS_SecureValueSlot)slot);

        mi.FinishGood(res);
    }

    void FSUSER_GetThisSaveDataSecureValue_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        
        s32 slot;
        
        if (good) good = mi.GetParameterS32(slot);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        bool exists, isGamecard; u64 secure_value;
        Result res = FSUSER_GetThisSaveDataSecureValue(&exists, &isGamecard, &secure_value, (FS_SecureValueSlot)slot);

        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        struct {
            bool exists;
            bool isGamecard;
            u64 secure_value;
        } secure_value_result;
        static_assert(sizeof(secure_value_result) == 0x10);

        ArticProtocolCommon::Buffer* sec_val_buf = mi.ReserveResultBuffer(0, sizeof(secure_value_result));
        if (!sec_val_buf) {
            return;
        }

        secure_value_result.exists = exists;
        secure_value_result.isGamecard = isGamecard;
        secure_value_result.secure_value = secure_value;
        memcpy(sec_val_buf->data, &secure_value_result, sizeof(secure_value_result));
        mi.FinishGood(res);
    }

    void FSUSER_CreateExtSaveData_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        FS_ExtSaveDataInfo info;
        void* formatInfoPtr; size_t formatInfoPtrSize;
        s32 directories, files;
        s64 size_limit;
        void* smdhIconPtr; size_t smdhIconPtrSize;
        
        if (good) good = mi.GetParameterBuffer(formatInfoPtr, formatInfoPtrSize);
        if (good) good = mi.GetParameterS32(directories);
        if (good) good = mi.GetParameterS32(files);
        if (good) good = mi.GetParameterS64(size_limit);
        if (good) good = mi.GetParameterBuffer(smdhIconPtr, smdhIconPtrSize);
        
        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        if (formatInfoPtrSize != sizeof(FS_ExtSaveDataInfo)) {
            mi.FinishInternalError();
            return;
        }
        memcpy(&info, formatInfoPtr, formatInfoPtrSize);

        Result res = FSUSER_CreateExtSaveData(info, directories, files, size_limit, static_cast<u32>(smdhIconPtrSize), reinterpret_cast<u8*>(smdhIconPtr));

        mi.FinishGood(res);
    }

    void FSUSER_DeleteExtSaveData_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        FS_ExtSaveDataInfo info;
        void* formatInfoPtr; size_t formatInfoPtrSize;
        
        if (good) good = mi.GetParameterBuffer(formatInfoPtr, formatInfoPtrSize);
        
        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        if (formatInfoPtrSize != sizeof(FS_ExtSaveDataInfo)) {
            mi.FinishInternalError();
            return;
        }
        memcpy(&info, formatInfoPtr, formatInfoPtrSize);

        Result res = FSUSER_DeleteExtSaveData(info);

        mi.FinishGood(res);
    }

    void FSUSER_CreateSysSaveData_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s8 duplicate_data;
        s32 high, low, total_size, block_size, number_directories, number_files, number_directory_buckets, number_file_buckets;

        if (good) good = mi.GetParameterS32(high);
        if (good) good = mi.GetParameterS32(low);
        if (good) good = mi.GetParameterS32(total_size);
        if (good) good = mi.GetParameterS32(block_size);
        if (good) good = mi.GetParameterS32(number_directories);
        if (good) good = mi.GetParameterS32(number_files);
        if (good) good = mi.GetParameterS32(number_directory_buckets);
        if (good) good = mi.GetParameterS32(number_file_buckets);
        if (good) good = mi.GetParameterS8(duplicate_data);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        // Citra has this structure wrong, high is actually the first 4 bytes of FS_SystemSaveDataInfo and low is saveId
        FS_SystemSaveDataInfo sinfo;
        memcpy(&sinfo, &high, sizeof(high));
        sinfo.saveId = low;

        Result res = FSUSER_CreateSystemSaveData(sinfo, (u32)total_size, (u32)block_size, (u32)number_directories, (u32)number_files, (u32)number_directory_buckets, (u32)number_file_buckets, duplicate_data != 0);
        mi.FinishGood(res);
    }

    void FSFILE_Close_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s32 handle;

        if (good) good = mi.GetParameterS32(handle);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Result res = FSFILE_Close(handle);
        openHandles.erase((u64)handle);

        mi.FinishGood(res);
    }

    void FSFILE_SetSize_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s32 handle;
        s64 size;

        if (good) good = mi.GetParameterS32(handle);
        if (good) good = mi.GetParameterS64(size);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Result res = FSFILE_SetSize(handle, size);

        mi.FinishGood(res);
    }

    void FSFILE_GetSize_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s32 handle;

        if (good) good = mi.GetParameterS32(handle);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        u64 fileSize;
        Result res = FSFILE_GetSize(handle, &fileSize);
        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        ArticProtocolCommon::Buffer* size_buf = mi.ReserveResultBuffer(0, sizeof(u64));
        if (!size_buf) {
            return;
        }

        *reinterpret_cast<u64*>(size_buf->data) = fileSize;
        mi.FinishGood(res);
    }

    void FSFILE_SetAttributes_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s32 handle;
        s32 attributes;

        if (good) good = mi.GetParameterS32(handle);
        if (good) good = mi.GetParameterS32(attributes);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Result res = FSFILE_SetAttributes(handle, attributes);

        mi.FinishGood(res);
    }

    void FSFILE_GetAttributes_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s32 handle;

        if (good) good = mi.GetParameterS32(handle);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        u32 attributes;
        Result res = FSFILE_GetAttributes(handle, &attributes);
        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        ArticProtocolCommon::Buffer* size_buf = mi.ReserveResultBuffer(0, sizeof(u32));
        if (!size_buf) {
            return;
        }

        *reinterpret_cast<u32*>(size_buf->data) = attributes;
        mi.FinishGood(res);
    }

    void FSFILE_Read_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s32 handle, size;
        s64 offset;
        u32 bytes_read;

        if (good) good = mi.GetParameterS32(handle);
        if (good) good = mi.GetParameterS64(offset);
        if (good) good = mi.GetParameterS32(size);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        logger.Debug("Read o=0x%08X, l=0x%08X", (u32)offset, (u32)size);

        ArticProtocolCommon::Buffer* read_buf = mi.ReserveResultBuffer(0, size);
        if (!read_buf) {
            return;
        }

        Result res = FSFILE_Read(handle, &bytes_read, offset, read_buf->data, read_buf->bufferSize);
        if (R_FAILED(res)) {
            mi.ResizeLastResultBuffer(read_buf, 0);
            mi.FinishGood(res);
            return;
        }

        mi.ResizeLastResultBuffer(read_buf, bytes_read);
        mi.FinishGood(res);
    }

    void FSFILE_Write_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s32 handle, size, flags;
        s64 offset;
        u32 bytes_written;
        void* dataPtr; size_t dataPtrSize;

        if (good) good = mi.GetParameterS32(handle);
        if (good) good = mi.GetParameterS64(offset);
        if (good) good = mi.GetParameterS32(size);
        if (good) good = mi.GetParameterS32(flags);
        if (good) good = mi.GetParameterBuffer(dataPtr, dataPtrSize);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        if (dataPtrSize != size) {
            mi.FinishInternalError();
            return;
        }

        Result res = FSFILE_Write(handle, &bytes_written, offset, dataPtr, size, flags);
        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        ArticProtocolCommon::Buffer* bytes_written_buf = mi.ReserveResultBuffer(0, sizeof(u32));
        if (!bytes_written_buf) {
            return;
        }

        *reinterpret_cast<u32*>(bytes_written_buf->data) = bytes_written;
        mi.FinishGood(res);
    }

    void FSFILE_Flush_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s32 handle;

        if (good) good = mi.GetParameterS32(handle);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Result res = FSFILE_Flush(handle);

        mi.FinishGood(res);
    }

    void FSDIR_Read_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s32 handle;
        s32 entryCount;

        if (good) good = mi.GetParameterS32(handle);
        if (good) good = mi.GetParameterS32(entryCount);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        ArticProtocolCommon::Buffer* read_dir_buf = mi.ReserveResultBuffer(0, entryCount * sizeof(FS_DirectoryEntry));
        if (!read_dir_buf) {
            return;
        }

        u32 entries_read;
        Result res = FSDIR_Read(handle, &entries_read, entryCount, reinterpret_cast<FS_DirectoryEntry*>(read_dir_buf->data));
        if (R_FAILED(res)) {
            mi.ResizeLastResultBuffer(read_dir_buf, 0);
            mi.FinishGood(res);
            return;
        }

        mi.ResizeLastResultBuffer(read_dir_buf, entries_read * sizeof(FS_DirectoryEntry));
        mi.FinishGood(res);
    }

    void FSDIR_Close_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s32 handle;

        if (good) good = mi.GetParameterS32(handle);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Result res = FSDIR_Close(handle);
        openHandles.erase((u64)handle);

        mi.FinishGood(res);
    }

    void AM_GetTitleCount_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s8 mediatype;

        if (good) good = mi.GetParameterS8(mediatype);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        CTRPluginFramework::Lock l(amMutex);
        Result res = amInit();
        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        u32 count = 0;
        res = AM_GetTitleCount(static_cast<FS_MediaType>(mediatype), &count);
        amExit();
        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        ArticProtocolCommon::Buffer* count_buf = mi.ReserveResultBuffer(0, sizeof(u32));
        if (!count_buf) {
            return;
        }

        *reinterpret_cast<u32*>(count_buf->data) = count;
        mi.FinishGood(res);
    }

    void AM_GetTitleList_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s32 count;
        s8 mediatype;

        if (good) good = mi.GetParameterS32(count);
        if (good) good = mi.GetParameterS8(mediatype);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        ArticProtocolCommon::Buffer* title_buf = mi.ReserveResultBuffer(0, count * sizeof(u64));
        if (!title_buf) {
            return;
        }

        CTRPluginFramework::Lock l(amMutex);
        Result res = amInit();
        if (R_FAILED(res)) {
            mi.ResizeLastResultBuffer(title_buf, 0);
            mi.FinishGood(res);
            return;
        }

        u32 titlesRead = 0;
        res = AM_GetTitleList(&titlesRead, static_cast<FS_MediaType>(mediatype), count, reinterpret_cast<u64*>(title_buf->data));
        amExit();
        if (R_FAILED(res)) {
            mi.ResizeLastResultBuffer(title_buf, 0);
            mi.FinishGood(res);
            return;
        }

        mi.ResizeLastResultBuffer(title_buf, sizeof(u64) * titlesRead);
        mi.FinishGood(res);
    }

    void AM_GetTitleInfo_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s8 mediatype;
        s8 ignorePlatform;
        void* titleList; size_t titleListSize;

        if (good) good = mi.GetParameterS8(mediatype);
        if (good) good = mi.GetParameterBuffer(titleList, titleListSize);
        if (good) good = mi.GetParameterS8(ignorePlatform);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        CTRPluginFramework::Lock l(amMutex);
        Result res = amInit();
        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        // Cannot use output buffer while using input at the same time, need to allocate
        u64* titleIDs = (u64*)malloc(titleListSize);
        memcpy(titleIDs, titleList, titleListSize);
        u32 count = titleListSize / sizeof(u64);

        ArticProtocolCommon::Buffer* title_buf = mi.ReserveResultBuffer(0, count * sizeof(AM_TitleEntry));
        if (!title_buf) {
            free(titleIDs);
            return;
        }

        if (ignorePlatform) {
            res = AM_GetTitleInfoIgnorePlatform(static_cast<FS_MediaType>(mediatype), count, titleIDs, reinterpret_cast<AM_TitleEntry*>(title_buf->data));
        } else {
            res = AM_GetTitleInfo(static_cast<FS_MediaType>(mediatype), count, titleIDs, reinterpret_cast<AM_TitleEntry*>(title_buf->data));
        }
        free(titleIDs);
        amExit();
        if (R_FAILED(res)) {
            mi.ResizeLastResultBuffer(title_buf, 0);
            mi.FinishGood(res);
            return;
        }

        mi.FinishGood(res);
    }

    void AMAPP_GetDLCContentInfoCount_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s8 mediatype;
        s64 title_id;

        if (good) good = mi.GetParameterS8(mediatype);
        if (good) good = mi.GetParameterS64(title_id);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        CTRPluginFramework::Lock l(amMutex);
        Result res = amAppInit();
        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        u32 count = 0;
        res = AMAPP_GetDLCContentInfoCount(&count, static_cast<FS_MediaType>(mediatype), static_cast<u64>(title_id));
        amExit();
        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        ArticProtocolCommon::Buffer* count_buf = mi.ReserveResultBuffer(0, sizeof(u32));
        if (!count_buf) {
            return;
        }

        *reinterpret_cast<u32*>(count_buf->data) = count;
        mi.FinishGood(res);
    }

    void AMAPP_FindDLCContentInfos_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s8 mediatype;
        s64 title_id;
        void* contentList; size_t contentListSize;

        if (good) good = mi.GetParameterS8(mediatype);
        if (good) good = mi.GetParameterS64(title_id);
        if (good) good = mi.GetParameterBuffer(contentList, contentListSize);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        CTRPluginFramework::Lock l(amMutex);
        Result res = amAppInit();
        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        // Cannot use output buffer while using input at the same time, need to allocate
        u16* contentIDs = (u16*)malloc(contentListSize);
        memcpy(contentIDs, contentList, contentListSize);
        u32 count = contentListSize / sizeof(u16);

        ArticProtocolCommon::Buffer* title_buf = mi.ReserveResultBuffer(0, count * sizeof(AM_ContentInfo));
        if (!title_buf) {
            free(contentIDs);
            return;
        }

        res = AMAPP_FindDLCContentInfos(static_cast<FS_MediaType>(mediatype), title_id, count, contentIDs, reinterpret_cast<AM_ContentInfo*>(title_buf->data));
        free(contentIDs);
        amExit();
        if (R_FAILED(res)) {
            mi.ResizeLastResultBuffer(title_buf, 0);
            mi.FinishGood(res);
            return;
        }

        mi.FinishGood(res);
    }

    void AMAPP_ListDLCContentInfos_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s32 count;
        s8 mediatype;
        s64 title_id;
        s32 start_index;

        if (good) good = mi.GetParameterS32(count);
        if (good) good = mi.GetParameterS8(mediatype);
        if (good) good = mi.GetParameterS64(title_id);
        if (good) good = mi.GetParameterS32(start_index);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        ArticProtocolCommon::Buffer* content_buf = mi.ReserveResultBuffer(0, count * sizeof(AM_ContentInfo));
        if (!content_buf) {
            return;
        }

        CTRPluginFramework::Lock l(amMutex);
        Result res = amAppInit();
        if (R_FAILED(res)) {
            mi.ResizeLastResultBuffer(content_buf, 0);
            mi.FinishGood(res);
            return;
        }

        u32 contentRead = 0;
        res = AMAPP_ListDLCContentInfos(&contentRead, static_cast<FS_MediaType>(mediatype), title_id, count, start_index, reinterpret_cast<AM_ContentInfo*>(content_buf->data));
        amExit();
        if (R_FAILED(res)) {
            mi.ResizeLastResultBuffer(content_buf, 0);
            mi.FinishGood(res);
            return;
        }

        mi.ResizeLastResultBuffer(content_buf, sizeof(AM_ContentInfo) * contentRead);
        mi.FinishGood(res);
    }

    void AMAPP_GetDLCTitleInfos_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s8 mediatype;
        void* titleList; size_t titleListSize;

        if (good) good = mi.GetParameterS8(mediatype);
        if (good) good = mi.GetParameterBuffer(titleList, titleListSize);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        CTRPluginFramework::Lock l(amMutex);
        Result res = amAppInit();
        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        // Cannot use output buffer while using input at the same time, need to allocate
        u64* titleIDs = (u64*)malloc(titleListSize);
        memcpy(titleIDs, titleList, titleListSize);
        u32 count = titleListSize / sizeof(u64);

        ArticProtocolCommon::Buffer* title_buf = mi.ReserveResultBuffer(0, count * sizeof(AM_TitleEntry));
        if (!title_buf) {
            free(titleIDs);
            return;
        }
        
        res = AMAPP_GetDLCTitleInfos(static_cast<FS_MediaType>(mediatype), count, titleIDs, reinterpret_cast<AM_TitleEntry*>(title_buf->data));
        free(titleIDs);
        amExit();
        if (R_FAILED(res)) {
            mi.ResizeLastResultBuffer(title_buf, 0);
            mi.FinishGood(res);
            return;
        }

        mi.FinishGood(res);
    }

    void AMAPP_ListDataTitleTicketInfos_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s32 count;
        s64 title_id;
        s32 start_index;

        if (good) good = mi.GetParameterS32(count);
        if (good) good = mi.GetParameterS64(title_id);
        if (good) good = mi.GetParameterS32(start_index);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        ArticProtocolCommon::Buffer* content_buf = mi.ReserveResultBuffer(0, count * sizeof(AM_TicketInfo));
        if (!content_buf) {
            return;
        }

        CTRPluginFramework::Lock l(amMutex);
        Result res = amAppInit();
        if (R_FAILED(res)) {
            mi.ResizeLastResultBuffer(content_buf, 0);
            mi.FinishGood(res);
            return;
        }

        u32 ticketsRead = 0;
        res = AMAPP_ListDataTitleTicketInfos(&ticketsRead, title_id, count, start_index, reinterpret_cast<AM_TicketInfo*>(content_buf->data));
        amExit();
        if (R_FAILED(res)) {
            mi.ResizeLastResultBuffer(content_buf, 0);
            mi.FinishGood(res);
            return;
        }

        mi.ResizeLastResultBuffer(content_buf, sizeof(AM_TicketInfo) * ticketsRead);
        mi.FinishGood(res);
    }

    void AMAPP_GetPatchTitleInfos_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s8 mediatype;
        void* titleList; size_t titleListSize;

        if (good) good = mi.GetParameterS8(mediatype);
        if (good) good = mi.GetParameterBuffer(titleList, titleListSize);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        CTRPluginFramework::Lock l(amMutex);
        Result res = amAppInit();
        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        // Cannot use output buffer while using input at the same time, need to allocate
        u64* titleIDs = (u64*)malloc(titleListSize);
        memcpy(titleIDs, titleList, titleListSize);
        u32 count = titleListSize / sizeof(u64);

        ArticProtocolCommon::Buffer* title_buf = mi.ReserveResultBuffer(0, count * sizeof(AM_TitleEntry));
        if (!title_buf) {
            free(titleIDs);
            return;
        }
        
        res = AMAPP_GetPatchTitleInfos(static_cast<FS_MediaType>(mediatype), count, titleIDs, reinterpret_cast<AM_TitleEntry*>(title_buf->data));
        free(titleIDs);
        amExit();
        if (R_FAILED(res)) {
            mi.ResizeLastResultBuffer(title_buf, 0);
            mi.FinishGood(res);
            return;
        }

        mi.FinishGood(res);
    }

    void CFGU_GetConfigInfoBlk2_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s32 block_id, size;

        if (good) good = mi.GetParameterS32(block_id);
        if (good) good = mi.GetParameterS32(size);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        CTRPluginFramework::Lock l(cfgMutex);
        Result res = cfguInit();
        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        ArticProtocolCommon::Buffer* conf_buf = mi.ReserveResultBuffer(0, size);
        if (!conf_buf) {
            return;
        }

        res = CFGU_GetConfigInfoBlk2(static_cast<u32>(size), static_cast<u32>(block_id), conf_buf->data);
        cfguExit();
        if (R_FAILED(res)) {
            mi.ResizeLastResultBuffer(conf_buf, 0);
            mi.FinishGood(res);
            return;
        }

        mi.FinishGood(res);
    }

    void ArticController::Handler(void* arg) {
        using namespace ArticController;

        struct ControllerPacket {
            u32 id;
            u32 pad;
            circlePosition c_pad;
            touchPosition touch;
            circlePosition c_stick;
            accelVector accel;
            angularRate gyro;
        } packet;
        u32 current_id = 0;
        static_assert(sizeof(packet) == 0x20, "ControllerPacket invalid size");

        constexpr int port = SERVER_PORT + 10;
        struct sockaddr_in addr = {0};
        socklen_t addr_size = static_cast<socklen_t>(sizeof(addr));
        int res, failedCount = 0;

        socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd < 0) {
            logger.Error("ArticController: Cannot create socket");
            socket_ready = true;
            threadExit(1);
            return;
        }

        if (!ArticProtocolServer::SetNonBlock(socket_fd, true)) {
            logger.Error("ArticController: Cannot set non-block");
            close(socket_fd);
            socket_fd = -1;
            socket_ready = true;
            threadExit(1);
            return;
        }

        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port        = htons(port);
        res = bind(socket_fd, (struct sockaddr *)&addr, addr_size);
        if (res < 0) {
            logger.Error("ArticController: Failed to bind() to port %d", port);
            close(socket_fd);
            socket_fd = -1;
            socket_ready = true;
            threadExit(1);
            return;
        }
        
        socket_ready = true;

        u8 a;
        size_t transfered = ArticProtocolServer::RecvFrom(socket_fd, &a, sizeof(a), &addr, &addr_size);
        if (transfered > 0) {
            logger.Debug("ArticController: Started");
        } else {
            logger.Error("ArticController: Error reading from socket");
            close(socket_fd);
            socket_fd = -1;
            threadExit(1);
            return;
        }

        constexpr CTRPluginFramework::Time INTERVAL = CTRPluginFramework::Milliseconds(2);
        // hid scan input is done on the main thread, no need to do it here.
        while (thread_run) {
            CTRPluginFramework::Clock clock;

            if (isControllerMode) {
                packet.id = current_id++;
                packet.pad = hidKeysHeld();
                hidCircleRead(&packet.c_pad);
                hidTouchRead(&packet.touch);
                irrstCstickRead(&packet.c_stick);
                hidAccelRead(&packet.accel);
                hidGyroRead(&packet.gyro);

                if (ArticProtocolServer::SendTo(socket_fd, &packet, sizeof(packet), &addr, &addr_size) <= 0) {
                    if (failedCount++ >= 1000) {
                        logger.Error("ArticController: Error writing to socket");
                        break;
                    }
                } else {
                    failedCount = 0;
                }
            }

            CTRPluginFramework::Time elapsed = clock.GetElapsedTime();
            if (elapsed < INTERVAL) {
                svcSleepThread((INTERVAL - elapsed).AsMicroseconds() * 1000);
            }
        }

        close(socket_fd);
        socket_fd = -1;
        threadExit(1);
    }

    static bool stopController(void);
    void Controller_Start(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        constexpr int port = SERVER_PORT + 10;
        ArticProtocolCommon::Buffer* conf_buf = mi.ReserveResultBuffer(0, sizeof(int));
        if (!conf_buf) {
            return;
        }
        *(int*)conf_buf->data = port;

        stopController();

        s32 prio = 0;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        ArticController::socket_ready = false;
        ArticController::thread_run = true;
        ArticController::thread = threadCreate(ArticController::Handler, nullptr, 0x800, prio + 1, -2, false);
        logger.Debug("ArticController: Starting...");
        isControllerMode = true;

        while (!ArticController::socket_ready) {
            svcSleepThread(1000000);
        }
        ArticController::socket_ready = false;

        mi.FinishGood(0);
    }

    void HIDUSER_EnableAccelerometer_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        if (good) mi.FinishInputParameters();

        Result res = HIDUSER_EnableAccelerometer();

        mi.FinishGood(res);
    }

    void HIDUSER_DisableAccelerometer_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        if (good) mi.FinishInputParameters();

        Result res = HIDUSER_DisableAccelerometer();

        mi.FinishGood(res);
    }

    void HIDUSER_EnableGyroscope_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        if (good) mi.FinishInputParameters();

        Result res = HIDUSER_EnableGyroscope();

        mi.FinishGood(res);
    }

    void HIDUSER_DisableGyroscope_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        if (good) mi.FinishInputParameters();

        Result res = HIDUSER_DisableGyroscope();

        mi.FinishGood(res);
    }

    void HIDUSER_GetGyroscopeRawToDpsCoefficient_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        if (good) mi.FinishInputParameters();

        float coef;
        Result res = HIDUSER_GetGyroscopeRawToDpsCoefficient(&coef);
        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        ArticProtocolCommon::Buffer* coef_buf = mi.ReserveResultBuffer(0, sizeof(float));
        if (!coef_buf) {
            return;
        }
        *(float*)coef_buf->data = coef;

        mi.FinishGood(res);
    }

    
    void HIDUSER_GetGyroscopeCalibrateParam_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        if (good) mi.FinishInputParameters();

        GyroscopeCalibrateParam param = { 0 };
        Result res = HIDUSER_GetGyroscopeCalibrateParam(&param);
        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        ArticProtocolCommon::Buffer* coef_buf = mi.ReserveResultBuffer(0, sizeof(param));
        if (!coef_buf) {
            return;
        }
        *(GyroscopeCalibrateParam*)coef_buf->data = param;

        mi.FinishGood(res);
    }

    template<std::size_t N>
    constexpr auto& METHOD_NAME(char const (&s)[N]) {
        static_assert(N < sizeof(ArticProtocolCommon::RequestPacket::method), "String exceeds 32 bytes!");
        return s;
    }

    std::map<std::string, void(*)(ArticProtocolServer::MethodInterface& mi)> functionHandlers = {
        {METHOD_NAME("Process_GetTitleID"), Process_GetTitleID},
        {METHOD_NAME("Process_GetProductInfo"), Process_GetProductInfo},
        {METHOD_NAME("Process_GetExheader"), Process_GetExheader},
        {METHOD_NAME("Process_ReadCode"), Process_ReadCode},
        {METHOD_NAME("Process_ReadIcon"), Process_ReadIcon},
        {METHOD_NAME("Process_ReadBanner"), Process_ReadBanner},
        {METHOD_NAME("Process_ReadLogo"), Process_ReadLogo},
        {METHOD_NAME("FSUSER_OpenFileDirectly"), FSUSER_OpenFileDirectly_},
        {METHOD_NAME("FSUSER_OpenArchive"), FSUSER_OpenArchive_},
        {METHOD_NAME("FSUSER_CloseArchive"), FSUSER_CloseArchive_},
        {METHOD_NAME("FSUSER_OpenFile"), FSUSER_OpenFile_},
        {METHOD_NAME("FSUSER_CreateFile"), FSUSER_CreateFile_},
        {METHOD_NAME("FSUSER_DeleteFile"), FSUSER_DeleteFile_},
        {METHOD_NAME("FSUSER_RenameFile"), FSUSER_RenameFile_},
        {METHOD_NAME("FSUSER_OpenDirectory"), FSUSER_OpenDirectory_},
        {METHOD_NAME("FSUSER_CreateDirectory"), FSUSER_CreateDirectory_},
        {METHOD_NAME("FSUSER_DeleteDirectory"), FSUSER_DeleteDirectory_},
        {METHOD_NAME("FSUSER_DeleteDirectoryRec"), FSUSER_DeleteDirectoryRecursively_},
        {METHOD_NAME("FSUSER_RenameDirectory"), FSUSER_RenameDirectory_},
        {METHOD_NAME("FSUSER_ControlArchive"), FSUSER_ControlArchive_},
        {METHOD_NAME("FSUSER_GetFreeBytes"), FSUSER_GetFreeBytes_},
        {METHOD_NAME("FSUSER_GetFormatInfo"), FSUSER_GetFormatInfo_},
        {METHOD_NAME("FSUSER_FormatSaveData"), FSUSER_FormatSaveData_},
        {METHOD_NAME("FSUSER_ObsSetSaveDataSecureVal"), FSUSER_ObsoletedSetSaveDataSecureValue_},
        {METHOD_NAME("FSUSER_ObsGetSaveDataSecureVal"), FSUSER_ObsoletedGetSaveDataSecureValue_},
        {METHOD_NAME("FSUSER_ControlSecureSave"), FSUSER_ControlSecureSave_},
        {METHOD_NAME("FSUSER_SetSaveDataSecureValue"), FSUSER_SetSaveDataSecureValue_},
        {METHOD_NAME("FSUSER_GetSaveDataSecureValue"), FSUSER_GetSaveDataSecureValue_},
        {METHOD_NAME("FSUSER_SetThisSaveDataSecVal"), FSUSER_SetThisSaveDataSecureValue_},
        {METHOD_NAME("FSUSER_GetThisSaveDataSecVal"), FSUSER_GetThisSaveDataSecureValue_},
        {METHOD_NAME("FSUSER_CreateExtSaveData"), FSUSER_CreateExtSaveData_},
        {METHOD_NAME("FSUSER_DeleteExtSaveData"), FSUSER_DeleteExtSaveData_},
        {METHOD_NAME("FSUSER_CreateSysSaveData"), FSUSER_CreateSysSaveData_},
        {METHOD_NAME("FSFILE_Close"), FSFILE_Close_},
        {METHOD_NAME("FSFILE_SetAttributes"), FSFILE_SetAttributes_},
        {METHOD_NAME("FSFILE_GetAttributes"), FSFILE_GetAttributes_},
        {METHOD_NAME("FSFILE_SetSize"), FSFILE_SetSize_},
        {METHOD_NAME("FSFILE_GetSize"), FSFILE_GetSize_},
        {METHOD_NAME("FSFILE_Read"), FSFILE_Read_},
        {METHOD_NAME("FSFILE_Write"), FSFILE_Write_},
        {METHOD_NAME("FSFILE_Flush"), FSFILE_Flush_},
        {METHOD_NAME("FSDIR_Read"), FSDIR_Read_},
        {METHOD_NAME("FSDIR_Close"), FSDIR_Close_},
        {METHOD_NAME("AM_GetTitleCount"), AM_GetTitleCount_},
        {METHOD_NAME("AM_GetTitleList"), AM_GetTitleList_},
        {METHOD_NAME("AM_GetTitleInfo"), AM_GetTitleInfo_},
        {METHOD_NAME("AMAPP_GetDLCContentInfoCount"), AMAPP_GetDLCContentInfoCount_},
        {METHOD_NAME("AMAPP_FindDLCContentInfos"), AMAPP_FindDLCContentInfos_},
        {METHOD_NAME("AMAPP_ListDLCContentInfos"), AMAPP_ListDLCContentInfos_},
        {METHOD_NAME("AMAPP_GetDLCTitleInfos"), AMAPP_GetDLCTitleInfos_},
        {METHOD_NAME("AMAPP_ListDataTitleTicketInfos"), AMAPP_ListDataTitleTicketInfos_},
        {METHOD_NAME("AMAPP_GetPatchTitleInfos"), AMAPP_GetPatchTitleInfos_},
        {METHOD_NAME("CFGU_GetConfigInfoBlk2"), CFGU_GetConfigInfoBlk2_},
        {METHOD_NAME("HIDUSER_EnableAccelerometer"), HIDUSER_EnableAccelerometer_},
        {METHOD_NAME("HIDUSER_DisableAccelerometer"), HIDUSER_DisableAccelerometer_},
        {METHOD_NAME("HIDUSER_EnableGyroscope"), HIDUSER_EnableGyroscope_},
        {METHOD_NAME("HIDUSER_DisableGyroscope"), HIDUSER_DisableGyroscope_},
        {METHOD_NAME("HIDUSER_GetGyroRawToDpsCoef"), HIDUSER_GetGyroscopeRawToDpsCoefficient_},
        {METHOD_NAME("HIDUSER_GetGyroCalibrateParam"), HIDUSER_GetGyroscopeCalibrateParam_},

        // UDP Streams
        {METHOD_NAME("#ArticController"), Controller_Start},
    };

    bool obtainExheader() {
        Result loaderInitCustom(void);
        void loaderExitCustom(void);
        Result LOADER_GetLastApplicationProgramInfo(ExHeader_Info* exheaderInfo);


        Result res = loaderInitCustom();
        if (R_SUCCEEDED(res)) res = LOADER_GetLastApplicationProgramInfo(&lastAppExheader);
        loaderExitCustom();

        if (R_FAILED(res)) {
            logger.Error("Failed to get ExHeader. Luma3DS may be outdated.");
            return false;
        }

        return true;
    }

    static bool closeHandles() {
        auto CloseHandle = [](u64 handle, HandleType type) {
            switch (type)
            {
            case HandleType::FILE:
                logger.Debug("Call pending FSFILE_Close");
                FSFILE_Close((Handle)handle);
                break;
            case HandleType::DIR:
                logger.Debug("Call pending FSDIR_Close");
                FSDIR_Close((Handle)handle);
                break;
            case HandleType::ARCHIVE:
                logger.Debug("Call pending FSUSER_CloseArchive");
                FSUSER_CloseArchive((FS_Archive)handle);
                break;
            default:
                break;
            }
        };
        for (auto it = openHandles.begin(); it != openHandles.end(); it++) {
            CloseHandle(it->first, it->second);
        }
        openHandles.clear();
        return true;
    }

    static bool stopController(void) {
        if (ArticController::thread_run) {
            logger.Debug("ArticController: Stopping...");
            ArticController::thread_run = false;
            if (ArticController::socket_fd != -1) {
                close(ArticController::socket_fd);
                ArticController::socket_fd = -1;
            }
            threadJoin(ArticController::thread, U64_MAX);
            threadFree(ArticController::thread);
            ArticController::thread = nullptr;
            logger.Debug("ArticController: Stopped...");
        }
        return true;
    }

    std::vector<bool(*)()> setupFunctions {
        obtainExheader,
    };

    std::vector<bool(*)()> destructFunctions {
        closeHandles,
        stopController,
    };

    Thread ArticController::thread = nullptr;
    bool ArticController::thread_run = false;
    int ArticController::socket_fd = -1;
    volatile bool ArticController::socket_ready = false;
}
