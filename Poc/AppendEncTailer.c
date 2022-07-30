﻿
#include "appendenctailer.h"
#include "manualencryptdecrypt.h"
#include "filefuncs.h"

#define POC_ENC_TAILER ('encT')

volatile BOOLEAN g_append_enc_tailer_enabled = FALSE;

PKMUTEX lg_mutex = NULL; //用于指示线程是否退出了，如果线程退出将会自动释放该mutex

LIST_ENTRY lg_append_enc_tailer_list_head = {0}; // 用于遍历stream_context，执行append_enc_tailer

PKSPIN_LOCK lg_add_stream_context_lock = NULL;    // 用于保护lg_append_enc_tailer_list_head
LIST_ENTRY lg_add_stream_context_list_head = {0}; // 用于快速插入stream_context

typedef struct _POC_ENC_TAILER_LIST
{
    LIST_ENTRY list_entry;
    PPOC_STREAM_CONTEXT stream_context;
} POC_ENC_TAILER_LIST, *PPOC_ENC_TAILER_LIST;

NTSTATUS PocAppendEncTailerToFile(
    IN PFLT_VOLUME Volume,
    IN PFLT_INSTANCE Instance,
    IN PPOC_STREAM_CONTEXT StreamContext)
{
    if (NULL == StreamContext)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->StreamContext is NULL.\n", __FUNCTION__));
        return STATUS_INVALID_PARAMETER;
    }

    if (NULL == StreamContext->FileName)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->StreamContext->FileName is NULL.\n", __FUNCTION__));
        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    PPOC_VOLUME_CONTEXT VolumeContext = NULL;

    UNICODE_STRING uFileName = {0};
    OBJECT_ATTRIBUTES ObjectAttributes = {0};

    HANDLE hFile = NULL;
    PFILE_OBJECT FileObject = NULL;
    IO_STATUS_BLOCK IoStatusBlock = {0};

    LONGLONG FileSize = 0;
    LARGE_INTEGER ByteOffset = {0};
    ULONG WriteLength = 0;
    PCHAR WriteBuffer = NULL;
    ULONG BytesWritten = 0;

    Status = FltGetVolumeContext(gFilterHandle, Volume, &VolumeContext);

    if (!NT_SUCCESS(Status) || 0 == VolumeContext->SectorSize)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s@%d FltGetVolumeContext failed. Status = 0x%x.\n", __FUNCTION__, __LINE__, Status));
        goto EXIT;
    }

    RtlInitUnicodeString(&uFileName, StreamContext->FileName);

    InitializeObjectAttributes(&ObjectAttributes, &uFileName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

    Status = FltCreateFileEx(
        gFilterHandle,
        Instance,
        &hFile,
        &FileObject,
        GENERIC_WRITE,
        &ObjectAttributes,
        &IoStatusBlock,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        // FILE_SHARE_READ,
        0,
        FILE_OPEN,
        FILE_NON_DIRECTORY_FILE | FILE_SEQUENTIAL_ONLY | FILE_NO_INTERMEDIATE_BUFFERING,
        NULL,
        0,
        IO_FORCE_ACCESS_CHECK);

    if (STATUS_SUCCESS != Status)
    {
        if (STATUS_OBJECT_NAME_NOT_FOUND == Status ||
            STATUS_OBJECT_PATH_SYNTAX_BAD == Status)
        {
            PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s@%d FltCreateFileEx Status = 0x%x. Success->It means that the file has been deleted.\n", __FUNCTION__, __LINE__, Status));
        }
        else
        {
            PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s@%d FltCreateFileEx failed. Status = 0x%x.\n", __FUNCTION__, __LINE__, Status));
        }
        goto EXIT;
    }

    /*PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("\n %s->Process = %p Thread = %p Fcb = %p Ccb = %p Resource = %p PagingIoResource = %p.\n\n",
        __FUNCTION__,
        PsGetCurrentProcess(),
        PsGetCurrentThread(),
        (PCHAR)FileObject->FsContext,
        FileObject->FsContext2,
        ((PFSRTL_ADVANCED_FCB_HEADER)FileObject->FsContext)->Resource,
        ((PFSRTL_ADVANCED_FCB_HEADER)FileObject->FsContext)->PagingIoResource));*/

    FileSize = StreamContext->FileSize;

    WriteLength = ROUND_TO_SIZE(PAGE_SIZE, VolumeContext->SectorSize);

    WriteBuffer = FltAllocatePoolAlignedWithTag(Instance, NonPagedPool, WriteLength, WRITE_BUFFER_TAG);

    if (NULL == WriteBuffer)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s@%d FltAllocatePoolAlignedWithTag WriteBuffer failed.\n", __FUNCTION__, __LINE__));
        Status = STATUS_UNSUCCESSFUL;
        goto EXIT;
    }

    // RtlZeroMemory(WriteBuffer, WriteLength);

    ByteOffset.QuadPart = ROUND_TO_SIZE(FileSize, VolumeContext->SectorSize);

    RtlMoveMemory(WriteBuffer, &EncryptionTailer, sizeof(POC_ENCRYPTION_TAILER));

    ((PPOC_ENCRYPTION_TAILER)WriteBuffer)->FileSize = StreamContext->FileSize;

    ((PPOC_ENCRYPTION_TAILER)WriteBuffer)->IsCipherText = StreamContext->IsCipherText;
    RtlMoveMemory(((PPOC_ENCRYPTION_TAILER)WriteBuffer)->FileName, StreamContext->FileName, wcslen(StreamContext->FileName) * sizeof(WCHAR));
    for (int i = 0; i < AES_BLOCK_SIZE; i++)
    {
        ((PPOC_ENCRYPTION_TAILER)WriteBuffer)->CipherText[i] = StreamContext->cipher_buffer[i];
    }

    Status = FltWriteFileEx(
        Instance,
        FileObject,
        &ByteOffset,
        WriteLength,
        WriteBuffer,
        FLTFL_IO_OPERATION_NON_CACHED |
            FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET,
        &BytesWritten,
        NULL,
        NULL,
        NULL,
        NULL);

    if (!NT_SUCCESS(Status))
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s@%d FltWriteFileEx failed. Status = 0x%x.\n", __FUNCTION__, __LINE__, Status));
        goto EXIT;
    }

    if (NULL != StreamContext->ShadowSectionObjectPointers->DataSectionObject)
    {

        FileObject->SectionObjectPointer = StreamContext->ShadowSectionObjectPointers;

        if (NULL == StreamContext->ShadowSectionObjectPointers->SharedCacheMap)
        {
            CHAR Buffer = {0};
            ByteOffset.QuadPart = 0;

            Status = FltReadFileEx(Instance, FileObject, &ByteOffset,
                                   sizeof(Buffer), &Buffer, 0, NULL, NULL, NULL, NULL, NULL);

            if (!NT_SUCCESS(Status) && STATUS_END_OF_FILE != Status)
            {
                PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s@%d PocInitShadowSectionObjectPointers->FltReadFileEx init ciphertext cache failed. Status = 0x%x\n", __FUNCTION__, __LINE__, Status));
                goto EXIT;
            }
        }

        if (CcIsFileCached(FileObject))
        {
            ExAcquireResourceExclusiveLite(((PFSRTL_ADVANCED_FCB_HEADER)(FileObject->FsContext))->Resource, TRUE);

            CcSetFileSizes(
                FileObject,
                (PCC_FILE_SIZES) & ((PFSRTL_ADVANCED_FCB_HEADER)(FileObject->FsContext))->AllocationSize);

            CcPurgeCacheSection(StreamContext->ShadowSectionObjectPointers, NULL, 0, FALSE);

            ExReleaseResourceLite(((PFSRTL_ADVANCED_FCB_HEADER)(FileObject->FsContext))->Resource);

            PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
                         ("%s@%d CcSetFileSizes chipertext cache map size.File = %ws AllocationSize = %I64d ValidDataLength = %I64d FileSize = %I64d SCM = %d DSO = %d.\n",
                          __FUNCTION__, __LINE__,
                          StreamContext->FileName,
                          ((PFSRTL_ADVANCED_FCB_HEADER)(FileObject->FsContext))->AllocationSize.QuadPart,
                          ((PFSRTL_ADVANCED_FCB_HEADER)(FileObject->FsContext))->ValidDataLength.QuadPart,
                          ((PFSRTL_ADVANCED_FCB_HEADER)(FileObject->FsContext))->FileSize.QuadPart,
                          NULL == StreamContext->ShadowSectionObjectPointers->SharedCacheMap ? 0 : 1,
                          NULL == StreamContext->ShadowSectionObjectPointers->DataSectionObject ? 0 : 1));
        }
    }

    ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);

    StreamContext->IsDirty = FALSE;

    ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);

EXIT:

    if (NULL != VolumeContext)
    {
        FltReleaseContext(VolumeContext);
        VolumeContext = NULL;
    }

    if (NULL != hFile)
    {
        FltClose(hFile);
        hFile = NULL;
    }

    if (NULL != FileObject)
    {
        ObDereferenceObject(FileObject);
        FileObject = NULL;
    }

    if (NULL != WriteBuffer)
    {
        FltFreePoolAlignedWithTag(Instance, WriteBuffer, WRITE_BUFFER_TAG);
        WriteBuffer = NULL;
    }

    return Status;
}

NTSTATUS PocReentryToEncrypt(
    IN PFLT_INSTANCE Instance,
    IN PWCHAR FileName)
{
    if (NULL == FileName)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->FileName is NULL.\n", __FUNCTION__));
        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    PPOC_STREAM_CONTEXT StreamContext = NULL;

    LONGLONG FileSize = 0;
    LARGE_INTEGER ByteOffset = {0};
    PCHAR ReadBuffer = NULL;

    UNICODE_STRING uFileName = {0};
    OBJECT_ATTRIBUTES ObjectAttributes = {0};

    HANDLE hFile = NULL;
    PFILE_OBJECT FileObject = NULL;
    IO_STATUS_BLOCK IoStatusBlock = {0};

    Status = PocReentryToGetStreamContext(
        Instance,
        FileName,
        &StreamContext);

    if (STATUS_SUCCESS != Status)
    {
        if (STATUS_NOT_FOUND == Status)
        {
            PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->PocReentryToGetStreamContext failed. Irrelevent file extension\n", __FUNCTION__));
            Status = POC_IRRELEVENT_FILE_EXTENSION;
        }
        else
        {
            PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->PocReentryToGetStreamContext failed. Status = 0x%x FileName = %ws\n",
                                                __FUNCTION__, Status, FileName));
        }
        goto EXIT;
    }

    if (TRUE == StreamContext->IsCipherText)
    {
        Status = POC_FILE_IS_CIPHERTEXT;

        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
                     ("%s->%ws is ciphertext. Encrypt failed. FileSize = %I64d.\n",
                      __FUNCTION__, FileName, StreamContext->FileSize));

        goto EXIT;
    }

    if (POC_RENAME_TO_ENCRYPT == StreamContext->Flag)
    {
        Status = STATUS_SUCCESS;
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
                     ("%s@%d %ws being rename to encrypt. Encrypt success.\n", __FUNCTION__, __LINE__, FileName));
        goto EXIT;
    }

    RtlInitUnicodeString(&uFileName, FileName);

    InitializeObjectAttributes(
        &ObjectAttributes,
        &uFileName,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);

    /*
     * 这里不能用FltCreateFile，因为它建的FileObject是非重入的，在这个FileObject上建立的缓冲也是非重入的，
     * 我们的PreWrite无法加密后面PocWriteFileIntoCache写入的数据。
     * 如果一个大文件，在特权加密之前并没有建立缓冲，那么这里就会出现上述情况，特权加密实际上是失败的。
     */

    Status = ZwCreateFile(
        &hFile,
        GENERIC_READ,
        &ObjectAttributes,
        &IoStatusBlock,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        FILE_OPEN,
        FILE_NON_DIRECTORY_FILE |
            FILE_SYNCHRONOUS_IO_NONALERT,
        NULL,
        0);

    if (STATUS_SUCCESS != Status)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->ZwCreateFile failed. Status = 0x%x\n", __FUNCTION__, Status));
        goto EXIT;
    }

    Status = ObReferenceObjectByHandle(
        hFile,
        STANDARD_RIGHTS_ALL,
        *IoFileObjectType,
        KernelMode,
        (PVOID *)&FileObject,
        NULL);

    if (STATUS_SUCCESS != Status)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->ObReferenceObjectByHandle failed. Status = 0x%x.\n", __FUNCTION__, Status));
        goto EXIT;
    }

    /*PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("\n %s->Process = %p Thread = %p Fcb = %p Ccb = %p Resource = %p PagingIoResource = %p.\n\n",
        __FUNCTION__,
        PsGetCurrentProcess(),
        PsGetCurrentThread(),
        (PCHAR)FileObject->FsContext,
        FileObject->FsContext2,
        ((PFSRTL_ADVANCED_FCB_HEADER)FileObject->FsContext)->Resource,
        ((PFSRTL_ADVANCED_FCB_HEADER)FileObject->FsContext)->PagingIoResource));*/

    FileSize = PocQueryEndOfFileInfo(Instance, FileObject);

    if (0 == FileSize)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->FileSize is zero.\n", __FUNCTION__));
        Status = STATUS_SUCCESS;
        goto EXIT;
    }

    ReadBuffer = ExAllocatePoolWithTag(PagedPool, FileSize, READ_BUFFER_TAG);

    if (NULL == ReadBuffer)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->ExAllocatePoolWithTag ReadBuffer failed.\n", __FUNCTION__));
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto EXIT;
    }

    RtlZeroMemory(ReadBuffer, FileSize);

    ByteOffset.QuadPart = 0;

    Status = PocReadFileFromCache(
        Instance,
        FileObject,
        ByteOffset,
        ReadBuffer,
        (ULONG)FileSize);

    if (STATUS_SUCCESS != Status)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->PocReadFileFromCache failed. Status = 0x%x\n", __FUNCTION__, Status));
        goto EXIT;
    }

    if (NULL != hFile)
    {
        FltClose(hFile);
        hFile = NULL;
    }

    if (NULL != FileObject)
    {
        ObDereferenceObject(FileObject);
        FileObject = NULL;
    }

    RtlZeroMemory(&ObjectAttributes, sizeof(ObjectAttributes));

    InitializeObjectAttributes(
        &ObjectAttributes,
        &uFileName,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);

    Status = ZwCreateFile(
        &hFile,
        GENERIC_WRITE,
        &ObjectAttributes,
        &IoStatusBlock,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ,
        FILE_OPEN,
        FILE_NON_DIRECTORY_FILE |
            FILE_SYNCHRONOUS_IO_NONALERT |
            FILE_WRITE_THROUGH,
        NULL,
        0);

    if (STATUS_SUCCESS != Status)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->ZwCreateFile failed. Status = 0x%x\n", __FUNCTION__, Status));
        goto EXIT;
    }

    PocUpdateFlagInStreamContext(StreamContext, 0);

    Status = ObReferenceObjectByHandle(
        hFile,
        STANDARD_RIGHTS_ALL,
        *IoFileObjectType,
        KernelMode,
        (PVOID *)&FileObject,
        NULL);

    if (STATUS_SUCCESS != Status)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->ObReferenceObjectByHandle failed. Status = 0x%x.\n", __FUNCTION__, Status));
        goto EXIT;
    }

    ByteOffset.QuadPart = 0;

    /*
     * 这里不能用FltWriteFileEx，因为它的缓冲写是非重入的，16个字节以内的文件
     * 我们需要缓冲写操作重入到minifilter中去扩展文件大小。
     */
    ZwWriteFile(
        hFile,
        NULL,
        NULL,
        NULL,
        &IoStatusBlock,
        ReadBuffer,
        (ULONG)FileSize,
        &ByteOffset,
        NULL);

    if (STATUS_SUCCESS != Status)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->ZwWriteFile failed. Status = 0x%x\n", __FUNCTION__, Status));
        goto EXIT;
    }

    PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("\n%s->success. FileName = %ws FileSize = %I64d.\n",
                                        __FUNCTION__,
                                        FileName,
                                        ((PFSRTL_ADVANCED_FCB_HEADER)(FileObject->FsContext))->FileSize.QuadPart));

EXIT:

    if (NULL != StreamContext)
    {
        FltReleaseContext(StreamContext);
        StreamContext = NULL;
    }

    if (NULL != ReadBuffer)
    {
        ExFreePoolWithTag(ReadBuffer, READ_BUFFER_TAG);
        ReadBuffer = NULL;
    }

    if (NULL != hFile)
    {
        ZwClose(hFile);
        hFile = NULL;
    }

    if (NULL != FileObject)
    {
        ObDereferenceObject(FileObject);
        FileObject = NULL;
    }

    return Status;
}

NTSTATUS PocReentryToDecrypt(
    IN PFLT_INSTANCE Instance,
    IN PWCHAR FileName)
{
    if (NULL == FileName)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->FileName is NULL.\n", __FUNCTION__));
        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    PPOC_STREAM_CONTEXT StreamContext = NULL;

    UNICODE_STRING uFileName = {0};
    OBJECT_ATTRIBUTES ObjectAttributes = {0};

    HANDLE hFile = NULL;
    PFILE_OBJECT FileObject = NULL;
    IO_STATUS_BLOCK IoStatusBlock = {0};

    LONGLONG FileSize = 0;
    LARGE_INTEGER ByteOffset = {0};

    PCHAR ReadBuffer = NULL;
    PCHAR WriteBuffer = NULL;

    ULONG WriteLength = 0, BytesWritten = 0;

    Status = PocReentryToGetStreamContext(
        Instance,
        FileName,
        &StreamContext);

    if (STATUS_SUCCESS != Status)
    {
        if (STATUS_NOT_FOUND == Status)
        {
            PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->PocReentryToGetStreamContext failed. Irrelevent file extension\n", __FUNCTION__));
            Status = POC_IRRELEVENT_FILE_EXTENSION;
        }
        else
        {
            PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->PocReentryToGetStreamContext failed. Status = 0x%x\n", __FUNCTION__, Status));
        }
        goto EXIT;
    }

    if (FALSE == StreamContext->IsCipherText)
    {
        Status = POC_FILE_IS_PLAINTEXT;
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->%ws is plaintext. Decrypt failed.\n", __FUNCTION__, FileName));
        goto EXIT;
    }

    PocUpdateFlagInStreamContext(StreamContext, 0);

    RtlInitUnicodeString(&uFileName, FileName);

    InitializeObjectAttributes(
        &ObjectAttributes,
        &uFileName,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);

    Status = ZwCreateFile(
        &hFile,
        GENERIC_READ,
        &ObjectAttributes,
        &IoStatusBlock,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        FILE_OPEN,
        FILE_NON_DIRECTORY_FILE |
            FILE_SYNCHRONOUS_IO_NONALERT,
        NULL,
        0);

    if (STATUS_SUCCESS != Status)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->ZwCreateFile failed. Status = 0x%x\n", __FUNCTION__, Status));
        goto EXIT;
    }

    Status = ObReferenceObjectByHandle(
        hFile,
        STANDARD_RIGHTS_ALL,
        *IoFileObjectType,
        KernelMode,
        (PVOID *)&FileObject,
        NULL);

    if (STATUS_SUCCESS != Status)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->ObReferenceObjectByHandle failed. Status = 0x%x.\n", __FUNCTION__, Status));
        goto EXIT;
    }

    if (FileObject->SectionObjectPointer == StreamContext->ShadowSectionObjectPointers)
    {
        if (TRUE == StreamContext->IsReEncrypted)
        {
            /*
             * PrivateCacheMap要置0，否则文件系统驱动不建立缓冲，不过这里不会进入了，
             * 因为在PostCreate对这个状态的文件，无论是什么进程，都指向明文缓冲。
             */
            FileObject->SectionObjectPointer = StreamContext->OriginSectionObjectPointers;
            FileObject->PrivateCacheMap = NULL;
        }
        else
        {
            PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->Unauthorized process can't decrypt file.\n", __FUNCTION__));
            Status = POC_IS_UNAUTHORIZED_PROCESS;
            goto EXIT;
        }
    }

    FileSize = StreamContext->FileSize;

    ReadBuffer = ExAllocatePoolWithTag(PagedPool, FileSize, READ_BUFFER_TAG);

    if (NULL == ReadBuffer)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->ExAllocatePoolWithTag ReadBuffer failed.\n", __FUNCTION__));
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto EXIT;
    }

    RtlZeroMemory(ReadBuffer, FileSize);

    ByteOffset.QuadPart = 0;

    Status = PocReadFileFromCache(
        Instance,
        FileObject,
        ByteOffset,
        ReadBuffer,
        (ULONG)FileSize);

    if (STATUS_SUCCESS != Status)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->PocReadFileFromCache failed. Status = 0x%x\n", __FUNCTION__, Status));
        goto EXIT;
    }

    Status = PocSetEndOfFileInfo(
        Instance,
        FileObject,
        FileSize);

    if (STATUS_SUCCESS != Status)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->PocSetEndOfFileInfo failed. Status = 0x%x\n", __FUNCTION__, Status));
        goto EXIT;
    }

    if (NULL != hFile)
    {
        ZwClose(hFile);
        hFile = NULL;
    }

    if (NULL != FileObject)
    {
        ObDereferenceObject(FileObject);
        FileObject = NULL;
    }

    RtlZeroMemory(&ObjectAttributes, sizeof(ObjectAttributes));

    InitializeObjectAttributes(
        &ObjectAttributes,
        &uFileName,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL);

    Status = FltCreateFileEx(
        gFilterHandle,
        Instance,
        &hFile,
        &FileObject,
        GENERIC_WRITE,
        &ObjectAttributes,
        &IoStatusBlock,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ,
        FILE_OPEN,
        FILE_NON_DIRECTORY_FILE |
            FILE_SYNCHRONOUS_IO_NONALERT |
            FILE_NO_INTERMEDIATE_BUFFERING,
        NULL,
        0,
        0);

    if (STATUS_SUCCESS != Status)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->FltCreateFileEx failed. Status = 0x%x\n", __FUNCTION__, Status));
        goto EXIT;
    }

    WriteLength = ROUND_TO_PAGES(FileSize);

    WriteBuffer = FltAllocatePoolAlignedWithTag(Instance, NonPagedPool, WriteLength, WRITE_BUFFER_TAG);

    if (NULL == WriteBuffer)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->FltAllocatePoolAlignedWithTag WriteBuffer failed.\n", __FUNCTION__));
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto EXIT;
    }

    RtlZeroMemory(WriteBuffer, WriteLength);

    RtlMoveMemory(WriteBuffer, ReadBuffer, FileSize);

    ByteOffset.QuadPart = 0;

    Status = FltWriteFileEx(
        Instance,
        FileObject,
        &ByteOffset,
        WriteLength,
        WriteBuffer,
        FLTFL_IO_OPERATION_NON_CACHED |
            FLTFL_IO_OPERATION_DO_NOT_UPDATE_BYTE_OFFSET |
            FLTFL_IO_OPERATION_PAGING |
            FLTFL_IO_OPERATION_SYNCHRONOUS_PAGING,
        &BytesWritten,
        NULL,
        NULL,
        NULL,
        NULL);

    if (STATUS_SUCCESS != Status)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->FltWriteFileEx failed. Status = 0x%x\n", __FUNCTION__, Status));
        goto EXIT;
    }

    PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->success. FileName = %ws FileSize = %I64d.\n\n",
                                        __FUNCTION__,
                                        FileName,
                                        ((PFSRTL_ADVANCED_FCB_HEADER)(FileObject->FsContext))->FileSize.QuadPart));

    PocUpdateFlagInStreamContext(StreamContext, 0);

    ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);

    StreamContext->IsCipherText = FALSE;
    StreamContext->FileSize = 0;
    RtlZeroMemory(StreamContext->FileName, POC_MAX_NAME_LENGTH * sizeof(WCHAR));

    StreamContext->IsReEncrypted = FALSE;

    ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);

EXIT:

    if (NULL != StreamContext)
    {
        FltReleaseContext(StreamContext);
        StreamContext = NULL;
    }

    if (NULL != hFile)
    {
        FltClose(hFile);
        hFile = NULL;
    }

    if (NULL != FileObject)
    {
        ObDereferenceObject(FileObject);
        FileObject = NULL;
    }

    if (NULL != ReadBuffer)
    {
        ExFreePoolWithTag(ReadBuffer, READ_BUFFER_TAG);
        ReadBuffer = NULL;
    }

    if (NULL != WriteBuffer)
    {
        FltFreePoolAlignedWithTag(Instance, WriteBuffer, WRITE_BUFFER_TAG);
        WriteBuffer = NULL;
    }

    return Status;
}

/**
 * @Author: wangzhankun
 * @Date: 2022-07-07 20:08:14
 * @LastEditors: wangzhankun
 * @update:
 * @brief
 * @param {IN PPOC_STREAM_CONTEXT} StreamContext
 * @return STATUS_SUCCESS 如果成功,  STATUS_TOO_MANY_THREADS 如果仍然有授权进程占有该文件; 其它返回值表示失败
 */
NTSTATUS PocAppendEncImmediately(IN PPOC_STREAM_CONTEXT StreamContext)
{
    if (NULL == StreamContext ||
        NULL == StreamContext->FileName ||
        NULL == StreamContext->Volume ||
        NULL == StreamContext->Instance)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
                     ("%s2%d StreamContext or FileName or Volume or Instance is NULL.\n", __FUNCTION__, __LINE__));
        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    Status = STATUS_SUCCESS;

    /*
     * 添加加密标识尾的地方
     * 或者如果加密标识尾内的FileName错了，PostClose更新一下
     * （之所以错误是因为对文件进行了重命名操作）
     * POC_TO_APPEND_ENCRYPTION_TAILER是在PreWrite设置的
     * POC_TAILER_WRONG_FILE_NAME是在PostCreate设置的
     * POC_RENAME_TO_ENCRYPT是在PostSetInfo设置的，针对的是tmp文件重命名为目标扩展文件的情况
     */

    ExAcquireResourceSharedLite(StreamContext->Resource, TRUE);

    if ((POC_TO_APPEND_ENCRYPTION_TAILER == StreamContext->Flag ||
         POC_TAILER_WRONG_FILE_NAME == StreamContext->Flag) &&
        StreamContext->AppendTailerThreadStart)
    {
        ExReleaseResourceLite(StreamContext->Resource);

        PocUpdateFlagInStreamContext(StreamContext, POC_BEING_APPEND_ENC_TAILER);

        Status = PocAppendEncTailerToFile(StreamContext->Volume, StreamContext->Instance, StreamContext);

        if (STATUS_SUCCESS != Status)
        {
            PocUpdateFlagInStreamContext(StreamContext, POC_TO_APPEND_ENCRYPTION_TAILER);
            PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->PocAppendEncTailerToFile failed. Status = 0x%x. FileName = %ws .\n",
                                                __FUNCTION__,
                                                Status,
                                                StreamContext->FileName));
        }
        else
        {
            PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("\n%s->Append tailer success. FileName = %ws.\n\n",
                                                __FUNCTION__,
                                                StreamContext->FileName));

            PocUpdateFlagInStreamContext(StreamContext, POC_FILE_HAS_ENCRYPTION_TAILER);
        }
    }
    else if (POC_RENAME_TO_ENCRYPT == StreamContext->Flag &&
             StreamContext->AppendTailerThreadStart)
    {
        ExReleaseResourceLite(StreamContext->Resource);

        PocUpdateFlagInStreamContext(StreamContext, POC_BEING_DIRECT_ENCRYPTING);

        /*
         * 其他类型文件重命名为目标扩展文件的情况，对重命名的文件进行加密
         * 这个POC_RENAME_TO_ENCRYPT是在PostSetInformation中设置的。
         */
        // Status = PocReentryToEncrypt(StreamContext->Instance, StreamContext->FileName);
        // TODO
        Status = PocManualEncryptOrDecrypt(StreamContext->FileName, StreamContext->Instance, TRUE);

        if (STATUS_SUCCESS != Status && POC_FILE_IS_CIPHERTEXT != Status)
        {
            PocUpdateFlagInStreamContext(StreamContext, POC_RENAME_TO_ENCRYPT);
            PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->PocReentryToEncrypt failed. Status = 0x%x.\n",
                                                __FUNCTION__,
                                                Status));
        }
        else
        {

            PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("\n%s->PocReentryToEncrypt success. FileName = %ws.\n\n",
                                                __FUNCTION__,
                                                StreamContext->FileName));
        }
    }
    else if (POC_TO_DECRYPT_FILE == StreamContext->Flag &&
             StreamContext->AppendTailerThreadStart)
    {
        ExReleaseResourceLite(StreamContext->Resource);

        PocUpdateFlagInStreamContext(StreamContext, POC_BEING_DECRYPT_FILE);

        // Status = PocReentryToDecrypt(StreamContext->Instance, StreamContext->FileName);
        // TODO 为啥需要解密呢？是在什么情况下有这个需求呢？
        Status = PocManualEncryptOrDecrypt(StreamContext->FileName, StreamContext->Instance, FALSE);

        if (STATUS_SUCCESS != Status)
        {
            PocUpdateFlagInStreamContext(StreamContext, POC_TO_DECRYPT_FILE);
            PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->PocReentryToDecrypt failed. Status = 0x%x.\n",
                                                __FUNCTION__,
                                                Status));
        }
        else
        {

            PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("\n%s->PocReentryToDecrypt success.\n\n",
                                                __FUNCTION__));
        }
    }
    else
    {
        Status = STATUS_UNSUCCESSFUL;
        ExReleaseResourceLite(StreamContext->Resource);
    }

    if (STATUS_SUCCESS == Status)
    {
        ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);
        StreamContext->AppendTailerThreadStart = FALSE;
        ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);
    }
    return Status;
}

NTSTATUS PocAppendEncTailerLazy(PPOC_STREAM_CONTEXT StreamContext)
{
    POC_IS_PARAMETER_NULL(StreamContext);
    POC_IS_PARAMETER_NULL(StreamContext->Instance);
    POC_IS_PARAMETER_NULL(StreamContext->FileName);
    POC_IS_PARAMETER_NULL(StreamContext->Resource);
    POC_IS_PARAMETER_NULL(StreamContext->Volume);

    if (PocAppendEncImmediately(StreamContext) == STATUS_SUCCESS)
    {
        return STATUS_SUCCESS;
    }
    PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s@%d try to PocAppendEncImmediately failed. FileName = %ws\n", __FUNCTION__, __LINE__, StreamContext->FileName));
    if (!g_append_enc_tailer_enabled)
    {
        return STATUS_UNSUCCESSFUL;
    }
    PPOC_ENC_TAILER_LIST node = ExAllocatePoolWithTag(NonPagedPool, sizeof(POC_ENC_TAILER_LIST), POC_ENC_TAILER);
    if (NULL == node)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->ExAllocatePoolWithTag failed.\n", __FUNCTION__));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    node->stream_context = StreamContext;

    ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);
    StreamContext->AppendTailerThreadStart = TRUE;
    ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);

    ExInterlockedInsertHeadList(&lg_add_stream_context_list_head, &node->list_entry, lg_add_stream_context_lock);
    return STATUS_SUCCESS;
}

VOID PocAppendEncTailerThread(PVOID param)
{
    UNREFERENCED_PARAMETER(param);

    KeWaitForSingleObject(lg_mutex, Executive, KernelMode, FALSE, NULL);

    NTSTATUS Status = STATUS_SUCCESS;

    LARGE_INTEGER Interval = {0};
    Interval.QuadPart = -10 * 1000 * 1000; // 1s

    int last_run_count = 3; //当驱动要求停止之后最多再执行这么多次之后就直接退出

    while (TRUE)
    {
        if (!g_append_enc_tailer_enabled)
        {
            last_run_count--;
            if (last_run_count <= 0)
            {
                break;
            }
            if (IsListEmpty(&lg_append_enc_tailer_list_head) && IsListEmpty(&lg_add_stream_context_list_head))
            {
                break;
            }
        }
        while (!IsListEmpty(&lg_add_stream_context_list_head))
        {
            PLIST_ENTRY p = ExInterlockedRemoveHeadList(&lg_add_stream_context_list_head, lg_add_stream_context_lock);
            if (NULL == p)
            {
                PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s@%d ExInterlockedRemoveHeadList failed.\n", __FUNCTION__, __LINE__));
                continue;
            }
            else
            {
                InsertTailList(&lg_append_enc_tailer_list_head, p);
            }
        }

        //正着查，保证最先添加的先处理
        PLIST_ENTRY pListEntry = lg_append_enc_tailer_list_head.Flink;

        while (pListEntry != &lg_append_enc_tailer_list_head)
        {

            PPOC_ENC_TAILER_LIST node = CONTAINING_RECORD(pListEntry, POC_ENC_TAILER_LIST, list_entry);
            pListEntry = pListEntry->Flink;

            if (node && node->stream_context)
            {
                PPOC_STREAM_CONTEXT StreamContext = node->stream_context;
                BOOLEAN remove_bool = TRUE;
                if (StreamContext->AppendTailerThreadStart)
                {
                    Status = PocAppendEncImmediately(StreamContext);

                    if (STATUS_SUCCESS != Status)
                    {
                        if (Status != STATUS_INVALID_PARAMETER || STATUS_OBJECT_NAME_NOT_FOUND != Status ||
                            STATUS_OBJECT_PATH_SYNTAX_BAD != Status)
                        {
                            remove_bool = FALSE;
                        }
                        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s@%d PocAppendEncTailerToFile failed. Status = 0x%x. FileName = %ws .\n",
                                                            __FUNCTION__, __LINE__,
                                                            Status,
                                                            StreamContext->FileName));
                    }
                    else
                    {
                        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("\n%s->Append tailer success. FileName = %ws.\n\n",
                                                            __FUNCTION__,
                                                            StreamContext->FileName));
                    }
                }

                if (remove_bool)
                {
                    if (NULL != node->stream_context)
                    {
                        FltReleaseContext(node->stream_context);
                        node->stream_context = NULL;
                    }
                    RemoveEntryList(&node->list_entry);
                    ExFreePoolWithTag(node, POC_ENC_TAILER);
                }
            }
        }
        KeDelayExecutionThread(KernelMode, FALSE, &Interval);
    };

    if (STATUS_SUCCESS != Status)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s@%d PocAppendEncImmediately failed. Status = 0x%x\n", __FUNCTION__, Status));
    }

    KeReleaseMutex(lg_mutex, FALSE);
    PsTerminateSystemThread(Status);
}

NTSTATUS PocCreateFileForEncTailer(
    IN PCFLT_RELATED_OBJECTS FltObjects,
    IN PPOC_STREAM_CONTEXT StreamContext,
    IN PWCHAR ProcessName)
{

    if (NULL == StreamContext)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocCreateFileForEncTailer->StreamContext is NULL.\n"));
        return STATUS_INVALID_PARAMETER;
    }

    if (NULL == StreamContext->FileName)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocCreateFileForEncTailer->StreamContext->FileName is NULL.\n"));
        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    LONGLONG FileSize = 0;
    LARGE_INTEGER ByteOffset = {0};
    PCHAR OutReadBuffer = NULL;
    ULONG BytesRead = 0;

    FileSize = PocQueryEndOfFileInfo(FltObjects->Instance, FltObjects->FileObject);

    if (FileSize < PAGE_SIZE)
    {
        Status = STATUS_END_OF_FILE;
        goto EXIT;
    }

    // PocReadFileNoCache里面会对ByteOffset对齐0x200
    ByteOffset.QuadPart = FileSize - PAGE_SIZE;

    Status = PocReadFileNoCache(
        FltObjects->Instance,
        FltObjects->Volume,
        StreamContext->FileName,
        ByteOffset,
        PAGE_SIZE,
        &OutReadBuffer,
        &BytesRead);

    if (!NT_SUCCESS(Status) || NULL == OutReadBuffer)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->PocReadFileNoCache failed. ProcessName = %ws Status = 0x%x\n", __FUNCTION__, ProcessName, Status));
        goto EXIT;
    }

    if (strncmp(((PPOC_ENCRYPTION_TAILER)OutReadBuffer)->Flag, EncryptionTailer.Flag,
                strlen(EncryptionTailer.Flag)) == 0 &&
        wcsncmp(((PPOC_ENCRYPTION_TAILER)OutReadBuffer)->FileName, StreamContext->FileName,
                wcslen(StreamContext->FileName)) == 0)
    {

        /*
         * 驱动加载后，文件如果有缓冲或者被内存映射读写过，清一下缓冲，防止出现密文
         * 只要驱动加载之前有缓冲，都要清掉。
         */
        if (0 == StreamContext->IsCipherText)
        {
            if (FltObjects->FileObject->SectionObjectPointer->DataSectionObject != NULL)
            {
                Status = PocNtfsFlushAndPurgeCache(FltObjects->Instance, FltObjects->FileObject);

                if (STATUS_SUCCESS != Status)
                {
                    PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocCreateFileForEncTailer->PocNtfsFlushAndPurgeCache failed. Status = 0x%x.\n", Status));
                }
                else
                {
                    PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocCreateFileForEncTailer->File has been opened. Flush and purge cache.\n"));
                }
            }
        }

        ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);

        if (0 == StreamContext->FileSize)
        {
            StreamContext->FileSize = ((PPOC_ENCRYPTION_TAILER)OutReadBuffer)->FileSize;
        }
        if (0 == StreamContext->IsCipherText)
        {
            StreamContext->IsCipherText = ((PPOC_ENCRYPTION_TAILER)OutReadBuffer)->IsCipherText;
        }

        ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);

        Status = POC_FILE_HAS_ENCRYPTION_TAILER;

        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("\n%s->File %ws has encryption tailer FileSize = %I64d ProcessName = %ws.\n",
                                            __FUNCTION__,
                                            StreamContext->FileName,
                                            StreamContext->FileSize,
                                            ProcessName));
    }
    else if (strncmp(((PPOC_ENCRYPTION_TAILER)OutReadBuffer)->Flag, EncryptionTailer.Flag,
                     strlen(EncryptionTailer.Flag)) == 0)
    {
        if (0 == StreamContext->IsCipherText)
        {
            if (FltObjects->FileObject->SectionObjectPointer->DataSectionObject != NULL)
            {
                Status = PocNtfsFlushAndPurgeCache(FltObjects->Instance, FltObjects->FileObject);

                if (STATUS_SUCCESS != Status)
                {
                    PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocCreateFileForEncTailer->PocNtfsFlushAndPurgeCache failed. Status = 0x%x.\n", Status));
                }
                else
                {
                    PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocCreateFileForEncTailer->File has been opened. Flush and purge cache.\n"));
                }
            }
        }

        ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);

        if (0 == StreamContext->FileSize)
        {
            StreamContext->FileSize = ((PPOC_ENCRYPTION_TAILER)OutReadBuffer)->FileSize;
        }
        if (0 == StreamContext->IsCipherText)
        {
            StreamContext->IsCipherText = ((PPOC_ENCRYPTION_TAILER)OutReadBuffer)->IsCipherText;
        }

        ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);

        Status = POC_TAILER_WRONG_FILE_NAME;

        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->Ciphetext->other extension->target extension. FileSize = %I64d ProcessName = %ws\n",
                                            __FUNCTION__,
                                            StreamContext->FileSize,
                                            ProcessName));
    }

EXIT:

    if ((POC_FILE_HAS_ENCRYPTION_TAILER == Status ||
         POC_TAILER_WRONG_FILE_NAME == Status) &&
        (StreamContext->FileSize & 0x0f) && OutReadBuffer)
    {
        for (int i = 0; i < AES_BLOCK_SIZE; i++)
            StreamContext->cipher_buffer[i] = ((PPOC_ENCRYPTION_TAILER)OutReadBuffer)->CipherText[i];
    }

    if (NULL != OutReadBuffer)
    {
        FltFreePoolAlignedWithTag(FltObjects->Instance, OutReadBuffer, READ_BUFFER_TAG);
        OutReadBuffer = NULL;
    }

    return Status;
}

NTSTATUS PocInitAndStartAppendEncTailerThread()
{
    InitializeListHead(&lg_append_enc_tailer_list_head);
    InitializeListHead(&lg_add_stream_context_list_head);

    lg_add_stream_context_lock = ExAllocatePoolWithTag(NonPagedPool, sizeof(KSPIN_LOCK), POC_ENC_TAILER);
    if (lg_add_stream_context_lock == NULL)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s@%d ExAllocatePoolWithTag failed.\n", __FUNCTION__, __LINE__));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(lg_add_stream_context_lock, sizeof(KSPIN_LOCK));

    KeInitializeSpinLock(lg_add_stream_context_lock);

    lg_mutex = ExAllocatePoolWithTag(NonPagedPool, sizeof(KMUTEX), POC_ENC_TAILER);
    if (lg_mutex == NULL)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s@%d ExAllocatePoolWithTag failed.\n", __FUNCTION__, __LINE__));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(lg_mutex, sizeof(KMUTEX));
    KeInitializeMutex(lg_mutex, 0);

    g_append_enc_tailer_enabled = TRUE;

    HANDLE h_thread_handle = NULL;
    NTSTATUS status = PsCreateSystemThread(
        &h_thread_handle,
        THREAD_ALL_ACCESS,
        NULL,
        NULL,
        NULL,
        PocAppendEncTailerThread,
        NULL);

    if (STATUS_SUCCESS != status)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
                     ("%s->PsCreateSystemThread PocAppendEncTailerThread failed. Status = 0x%x.\n",
                      __FUNCTION__,
                      status));
    }
    else
    {
        ZwClose(h_thread_handle);
    }

    return status;
}

NTSTATUS PocStopAndCleanAppendEncTailerThread()
{
    g_append_enc_tailer_enabled = FALSE;

    if (lg_mutex)
    {
        KeWaitForSingleObject(lg_mutex, Executive, KernelMode, FALSE, NULL); //不需要设置超时，因为线程会在有限时间内执行完毕
    }

    while (!IsListEmpty(&lg_add_stream_context_list_head))
    {
        PPOC_ENC_TAILER_LIST node = (PPOC_ENC_TAILER_LIST)RemoveHeadList(&lg_add_stream_context_list_head);
        PocAppendEncImmediately(node->stream_context);
        FltReleaseContext(node->stream_context);
        ExFreePoolWithTag(node, POC_ENC_TAILER);
    }
    while (!IsListEmpty(&lg_append_enc_tailer_list_head))
    {
        PPOC_ENC_TAILER_LIST node = (PPOC_ENC_TAILER_LIST)RemoveHeadList(&lg_append_enc_tailer_list_head);
        PocAppendEncImmediately(node->stream_context);
        FltReleaseContext(node->stream_context);
        ExFreePoolWithTag(node, POC_ENC_TAILER);
    }

    if (NULL != lg_add_stream_context_lock)
    {
        ExFreePoolWithTag(lg_add_stream_context_lock, POC_ENC_TAILER);
    }

    if (lg_mutex)
    {
        KeReleaseMutex(lg_mutex, FALSE);
        ExFreePoolWithTag(lg_mutex, POC_ENC_TAILER);
        lg_mutex = NULL;
    }
    return STATUS_SUCCESS;
}
