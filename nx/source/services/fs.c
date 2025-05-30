// Copyright 2017 plutoo
#include <string.h>
#include "service_guard.h"
#include "sf/sessionmgr.h"
#include "runtime/hosversion.h"
#include "services/fs.h"

__attribute__((weak)) u32 __nx_fs_num_sessions = 3;

static Service g_fsSrv;
static SessionMgr g_fsSessionMgr;

static __thread u32 g_fsPriority = FsPriority_Normal;

NX_INLINE bool _fsObjectIsChild(Service* s)
{
    return s->session == g_fsSrv.session;
}

static void _fsObjectClose(Service* s)
{
    if (!_fsObjectIsChild(s)) {
        serviceClose(s);
    }
    else {
        int slot = sessionmgrAttachClient(&g_fsSessionMgr);
        uint32_t object_id = serviceGetObjectId(s);
        serviceAssumeDomain(s);
        cmifMakeCloseRequest(armGetTls(), object_id);
        svcSendSyncRequest(sessionmgrGetClientSession(&g_fsSessionMgr, slot));
        sessionmgrDetachClient(&g_fsSessionMgr, slot);
    }
}

NX_INLINE Result _fsObjectDispatchImpl(
    Service* s, u32 request_id,
    const void* in_data, u32 in_data_size,
    void* out_data, u32 out_data_size,
    SfDispatchParams disp
) {
    int slot = -1;
    if (_fsObjectIsChild(s)) {
        slot = sessionmgrAttachClient(&g_fsSessionMgr);
        if (slot < 0) __builtin_unreachable();
        disp.target_session = sessionmgrGetClientSession(&g_fsSessionMgr, slot);
        serviceAssumeDomain(s);
    }

    disp.context = g_fsPriority;
    Result rc = serviceDispatchImpl(s, request_id, in_data, in_data_size, out_data, out_data_size, disp);

    if (slot >= 0) {
        sessionmgrDetachClient(&g_fsSessionMgr, slot);
    }

    return rc;
}

#define _fsObjectDispatch(_s,_rid,...) \
    _fsObjectDispatchImpl((_s),(_rid),NULL,0,NULL,0,(SfDispatchParams){ __VA_ARGS__ })

#define _fsObjectDispatchIn(_s,_rid,_in,...) \
    ({ _Static_assert(!(serviceMacroDetectIsPointer(_in))); \
    _fsObjectDispatchImpl((_s),(_rid),&(_in),sizeof(_in),NULL,0,(SfDispatchParams){ __VA_ARGS__ }); })

#define _fsObjectDispatchOut(_s,_rid,_out,...) \
    ({ _Static_assert(!(serviceMacroDetectIsPointer(_out))); \
    _fsObjectDispatchImpl((_s),(_rid),NULL,0,&(_out),sizeof(_out),(SfDispatchParams){ __VA_ARGS__ }); })

#define _fsObjectDispatchInOut(_s,_rid,_in,_out,...) \
    ({ _Static_assert(!(serviceMacroDetectIsPointer(_in))); \
    _Static_assert(!(serviceMacroDetectIsPointer(_out))); \
    _fsObjectDispatchImpl((_s),(_rid),&(_in),sizeof(_in),&(_out),sizeof(_out),(SfDispatchParams){ __VA_ARGS__ }); })

NX_GENERATE_SERVICE_GUARD(fs);

Result _fsInitialize(void) {
    Result rc = smGetService(&g_fsSrv, "fsp-srv");

    if (R_SUCCEEDED(rc)) {
        rc = serviceConvertToDomain(&g_fsSrv);
    }

    if (R_SUCCEEDED(rc)) {
        u64 pid_placeholder = 0;
        serviceAssumeDomain(&g_fsSrv);
        rc = serviceDispatchIn(&g_fsSrv, 1, pid_placeholder, .in_send_pid = true);
    }

    if (R_SUCCEEDED(rc))
        rc = sessionmgrCreate(&g_fsSessionMgr, g_fsSrv.session, __nx_fs_num_sessions);

    return rc;
}

void _fsCleanup(void) {
    // Close extra sessions
    sessionmgrClose(&g_fsSessionMgr);

    // We can't assume g_fsSrv is a domain here because serviceConvertToDomain might have failed
    serviceClose(&g_fsSrv);
}

Service* fsGetServiceSession(void) {
    return &g_fsSrv;
}

void fsSetPriority(FsPriority prio) {
    if (hosversionAtLeast(5,0,0))
        g_fsPriority = prio;
}

static Result _fsCmdGetSession(Service* srv, Service* srv_out, u32 cmd_id) {
    return _fsObjectDispatch(srv, cmd_id,
        .out_num_objects = 1,
        .out_objects = srv_out,
    );
}

static Result _fsCmdNoIO(Service* srv, u32 cmd_id) {
    return _fsObjectDispatch(srv, cmd_id);
}

static Result _fsCmdNoInOutU8(Service* srv, u8 *out, u32 cmd_id) {
    return _fsObjectDispatchOut(srv, cmd_id, *out);
}

static Result _fsCmdNoInOutU32(Service* srv, u32 *out, u32 cmd_id) {
    return _fsObjectDispatchOut(srv, cmd_id, *out);
}

static Result _fsCmdNoInOutS64(Service* srv, s64 *out, u32 cmd_id) {
    return _fsObjectDispatchOut(srv, cmd_id, *out);
}

static Result _fsCmdNoInOutBool(Service* srv, bool *out, u32 cmd_id) {
    u8 tmp=0;
    Result rc = _fsCmdNoInOutU8(srv, &tmp, cmd_id);
    if (R_SUCCEEDED(rc) && out) *out = tmp & 1;
    return rc;
}

static Result _fsCmdInSizeOutBuffer(Service* srv, void *dst, size_t dst_size, s64 size, u32 cmd_id) {
    return _fsObjectDispatchIn(srv, cmd_id, size,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { dst, dst_size } },
    );
}

//-----------------------------------------------------------------------------
// IFileSystemProxy
//-----------------------------------------------------------------------------

Result fsOpenFileSystem(FsFileSystem* out, FsFileSystemType fsType, const char* contentPath) {
    return fsOpenFileSystemWithId(out, 0, fsType, contentPath, FsContentAttributes_None);
}

static Result _fsOpenFileSystem(FsFileSystem* out, FsFileSystemType fsType, const char* contentPath) {
    u32 tmp=fsType;
    return _fsObjectDispatchIn(&g_fsSrv, 0, tmp,
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In },
        .buffers = { { contentPath, FS_MAX_PATH } },
        .out_num_objects = 1,
        .out_objects = &out->s,
    );
}

Result fsOpenDataFileSystemByCurrentProcess(FsFileSystem *out) {
    return _fsCmdGetSession(&g_fsSrv, &out->s, 2);
}

Result fsOpenFileSystemWithPatch(FsFileSystem* out, u64 id, FsFileSystemType fsType) {
    if (hosversionBefore(2,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    const struct {
        u32 fsType;
        u64 id;
    } in = { fsType, id };

    return _fsObjectDispatchIn(&g_fsSrv, 7, in,
        .out_num_objects = 1,
        .out_objects = &out->s,
    );
}

static Result _fsOpenFileSystemWithId(FsFileSystem* out, u64 id, FsFileSystemType fsType, const char* contentPath, FsContentAttributes attr) {
    if (hosversionAtLeast(16,0,0)) {
        const struct {
            u8 attr;
            u32 fsType;
            u64 id;
        } in = { attr, fsType, id };

        return _fsObjectDispatchIn(&g_fsSrv, 10, in,
            .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In },
            .buffers = { { contentPath, FS_MAX_PATH } },
            .out_num_objects = 1,
            .out_objects = &out->s,
        );
    } else {
        const struct {
            u32 fsType;
            u64 id;
        } in = { fsType, id };

        return _fsObjectDispatchIn(&g_fsSrv, 8, in,
            .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In },
            .buffers = { { contentPath, FS_MAX_PATH } },
            .out_num_objects = 1,
            .out_objects = &out->s,
        );
    }
}

Result fsOpenDataFileSystemByProgramId(FsFileSystem *out, u64 program_id) {
    if (hosversionBefore(3,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    return _fsObjectDispatchIn(&g_fsSrv, 9, program_id,
        .out_num_objects = 1,
        .out_objects     = &out->s,
    );
}

Result fsOpenFileSystemWithId(FsFileSystem* out, u64 id, FsFileSystemType fsType, const char* contentPath, FsContentAttributes attr) {
    char sendStr[FS_MAX_PATH] = {0};
    strncpy(sendStr, contentPath, sizeof(sendStr)-1);

    if (hosversionAtLeast(2,0,0))
        return _fsOpenFileSystemWithId(out, id, fsType, sendStr, attr);
    else
        return _fsOpenFileSystem(out, fsType, sendStr);
}

Result fsOpenBisFileSystem(FsFileSystem* out, FsBisPartitionId partitionId, const char* string) {
    char tmpstr[FS_MAX_PATH] = {0};
    strncpy(tmpstr, string, sizeof(tmpstr)-1);

    u32 tmp=partitionId;
    return _fsObjectDispatchIn(&g_fsSrv, 11, tmp,
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In },
        .buffers = { { tmpstr, sizeof(tmpstr) } },
        .out_num_objects = 1,
        .out_objects = &out->s,
    );
}

Result fsOpenBisStorage(FsStorage* out, FsBisPartitionId partitionId) {
    u32 tmp=partitionId;
    return _fsObjectDispatchIn(&g_fsSrv, 12, tmp,
        .out_num_objects = 1,
        .out_objects = &out->s,
    );
}

Result fsOpenSdCardFileSystem(FsFileSystem* out) {
    return _fsCmdGetSession(&g_fsSrv, &out->s, 18);
}

Result fsOpenHostFileSystem(FsFileSystem* out, const char *path) {
    char tmpstr[FS_MAX_PATH] = {0};
    strncpy(tmpstr, path, sizeof(tmpstr)-1);

    return _fsObjectDispatch(&g_fsSrv, 17,
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In },
        .buffers = { { tmpstr, sizeof(tmpstr) } },
        .out_num_objects = 1,
        .out_objects = &out->s,
    );
}

Result fsOpenHostFileSystemWithOption(FsFileSystem* out, const char *path, u32 flags) {
    if (hosversionBefore(9,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    char tmpstr[FS_MAX_PATH] = {0};
    strncpy(tmpstr, path, sizeof(tmpstr)-1);

    return _fsObjectDispatchIn(&g_fsSrv, 36, flags,
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In },
        .buffers = { { tmpstr, sizeof(tmpstr) } },
        .out_num_objects = 1,
        .out_objects = &out->s,
    );
}

Result fsDeleteSaveDataFileSystem(u64 application_id) {
    return _fsObjectDispatchIn(&g_fsSrv, 21, application_id);
}

Result fsCreateSaveDataFileSystem(const FsSaveDataAttribute* attr, const FsSaveDataCreationInfo* creation_info, const FsSaveDataMetaInfo* meta) {
    const struct {
        FsSaveDataAttribute attr;
        FsSaveDataCreationInfo creation_info;
        FsSaveDataMetaInfo meta;
    } in = { *attr, *creation_info, *meta };

    return _fsObjectDispatchIn(&g_fsSrv, 22, in);
}

Result fsCreateSaveDataFileSystemBySystemSaveDataId(const FsSaveDataAttribute* attr, const FsSaveDataCreationInfo* creation_info) {
    const struct {
        FsSaveDataAttribute attr;
        FsSaveDataCreationInfo creation_info;
    } in = { *attr, *creation_info };

    return _fsObjectDispatchIn(&g_fsSrv, 23, in);
}

Result fsDeleteSaveDataFileSystemBySaveDataSpaceId(FsSaveDataSpaceId save_data_space_id, u64 saveID) {
    if (hosversionBefore(2,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    const struct {
        u8 save_data_space_id;
        u64 saveID;
    } in = { (u8)save_data_space_id, saveID };

    return _fsObjectDispatchIn(&g_fsSrv, 25, in);
}

Result fsDeleteSaveDataFileSystemBySaveDataAttribute(FsSaveDataSpaceId save_data_space_id, const FsSaveDataAttribute* attr) {
    if (hosversionBefore(4,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    const struct {
        u8 save_data_space_id;
        FsSaveDataAttribute attr;
    } in = { (u8)save_data_space_id, *attr };

    return _fsObjectDispatchIn(&g_fsSrv, 28, in);
}

Result fsIsExFatSupported(bool* out) {
    if (hosversionBefore(2,0,0)) {
        *out = false;
        return 0;
    }

    return _fsCmdNoInOutBool(&g_fsSrv, out, 27);
}

Result fsOpenGameCardFileSystem(FsFileSystem* out, const FsGameCardHandle* handle, FsGameCardPartition partition) {
    const struct {
        FsGameCardHandle handle;
        u32 partition;
    } in = { *handle, partition };

    return _fsObjectDispatchIn(&g_fsSrv, 31, in,
        .out_num_objects = 1,
        .out_objects = &out->s,
    );
}

Result fsExtendSaveDataFileSystem(FsSaveDataSpaceId save_data_space_id, u64 saveID, s64 data_size, s64 journal_size) {
    if (hosversionBefore(3,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    const struct {
        u8 save_data_space_id;
        u8 pad[7];
        u64 saveID;
        s64 data_size;
        s64 journal_size;
    } in = { (u8)save_data_space_id, {0}, saveID, data_size, journal_size };

    return _fsObjectDispatchIn(&g_fsSrv, 32, in);
}

Result fsOpenSaveDataFileSystem(FsFileSystem* out, FsSaveDataSpaceId save_data_space_id, const FsSaveDataAttribute *attr) {
    const struct {
        u8 save_data_space_id;
        u8 pad[7];
        FsSaveDataAttribute attr;
    } in = { (u8)save_data_space_id, {0}, *attr };

    return _fsObjectDispatchIn(&g_fsSrv, 51, in,
        .out_num_objects = 1,
        .out_objects = &out->s,
    );
}

Result fsOpenSaveDataFileSystemBySystemSaveDataId(FsFileSystem* out, FsSaveDataSpaceId save_data_space_id, const FsSaveDataAttribute *attr) {
    const struct {
        u8 save_data_space_id;
        u8 pad[7];
        FsSaveDataAttribute attr;
    } in = { (u8)save_data_space_id, {0}, *attr };

    return _fsObjectDispatchIn(&g_fsSrv, 52, in,
        .out_num_objects = 1,
        .out_objects = &out->s,
    );
}

Result fsOpenReadOnlySaveDataFileSystem(FsFileSystem* out, FsSaveDataSpaceId save_data_space_id, const FsSaveDataAttribute *attr) {
    if (hosversionBefore(2,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    const struct {
        u8 save_data_space_id;
        u8 pad[7];
        FsSaveDataAttribute attr;
    } in = { (u8)save_data_space_id, {0}, *attr };

    return _fsObjectDispatchIn(&g_fsSrv, 53, in,
        .out_num_objects = 1,
        .out_objects = &out->s,
    );
}

Result fsReadSaveDataFileSystemExtraDataBySaveDataSpaceId(void* buf, size_t len, FsSaveDataSpaceId save_data_space_id, u64 saveID) {
    if (hosversionBefore(3,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    const struct {
        u8 save_data_space_id;
        u64 saveID;
    } in = { (u8)save_data_space_id, saveID };

    return _fsObjectDispatchIn(&g_fsSrv, 57, in,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { buf, len } },
    );
}

Result fsReadSaveDataFileSystemExtraData(void* buf, size_t len, u64 saveID) {
    return _fsObjectDispatchIn(&g_fsSrv, 58, saveID,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { buf, len } },
    );
}

Result fsWriteSaveDataFileSystemExtraData(const void* buf, size_t len, FsSaveDataSpaceId save_data_space_id, u64 saveID) {
    if (hosversionBefore(2,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    const struct {
        u8 save_data_space_id;
        u64 saveID;
    } in = { (u8)save_data_space_id, saveID };

    return _fsObjectDispatchIn(&g_fsSrv, 59, in,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { buf, len } },
    );
}

static Result _fsOpenSaveDataInfoReader(FsSaveDataInfoReader* out) {
    return _fsCmdGetSession(&g_fsSrv, &out->s, 60);
}

static Result _fsOpenSaveDataInfoReaderBySaveDataSpaceId(FsSaveDataInfoReader* out, FsSaveDataSpaceId save_data_space_id) {
    u8 in = (u8)save_data_space_id;
    return _fsObjectDispatchIn(&g_fsSrv, 61, in,
        .out_num_objects = 1,
        .out_objects = &out->s,
    );

}

Result fsOpenSaveDataInfoReader(FsSaveDataInfoReader* out, FsSaveDataSpaceId save_data_space_id) {
    if (save_data_space_id == FsSaveDataSpaceId_All) {
        return _fsOpenSaveDataInfoReader(out);
    } else {
        return _fsOpenSaveDataInfoReaderBySaveDataSpaceId(out, save_data_space_id);
    }
}

Result fsOpenSaveDataInfoReaderWithFilter(FsSaveDataInfoReader* out, FsSaveDataSpaceId save_data_space_id, const FsSaveDataFilter *save_data_filter) {
    const struct {
        u8 save_data_space_id;
        u8 pad[7];
        FsSaveDataFilter save_data_filter;
    } in = { (u8)save_data_space_id, {0}, *save_data_filter };

    return _fsObjectDispatchIn(&g_fsSrv, 68, in,
        .out_num_objects = 1,
        .out_objects = &out->s,
    );
}

Result fsOpenImageDirectoryFileSystem(FsFileSystem* out, FsImageDirectoryId image_directory_id) {
    u32 tmp=image_directory_id;
    return _fsObjectDispatchIn(&g_fsSrv, 100, tmp,
        .out_num_objects = 1,
        .out_objects = &out->s,
    );
}

Result fsOpenContentStorageFileSystem(FsFileSystem* out, FsContentStorageId content_storage_id) {
    u32 tmp=content_storage_id;
    return _fsObjectDispatchIn(&g_fsSrv, 110, tmp,
        .out_num_objects = 1,
        .out_objects = &out->s,
    );
}

Result fsOpenCustomStorageFileSystem(FsFileSystem* out, FsCustomStorageId custom_storage_id) {
    if (hosversionBefore(7,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    u32 tmp=custom_storage_id;
    return _fsObjectDispatchIn(&g_fsSrv, 130, tmp,
        .out_num_objects = 1,
        .out_objects = &out->s,
    );
}

Result fsOpenDataStorageByCurrentProcess(FsStorage* out) {
    return _fsCmdGetSession(&g_fsSrv, &out->s, 200);
}

Result fsOpenDataStorageByProgramId(FsStorage *out, u64 program_id) {
    if (hosversionBefore(3,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    return _fsObjectDispatchIn(&g_fsSrv, 201, program_id,
        .out_num_objects = 1,
        .out_objects = &out->s,
    );
}

Result fsOpenDataStorageByDataId(FsStorage* out, u64 dataId, NcmStorageId storageId) {
    const struct {
        u8 storage_id;
        u64 data_id;
    } in = { storageId, dataId };

    return _fsObjectDispatchIn(&g_fsSrv, 202, in,
        .out_num_objects = 1,
        .out_objects = &out->s,
    );
}

Result fsOpenPatchDataStorageByCurrentProcess(FsStorage* out) {
    return _fsCmdGetSession(&g_fsSrv, &out->s, 203);
}

Result fsOpenDeviceOperator(FsDeviceOperator* out) {
    return _fsCmdGetSession(&g_fsSrv, &out->s, 400);
}

Result fsOpenSdCardDetectionEventNotifier(FsEventNotifier* out) {
    return _fsCmdGetSession(&g_fsSrv, &out->s, 500);
}

Result fsIsSignedSystemPartitionOnSdCardValid(bool *out) {
    if (!hosversionBetween(4, 8))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    return _fsCmdNoInOutBool(&g_fsSrv, out, 640);
}

Result fsGetProgramId(u64* out, const char *path, FsContentAttributes attr) {
    if (hosversionBefore(17,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    char send_path[FS_MAX_PATH] = {0};
    strncpy(send_path, path, FS_MAX_PATH-1);

    const u8 in = attr;
    return _fsObjectDispatchInOut(&g_fsSrv, 618, in, *out,
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In },
        .buffers = { { send_path, sizeof(send_path) } },
    );
}

Result fsGetRightsIdByPath(const char* path, FsRightsId* out_rights_id) {
    if (!hosversionBetween(2, 16))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    char send_path[FS_MAX_PATH] = {0};
    strncpy(send_path, path, FS_MAX_PATH-1);

    return _fsObjectDispatchOut(&g_fsSrv, 609, *out_rights_id,
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In },
        .buffers = { { send_path, sizeof(send_path) } },
    );
}

Result fsGetRightsIdAndKeyGenerationByPath(const char* path, FsContentAttributes attr, u8* out_key_generation, FsRightsId* out_rights_id) {
    if (hosversionBefore(3,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    char send_path[FS_MAX_PATH] = {0};
    strncpy(send_path, path, FS_MAX_PATH-1);

    const u8 in = attr;

    struct {
        u8 key_generation;
        u8 padding[0x7];
        FsRightsId rights_id;
    } out;

    Result rc;
    if (hosversionAtLeast(16,0,0)) {
        rc = _fsObjectDispatchInOut(&g_fsSrv, 610, in, out,
            .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In },
            .buffers = { { send_path, sizeof(send_path) } },
        );
    } else {
        rc = _fsObjectDispatchOut(&g_fsSrv, 610, out,
            .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In },
            .buffers = { { send_path, sizeof(send_path) } },
        );
    }

    if (R_SUCCEEDED(rc)) {
        if (out_key_generation) *out_key_generation = out.key_generation;
        if (out_rights_id) *out_rights_id = out.rights_id;
    }

    return rc;
}

Result fsGetAndClearErrorInfo(FsFileSystemProxyErrorInfo *out) {
    if (hosversionBefore(2,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    return _fsObjectDispatchOut(&g_fsSrv, 800, *out);
}

Result fsGetContentStorageInfoIndex(s32 *out) {
    if (hosversionBefore(19,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    return _fsCmdNoInOutU32(&g_fsSrv, (u32 *)out, 820);
}

Result fsDisableAutoSaveDataCreation(void) {
    return _fsCmdNoIO(&g_fsSrv, 1003);
}

Result fsSetGlobalAccessLogMode(u32 mode) {
    return _fsObjectDispatchIn(&g_fsSrv, 1004, mode);
}

Result fsGetGlobalAccessLogMode(u32* out_mode) {
    return _fsObjectDispatchOut(&g_fsSrv, 1005, *out_mode);
}

Result fsOutputAccessLogToSdCard(const char *log, size_t size) {
    return _fsObjectDispatch(&g_fsSrv, 1006,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { log, size } },
    );
}

Result fsGetProgramIndexForAccessLog(u32 *out_program_index, u32 *out_program_count) {
    if (hosversionBefore(7,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    struct {
        u32 index;
        u32 count;
    } out;

    Result rc = _fsObjectDispatchOut(&g_fsSrv, 1007, out);
    if (R_SUCCEEDED(rc)) {
        if (out_program_index) *out_program_index = out.index;
        if (out_program_count) *out_program_count = out.count;
    }
    return rc;
}

Result fsGetAndClearMemoryReportInfo(FsMemoryReportInfo* out) {
    if (hosversionBefore(4,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    return _fsObjectDispatchOut(&g_fsSrv, 1009, *out);
}

// Wrapper(s) for fsCreateSaveDataFileSystem.
Result fsCreate_TemporaryStorage(u64 application_id, u64 owner_id, s64 size, u32 flags) {
    FsSaveDataAttribute attr = {
        .application_id = application_id,
        .save_data_type = FsSaveDataType_Temporary,
    };
    FsSaveDataCreationInfo create = {
        .save_data_size = size,
        .journal_size = 0,
        .available_size = 0x4000,
        .owner_id = owner_id,
        .flags = flags,
        .save_data_space_id = FsSaveDataSpaceId_Temporary,
    };
    FsSaveDataMetaInfo meta={};

    return fsCreateSaveDataFileSystem(&attr, &create, &meta);
}

// Wrapper(s) for fsCreateSaveDataFileSystemBySystemSaveDataId.
Result fsCreate_SystemSaveDataWithOwner(FsSaveDataSpaceId save_data_space_id, u64 system_save_data_id, AccountUid uid, u64 owner_id, s64 size, s64 journal_size, u32 flags) {
    FsSaveDataAttribute attr = {
        .uid = uid,
        .system_save_data_id = system_save_data_id,
    };
    FsSaveDataCreationInfo create = {
        .save_data_size = size,
        .journal_size = journal_size,
        .available_size = 0x4000,
        .owner_id = owner_id,
        .flags = flags,
        .save_data_space_id = save_data_space_id,
    };

    return fsCreateSaveDataFileSystemBySystemSaveDataId(&attr, &create);
}

Result fsCreate_SystemSaveData(FsSaveDataSpaceId save_data_space_id, u64 system_save_data_id, s64 size, s64 journal_size, u32 flags) {
    return fsCreate_SystemSaveDataWithOwner(save_data_space_id, system_save_data_id, (AccountUid){}, 0, size, journal_size, flags);
}

// Wrapper(s) for fsOpenSaveDataFileSystem.
Result fsOpen_SaveData(FsFileSystem* out, u64 application_id, AccountUid uid) {
    FsSaveDataAttribute attr;

    memset(&attr, 0, sizeof(attr));
    attr.application_id = application_id;
    attr.uid = uid;
    attr.save_data_type = FsSaveDataType_Account;

    return fsOpenSaveDataFileSystem(out, FsSaveDataSpaceId_User, &attr);
}

Result fsOpen_SaveDataReadOnly(FsFileSystem* out, u64 application_id, AccountUid uid) {
    FsSaveDataAttribute attr;

    memset(&attr, 0, sizeof(attr));
    attr.application_id = application_id;
    attr.uid = uid;
    attr.save_data_type = FsSaveDataType_Account;

    return fsOpenReadOnlySaveDataFileSystem(out, FsSaveDataSpaceId_User, &attr);
}

Result fsOpen_BcatSaveData(FsFileSystem* out, u64 application_id) {
    FsSaveDataAttribute attr;

    memset(&attr, 0, sizeof(attr));
    attr.application_id = application_id;
    attr.save_data_type = FsSaveDataType_Bcat;

    return fsOpenSaveDataFileSystem(out, FsSaveDataSpaceId_User, &attr);
}

Result fsOpen_DeviceSaveData(FsFileSystem* out, u64 application_id) {
    FsSaveDataAttribute attr;

    memset(&attr, 0, sizeof(attr));
    attr.application_id = application_id;
    attr.save_data_type = FsSaveDataType_Device;

    return fsOpenSaveDataFileSystem(out, FsSaveDataSpaceId_User, &attr);
}

Result fsOpen_TemporaryStorage(FsFileSystem* out) {
    if (hosversionBefore(3,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    FsSaveDataAttribute attr;

    memset(&attr, 0, sizeof(attr));
    attr.save_data_type = FsSaveDataType_Temporary;

    return fsOpenSaveDataFileSystem(out, FsSaveDataSpaceId_Temporary, &attr);
}

Result fsOpen_CacheStorage(FsFileSystem* out, u64 application_id, u16 save_data_index) {
    if (hosversionBefore(3,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    FsSaveDataAttribute attr;

    memset(&attr, 0, sizeof(attr));
    attr.application_id = application_id;
    attr.save_data_type = FsSaveDataType_Cache;
    attr.save_data_index = save_data_index;

    return fsOpenSaveDataFileSystem(out, FsSaveDataSpaceId_User, &attr);
}

Result fsOpen_SystemSaveData(FsFileSystem* out, FsSaveDataSpaceId save_data_space_id, u64 system_save_data_id, AccountUid uid) {
    FsSaveDataAttribute attr;

    memset(&attr, 0, sizeof(attr));
    attr.uid = uid;
    attr.system_save_data_id = system_save_data_id;
    attr.save_data_type = FsSaveDataType_System;

    return fsOpenSaveDataFileSystemBySystemSaveDataId(out, save_data_space_id, &attr);
}

Result fsOpen_SystemBcatSaveData(FsFileSystem* out, u64 system_save_data_id) {
    if (hosversionBefore(4,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    FsSaveDataAttribute attr;

    memset(&attr, 0, sizeof(attr));
    attr.system_save_data_id = system_save_data_id;
    attr.save_data_type = FsSaveDataType_SystemBcat;

    return fsOpenSaveDataFileSystemBySystemSaveDataId(out, FsSaveDataSpaceId_System, &attr);
}

//-----------------------------------------------------------------------------
// IFileSystem
//-----------------------------------------------------------------------------

Result fsFsCreateFile(FsFileSystem* fs, const char* path, s64 size, u32 option) {
    const struct {
        u32 option;
        u64 size;
    } in = { option, size };

    return _fsObjectDispatchIn(&fs->s, 0, in,
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In },
        .buffers = { { path, FS_MAX_PATH } },
    );
}

static Result _fsFsCmdWithInPath(FsFileSystem* fs, const char* path, u32 cmd_id) {
    return _fsObjectDispatch(&fs->s, cmd_id,
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In },
        .buffers = { { path, FS_MAX_PATH } },
    );
}

Result fsFsDeleteFile(FsFileSystem* fs, const char* path) {
    return _fsFsCmdWithInPath(fs, path, 1);
}

Result fsFsCreateDirectory(FsFileSystem* fs, const char* path) {
    return _fsFsCmdWithInPath(fs, path, 2);
}

Result fsFsDeleteDirectory(FsFileSystem* fs, const char* path) {
    return _fsFsCmdWithInPath(fs, path, 3);
}

Result fsFsDeleteDirectoryRecursively(FsFileSystem* fs, const char* path) {
    return _fsFsCmdWithInPath(fs, path, 4);
}

static Result _fsFsCmdWithTwoInPaths(FsFileSystem* fs, const char* cur_path, const char* new_path, u32 cmd_id) {
    return _fsObjectDispatch(&fs->s, cmd_id,
        .buffer_attrs = {
            SfBufferAttr_HipcPointer | SfBufferAttr_In,
            SfBufferAttr_HipcPointer | SfBufferAttr_In,
        },
        .buffers = {
            { cur_path, FS_MAX_PATH },
            { new_path, FS_MAX_PATH },
        },
    );
}

Result fsFsRenameFile(FsFileSystem* fs, const char* cur_path, const char* new_path) {
    return _fsFsCmdWithTwoInPaths(fs, cur_path, new_path, 5);
}

Result fsFsRenameDirectory(FsFileSystem* fs, const char* cur_path, const char* new_path) {
    return _fsFsCmdWithTwoInPaths(fs, cur_path, new_path, 6);
}

Result fsFsGetEntryType(FsFileSystem* fs, const char* path, FsDirEntryType* out) {
    return _fsObjectDispatchOut(&fs->s, 7, *out,
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In },
        .buffers = { { path, FS_MAX_PATH } },
    );
}

static Result _fsFsOpenCommon(FsFileSystem* fs, const char* path, u32 flags, Service* out, u32 cmd_id) {
    return _fsObjectDispatchIn(&fs->s, cmd_id, flags,
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In },
        .buffers = { { path, FS_MAX_PATH } },
        .out_num_objects = 1,
        .out_objects = out,
    );
}

Result fsFsOpenFile(FsFileSystem* fs, const char* path, u32 mode, FsFile* out) {
    return _fsFsOpenCommon(fs, path, mode, &out->s, 8);
}

Result fsFsOpenDirectory(FsFileSystem* fs, const char* path, u32 mode, FsDir* out) {
    return _fsFsOpenCommon(fs, path, mode, &out->s, 9);
}

Result fsFsCommit(FsFileSystem* fs) {
    return _fsCmdNoIO(&fs->s, 10);
}

static Result _fsFsCmdWithInPathAndOutU64(FsFileSystem* fs, const char* path, u64* out, u32 cmd_id) {
    return _fsObjectDispatchOut(&fs->s, cmd_id, *out,
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In },
        .buffers = { { path, FS_MAX_PATH } },
    );
}

Result fsFsGetFreeSpace(FsFileSystem* fs, const char* path, s64* out) {
    return _fsFsCmdWithInPathAndOutU64(fs, path, (u64*)out, 11);
}

Result fsFsGetTotalSpace(FsFileSystem* fs, const char* path, s64* out) {
    return _fsFsCmdWithInPathAndOutU64(fs, path, (u64*)out, 12);
}

Result fsFsCleanDirectoryRecursively(FsFileSystem* fs, const char* path) {
    if (hosversionBefore(3,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    return _fsFsCmdWithInPath(fs, path, 13);
}

Result fsFsGetFileTimeStampRaw(FsFileSystem* fs, const char* path, FsTimeStampRaw *out) {
    if (hosversionBefore(3,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    return _fsObjectDispatchOut(&fs->s, 14, *out,
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In },
        .buffers = { { path, FS_MAX_PATH } },
    );
}

Result fsFsQueryEntry(FsFileSystem* fs, void *out, size_t out_size, const void *in, size_t in_size, const char* path, FsFileSystemQueryId query_id) {
    if (hosversionBefore(4,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    return _fsObjectDispatchIn(&fs->s, 15, query_id,
        .buffer_attrs = {
            SfBufferAttr_HipcPointer  | SfBufferAttr_In,
            SfBufferAttr_HipcMapAlias | SfBufferAttr_In  | SfBufferAttr_HipcMapTransferAllowsNonSecure,
            SfBufferAttr_HipcMapAlias | SfBufferAttr_Out | SfBufferAttr_HipcMapTransferAllowsNonSecure,
        },
        .buffers = {
            { path, FS_MAX_PATH },
            { in,   in_size     },
            { out,  out_size    },
        },
    );
}

Result fsFsGetFileSystemAttribute(FsFileSystem* fs, FsFileSystemAttribute *out) {
    if (hosversionBefore(15,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    return _fsObjectDispatchOut(&fs->s, 16, *out);
}

Result fsFsSetConcatenationFileAttribute(FsFileSystem* fs, const char *path) {
    return fsFsQueryEntry(fs, NULL, 0, NULL, 0, path, FsFileSystemQueryId_SetConcatenationFileAttribute);
}

Result fsFsIsValidSignedSystemPartitionOnSdCard(FsFileSystem* fs, bool *out) {
    if (hosversionBefore(8,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    u8 tmp=0;
    char send_path[FS_MAX_PATH] = "/";
    Result rc = fsFsQueryEntry(fs, &tmp, sizeof(tmp), NULL, 0, send_path, FsFileSystemQueryId_IsValidSignedSystemPartitionOnSdCard);
    if (R_SUCCEEDED(rc) && out) *out = tmp & 1;
    return rc;
}

void fsFsClose(FsFileSystem* fs) {
    _fsObjectClose(&fs->s);
}

//-----------------------------------------------------------------------------
// IFile
//-----------------------------------------------------------------------------

Result fsFileRead(FsFile* f, s64 off, void* buf, u64 read_size, u32 option, u64* bytes_read) {
    const struct {
        u32 option;
        u32 pad;
        s64 offset;
        u64 read_size;
    } in = { option, 0, off, read_size };

    return _fsObjectDispatchInOut(&f->s, 0, in, *bytes_read,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out | SfBufferAttr_HipcMapTransferAllowsNonSecure },
        .buffers = { { buf, read_size } },
    );
}

Result fsFileWrite(FsFile* f, s64 off, const void* buf, u64 write_size, u32 option) {
    const struct {
        u32 option;
        u32 pad;
        s64 offset;
        u64 write_size;
    } in = { option, 0, off, write_size };

    return _fsObjectDispatchIn(&f->s, 1, in,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In | SfBufferAttr_HipcMapTransferAllowsNonSecure },
        .buffers = { { buf, write_size } },
    );
}

Result fsFileFlush(FsFile* f) {
    return _fsCmdNoIO(&f->s, 2);
}

Result fsFileSetSize(FsFile* f, s64 sz) {
    return _fsObjectDispatchIn(&f->s, 3, sz);
}

Result fsFileGetSize(FsFile* f, s64* out) {
    return _fsObjectDispatchOut(&f->s, 4, *out);
}

Result fsFileOperateRange(FsFile* f, FsOperationId op_id, s64 off, s64 len, FsRangeInfo* out) {
    if (hosversionBefore(4,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    const struct {
        u32 op_id;
        u32 pad;
        s64 off;
        s64 len;
    } in = { op_id, 0, off, len };

    return _fsObjectDispatchInOut(&f->s, 5, in, *out);
}

void fsFileClose(FsFile* f) {
    _fsObjectClose(&f->s);
}

// IDirectory implementation
void fsDirClose(FsDir* d) {
    _fsObjectClose(&d->s);
}

Result fsDirRead(FsDir* d, s64* total_entries, size_t max_entries, FsDirectoryEntry *buf) {
    return _fsObjectDispatchOut(&d->s, 0, *total_entries,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { buf, max_entries*sizeof(FsDirectoryEntry) } },
    );
}

Result fsDirGetEntryCount(FsDir* d, s64* count) {
    return _fsObjectDispatchOut(&d->s, 1, *count);
}

//-----------------------------------------------------------------------------
// IStorage
//-----------------------------------------------------------------------------

Result fsStorageRead(FsStorage* s, s64 off, void* buf, u64 read_size) {
    const struct {
        s64 offset;
        u64 read_size;
    } in = { off, read_size };

    return _fsObjectDispatchIn(&s->s, 0, in,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out | SfBufferAttr_HipcMapTransferAllowsNonSecure },
        .buffers = { { buf, read_size } },
    );
}

Result fsStorageWrite(FsStorage* s, s64 off, const void* buf, u64 write_size) {
    const struct {
        s64 offset;
        u64 write_size;
    } in = { off, write_size };

    return _fsObjectDispatchIn(&s->s, 1, in,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In | SfBufferAttr_HipcMapTransferAllowsNonSecure },
        .buffers = { { buf, write_size } },
    );
}

Result fsStorageFlush(FsStorage* s) {
    return _fsCmdNoIO(&s->s, 2);
}

Result fsStorageSetSize(FsStorage* s, s64 sz) {
    return _fsObjectDispatchIn(&s->s, 3, sz);
}

Result fsStorageGetSize(FsStorage* s, s64* out) {
    return _fsObjectDispatchOut(&s->s, 4, *out);
}

Result fsStorageOperateRange(FsStorage* s, FsOperationId op_id, s64 off, s64 len, FsRangeInfo* out) {
    if (hosversionBefore(4,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    const struct {
        u32 op_id;
        u32 pad;
        s64 off;
        s64 len;
    } in = { op_id, 0, off, len };

    return _fsObjectDispatchInOut(&s->s, 5, in, *out);
}

void fsStorageClose(FsStorage* s) {
    _fsObjectClose(&s->s);
}

//-----------------------------------------------------------------------------
// ISaveDataInfoReader
//-----------------------------------------------------------------------------

// Actually called ReadSaveDataInfo
Result fsSaveDataInfoReaderRead(FsSaveDataInfoReader *s, FsSaveDataInfo* buf, size_t max_entries, s64* total_entries) {
    return _fsObjectDispatchOut(&s->s, 0, *total_entries,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { buf, max_entries*sizeof(FsSaveDataInfo) } },
    );
}

void fsSaveDataInfoReaderClose(FsSaveDataInfoReader* s) {
    _fsObjectClose(&s->s);
}

//-----------------------------------------------------------------------------
// IEventNotifier
//-----------------------------------------------------------------------------

Result fsEventNotifierGetEventHandle(FsEventNotifier* e, Event* out, bool autoclear) {
    Handle event = INVALID_HANDLE;
    Result rc = _fsObjectDispatch(&e->s, 0,
        .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
        .out_handles = &event,
    );

    if (R_SUCCEEDED(rc))
        eventLoadRemote(out, event, autoclear);

    return rc;
}

void fsEventNotifierClose(FsEventNotifier* e) {
    _fsObjectClose(&e->s);
}

//-----------------------------------------------------------------------------
// IDeviceOperator
//-----------------------------------------------------------------------------

static Result _fsCmdGetAndClearStorageErrorInfo(Service *srv, FsStorageErrorInfo* out_sei, s64 *out_log_size, void *dst, size_t dst_size, s64 size, u32 cmd_id) {
    struct {
        FsStorageErrorInfo error_info;
        s64 log_size;
    } out;

    Result rc = _fsObjectDispatchInOut(srv, cmd_id, size, out,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { dst, dst_size } },
    );

    if (R_SUCCEEDED(rc)) {
        *out_sei = out.error_info;
        *out_log_size = out.log_size;
    }

    return rc;
}

Result fsDeviceOperatorIsSdCardInserted(FsDeviceOperator* d, bool* out) {
    return _fsCmdNoInOutBool(&d->s, out, 0);
}

Result fsDeviceOperatorGetSdCardSpeedMode(FsDeviceOperator* d, s64* out) {
    return _fsCmdNoInOutS64(&d->s, out, 1);
}

Result fsDeviceOperatorGetSdCardCid(FsDeviceOperator* d, void* dst, size_t dst_size, s64 size) {
    if (hosversionBefore(2,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    return _fsCmdInSizeOutBuffer(&d->s, dst, dst_size, size, 2);
}

Result fsDeviceOperatorGetSdCardUserAreaSize(FsDeviceOperator* d, s64* out) {
    if (hosversionBefore(2,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    return _fsCmdNoInOutS64(&d->s, out, 3);
}

Result fsDeviceOperatorGetSdCardProtectedAreaSize(FsDeviceOperator* d, s64* out) {
    if (hosversionBefore(2,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    return _fsCmdNoInOutS64(&d->s, out, 4);
}

Result fsDeviceOperatorGetAndClearSdCardErrorInfo(FsDeviceOperator* d, FsStorageErrorInfo* out, s64 *out_log_size, void *dst, size_t dst_size, s64 size) {
    if (hosversionBefore(2,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    return _fsCmdGetAndClearStorageErrorInfo(&d->s, out, out_log_size, dst, dst_size, size, 5);
}

Result fsDeviceOperatorGetMmcCid(FsDeviceOperator* d, void* dst, size_t dst_size, s64 size) {
    return _fsCmdInSizeOutBuffer(&d->s, dst, dst_size, size, 100);
}

Result fsDeviceOperatorGetMmcSpeedMode(FsDeviceOperator* d, s64* out) {
    return _fsCmdNoInOutS64(&d->s, out, 101);
}

Result fsDeviceOperatorGetMmcPatrolCount(FsDeviceOperator* d, u32* out) {
    if (hosversionBefore(2,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    return _fsCmdNoInOutU32(&d->s, out, 112);
}

Result fsDeviceOperatorGetAndClearMmcErrorInfo(FsDeviceOperator* d, FsStorageErrorInfo* out, s64 *out_log_size, void *dst, size_t dst_size, s64 size) {
    if (hosversionBefore(2,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    return _fsCmdGetAndClearStorageErrorInfo(&d->s, out, out_log_size, dst, dst_size, size, 113);
}

Result fsDeviceOperatorGetMmcExtendedCsd(FsDeviceOperator* d, void* dst, size_t dst_size, s64 size) {
    if (hosversionBefore(2,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    return _fsCmdInSizeOutBuffer(&d->s, dst, dst_size, size, 114);
}

Result fsDeviceOperatorIsGameCardInserted(FsDeviceOperator* d, bool* out) {
    return _fsCmdNoInOutBool(&d->s, out, 200);
}

Result fsDeviceOperatorGetGameCardHandle(FsDeviceOperator* d, FsGameCardHandle* out) {
    return _fsObjectDispatchOut(&d->s, 202, *out);
}

Result fsDeviceOperatorGetGameCardUpdatePartitionInfo(FsDeviceOperator* d, const FsGameCardHandle* handle, FsGameCardUpdatePartitionInfo* out) {
    return _fsObjectDispatchInOut(&d->s, 203, *handle, *out);
}

Result fsDeviceOperatorGetGameCardAttribute(FsDeviceOperator* d, const FsGameCardHandle* handle, u8 *out) {
    return _fsObjectDispatchInOut(&d->s, 205, *handle, *out);
}

Result fsDeviceOperatorGetGameCardDeviceCertificate(FsDeviceOperator* d, const FsGameCardHandle* handle, void* dst, size_t dst_size, s64* out_size, s64 size) {
    const struct {
        FsGameCardHandle handle;
        s64 buffer_size;
    } in = { *handle, size };

    // Assume old gamecard certificate size on pre-19.0.0
    s64 os = 0x200;
    Result rc;

    if (hosversionAtLeast(19,0,0)) {
        rc = _fsObjectDispatchInOut(&d->s, 206, in, os,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { dst, dst_size } });
    } else {
        rc = _fsObjectDispatchIn(&d->s, 206, in,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { dst, dst_size } });
    }

    if (R_SUCCEEDED(rc))
        *out_size = os;

    return rc;
}

Result fsDeviceOperatorGetGameCardIdSet(FsDeviceOperator* d, void* dst, size_t dst_size, s64 size) {
    return _fsCmdInSizeOutBuffer(&d->s, dst, dst_size, size, 208);
}

Result fsDeviceOperatorGetGameCardErrorReportInfo(FsDeviceOperator* d, FsGameCardErrorReportInfo* out) {
    if (hosversionBefore(2,1,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    return _fsObjectDispatchOut(&d->s, 217, *out);
}

Result fsDeviceOperatorGetGameCardDeviceId(FsDeviceOperator* d, void* dst, size_t dst_size, s64 size) {
    if (hosversionBefore(3,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    return _fsCmdInSizeOutBuffer(&d->s, dst, dst_size, size, 218);
}

Result fsDeviceOperatorChallengeCardExistence(FsDeviceOperator* d, const FsGameCardHandle* handle, void* dst, size_t dst_size, void* seed, size_t seed_size, void* value, size_t value_size) {
    if (hosversionBefore(8,0,0))
        return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    return _fsObjectDispatchIn(&d->s, 219, *handle,
        .buffer_attrs = {
            SfBufferAttr_HipcMapAlias | SfBufferAttr_Out,
            SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
            SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
        },
        .buffers = {
            { dst,   dst_size   },
            { seed,  seed_size  },
            { value, value_size },
        },
    );
}

void fsDeviceOperatorClose(FsDeviceOperator* d) {
    _fsObjectClose(&d->s);
}
