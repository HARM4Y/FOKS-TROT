
#include "fileinfo.h"
#include "context.h"
#include "utils.h"
#include "filefuncs.h"
#include "process.h"
#include "cipher.h"
#include "appendenctailer.h"
FLT_POSTOP_CALLBACK_STATUS
PocPostSetInformationOperationWhenSafe(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags);

FLT_PREOP_CALLBACK_STATUS
PocPreQueryInformationOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    NTSTATUS Status;

    WCHAR ProcessName[POC_MAX_NAME_LENGTH] = {0};

    PPOC_STREAM_CONTEXT StreamContext = NULL;
    BOOLEAN ContextCreated = FALSE;

    PEPROCESS eProcess = NULL;
    HANDLE ProcessId = NULL;

    PPOC_CREATED_PROCESS_INFO OutProcessInfo = NULL;

    Status = PocFindOrCreateStreamContext(
        Data->Iopb->TargetInstance,
        Data->Iopb->TargetFileObject,
        FALSE,
        &StreamContext,
        &ContextCreated);

    if (STATUS_SUCCESS != Status)
    {
        if (STATUS_NOT_FOUND != Status && !FsRtlIsPagingFile(Data->Iopb->TargetFileObject))
        /*
        * ˵������Ŀ����չ�ļ�����Create��û�д���StreamContext������Ϊ�Ǹ�����
        * ������һ��Paging file������᷵��0xc00000bb��
        * ԭ����Fcb->Header.Flags2, FSRTL_FLAG2_SUPPORTS_FILTER_CONTEXTS�������
        *
        //
        //  To make FAT match the present functionality of NTFS, disable
        //  stream contexts on paging files
        //

        if (IsPagingFile) {
            SetFlag( Fcb->Header.Flags2, FSRTL_FLAG2_IS_PAGING_FILE );
            ClearFlag( Fcb->Header.Flags2, FSRTL_FLAG2_SUPPORTS_FILTER_CONTEXTS );
        }
        */
        {
            PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->PocFindOrCreateStreamContext failed. Status = 0x%x\n",
                                                __FUNCTION__,
                                                Status));
        }
        Status = FLT_PREOP_SUCCESS_NO_CALLBACK;
        goto EXIT;
    }

    Status = PocGetProcessName(Data, ProcessName);

    if (!StreamContext->IsCipherText && 0 == StreamContext->FileSize)
    {
        Status = FLT_PREOP_SUCCESS_NO_CALLBACK;
        goto EXIT;
    }

    if (FlagOn(Data->Flags, FLTFL_CALLBACK_DATA_FAST_IO_OPERATION))
    {
        Status = FLT_PREOP_DISALLOW_FASTIO;
        goto EXIT;
    }

    try
    {
        eProcess = FltGetRequestorProcess(Data);

        if (NULL == eProcess)
        {
            PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->EProcess FltGetRequestorProcess failed.\n", __FUNCTION__));
            leave;
        }

        ProcessId = PsGetProcessId(eProcess);

        if (NULL == ProcessId)
        {
            PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
                         ("%s->PsGetProcessId %p failed.\n",
                          __FUNCTION__, eProcess));
            leave;
        }

        Status = PocFindProcessInfoNodeByPidEx(
            ProcessId,
            &OutProcessInfo,
            FALSE,
            FALSE);
    }
    except(EXCEPTION_EXECUTE_HANDLER)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->Except 0x%x.\n", __FUNCTION__, GetExceptionCode()));
        Status = FLT_PREOP_SUCCESS_NO_CALLBACK;
        goto EXIT;
    }

    /*
     * ���ݽ��̿��Կ����������ļ��������ļ���ʶβ�����Բ�����PostQueryȥ���غ������ݡ�
     *
     * ������ļ����ݽṹ��PreRead�е�ע�����л���
     */
    if (NULL != OutProcessInfo &&
        POC_PR_ACCESS_BACKUP == OutProcessInfo->OwnedProcessRule->Access)
    {
        Status = FLT_PREOP_SUCCESS_NO_CALLBACK;
        goto EXIT;
    }

    /*PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("\nPocPreQueryInformationOperation->enter FileInformationClass = %d ProcessName = %ws File = %ws.\n",
        Data->Iopb->Parameters.QueryFileInformation.FileInformationClass,
        ProcessName, StreamContext->FileName));*/

    *CompletionContext = StreamContext;
    Status = FLT_PREOP_SUCCESS_WITH_CALLBACK;
    return Status;

EXIT:

    if (NULL != StreamContext)
    {
        FltReleaseContext(StreamContext);
        StreamContext = NULL;
    }

    return Status;
}

FLT_POSTOP_CALLBACK_STATUS
PocPostQueryInformationOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(Flags);

    ASSERT(CompletionContext != NULL);

    PPOC_STREAM_CONTEXT StreamContext = NULL;
    PVOID InfoBuffer = NULL;

    StreamContext = CompletionContext;
    InfoBuffer = Data->Iopb->Parameters.QueryFileInformation.InfoBuffer;

    /*
     * StreamContext->FileSize��¼�����ĵĴ�С����д�뵽�˱�ʶβ�У����������ϲ�����"���ĳ���"֮�����ݵĵط�֮һ
     * ��һ���ط���Read�У���Գ������ĳ��ȵ�CachedIo����Ȩ���̺ͷ���Ȩ�������غ�������ݡ�
     */
    switch (Data->Iopb->Parameters.QueryFileInformation.FileInformationClass)
    {

    case FileStandardInformation:
    {
        PFILE_STANDARD_INFORMATION Info = (PFILE_STANDARD_INFORMATION)InfoBuffer;
        Info->EndOfFile.QuadPart = StreamContext->FileSize;
        break;
    }
    case FileAllInformation:
    {
        PFILE_ALL_INFORMATION Info = (PFILE_ALL_INFORMATION)InfoBuffer;
        if (Data->IoStatus.Information >=
            sizeof(FILE_BASIC_INFORMATION) +
                sizeof(FILE_STANDARD_INFORMATION))
        {
            Info->StandardInformation.EndOfFile.QuadPart = StreamContext->FileSize;
        }
        break;
    }
    case FileEndOfFileInformation:
    {
        PFILE_END_OF_FILE_INFORMATION Info = (PFILE_END_OF_FILE_INFORMATION)InfoBuffer;
        Info->EndOfFile.QuadPart = StreamContext->FileSize;
        break;
    }
    case FileNetworkOpenInformation:
    {
        PFILE_NETWORK_OPEN_INFORMATION Info = (PFILE_NETWORK_OPEN_INFORMATION)InfoBuffer;
        Info->EndOfFile.QuadPart = StreamContext->FileSize;
        break;
    }
    default:
    {
        break;
    }
    }

    if (NULL != StreamContext)
    {
        FltReleaseContext(StreamContext);
        StreamContext = NULL;
    }

    return FLT_POSTOP_FINISHED_PROCESSING;
}

FLT_PREOP_CALLBACK_STATUS
PocPreSetInformationOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID *CompletionContext)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    NTSTATUS Status;

    PPOC_STREAM_CONTEXT StreamContext = NULL;
    BOOLEAN ContextCreated = FALSE;

    PVOID InfoBuffer = NULL;
    WCHAR ProcessName[POC_MAX_NAME_LENGTH] = {0};
    PocGetProcessName(Data, ProcessName);

    Status = PocFindOrCreateStreamContext(
        Data->Iopb->TargetInstance,
        Data->Iopb->TargetFileObject,
        FALSE,
        &StreamContext,
        &ContextCreated);

    if (STATUS_SUCCESS != Status)
    {
        if (STATUS_NOT_FOUND != Status && !FsRtlIsPagingFile(Data->Iopb->TargetFileObject))
        /*
        * ˵������Ŀ����չ�ļ�����Create��û�д���StreamContext������Ϊ�Ǹ�����
        * ������һ��Paging file������᷵��0xc00000bb��
        * ԭ����Fcb->Header.Flags2, FSRTL_FLAG2_SUPPORTS_FILTER_CONTEXTS�������
        *
        //
        //  To make FAT match the present functionality of NTFS, disable
        //  stream contexts on paging files
        //

        if (IsPagingFile) {
            SetFlag( Fcb->Header.Flags2, FSRTL_FLAG2_IS_PAGING_FILE );
            ClearFlag( Fcb->Header.Flags2, FSRTL_FLAG2_SUPPORTS_FILTER_CONTEXTS );
        }
        */
        {
            PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->PocFindOrCreateStreamContext failed. Status = 0x%x\n",
                                                __FUNCTION__,
                                                Status));
        }
        goto EXIT;
    }

    /*
     * ���Ӧ�ó�����Ҫд��16���ֽ����ڵ����ݣ����������ڴ�ӳ��д��Ļ����ǲ��ᵽCachedIo Write�ģ�����������ǰ����EOF��
     * ���ǽ�����չ��16���ֽڣ��Ա�AES�����Ժ���ܵ�������д������С�
     *
     * ��һ����չ�ĵط���PreWrite->CachedIo
     */
    InfoBuffer = Data->Iopb->Parameters.SetFileInformation.InfoBuffer;

    switch (Data->Iopb->Parameters.SetFileInformation.FileInformationClass)
    {
    case FileEndOfFileInformation:
    {
        // PFILE_END_OF_FILE_INFORMATION Info = (PFILE_END_OF_FILE_INFORMATION)InfoBuffer;

        // if (Info->EndOfFile.QuadPart < AES_BLOCK_SIZE && Info->EndOfFile.QuadPart > 0)
        // {
        //     ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);

        //     StreamContext->FileSize = Info->EndOfFile.QuadPart;
        //     StreamContext->LessThanAesBlockSize = TRUE;

        //     ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);

        //     Info->EndOfFile.QuadPart = (Info->EndOfFile.QuadPart / AES_BLOCK_SIZE + 1) * AES_BLOCK_SIZE;

        //     Status = PocGetProcessName(Data, ProcessName);

        //     PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->EndOfFile filename = %ws origin filesize = %I64d new filesize = %I64d process = %ws.\n",
        //         __FUNCTION__,
        //         StreamContext->FileName,
        //         StreamContext->FileSize,
        //         Info->EndOfFile.QuadPart,
        //         ProcessName));

        //     FltSetCallbackDataDirty(Data);
        // }
        // else if (0 == Info->EndOfFile.QuadPart)
        // {
        //     ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);

        //     StreamContext->FileSize = Info->EndOfFile.QuadPart;
        //     StreamContext->IsCipherText = FALSE;

        //     ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);
        // }

        // ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);
        // StreamContext->FileSize = Info->EndOfFile.QuadPart;
        // ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);

        break;
    }
    case FileStandardInformation:
    {
        // PFILE_STANDARD_INFORMATION Info = (PFILE_STANDARD_INFORMATION)InfoBuffer;

        // if (Info->EndOfFile.QuadPart < AES_BLOCK_SIZE && Info->EndOfFile.QuadPart > 0)
        // {
        //     Info->EndOfFile.QuadPart = (Info->EndOfFile.QuadPart / AES_BLOCK_SIZE + 1) * AES_BLOCK_SIZE;

        //     ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);

        //     StreamContext->FileSize = Info->EndOfFile.QuadPart;
        //     StreamContext->LessThanAesBlockSize = TRUE;

        //     ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);

        //     /*PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->StandInfo EndOfFile filename = %ws origin filesize = %I64d new filesize = %I64d.\n",
        //         __FUNCTION__,
        //         StreamContext->FileName,
        //         StreamContext->FileSize,
        //         Info->EndOfFile.QuadPart));*/

        //     FltSetCallbackDataDirty(Data);
        // }
        // else if (0 == Info->EndOfFile.QuadPart)
        // {
        //     ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);

        //     StreamContext->FileSize = Info->EndOfFile.QuadPart;
        //     StreamContext->IsCipherText = FALSE;

        //     ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);
        // }

        // if (Info->AllocationSize.QuadPart < AES_BLOCK_SIZE && Info->AllocationSize.QuadPart > 0)
        // {
        //     Info->AllocationSize.QuadPart = (Info->AllocationSize.QuadPart / AES_BLOCK_SIZE + 1) * AES_BLOCK_SIZE;

        //     /*PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->StandInfo Alloc filename = %ws origin filesize = %I64d new filesize = %I64d.\n",
        //         __FUNCTION__,
        //         StreamContext->FileName,
        //         StreamContext->FileSize,
        //         Info->AllocationSize.QuadPart));*/

        //     FltSetCallbackDataDirty(Data);
        // }
        // ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);
        // StreamContext->FileSize = Info->EndOfFile.QuadPart;
        // ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);
        break;
    }
    case FileAllocationInformation:
    {
        // PFILE_ALLOCATION_INFORMATION Info = (PFILE_ALLOCATION_INFORMATION)InfoBuffer;

        // if (Info->AllocationSize.QuadPart < AES_BLOCK_SIZE && Info->AllocationSize.QuadPart > 0)
        // {
        //     Info->AllocationSize.QuadPart = (Info->AllocationSize.QuadPart / AES_BLOCK_SIZE + 1) * AES_BLOCK_SIZE;

        //     /*PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->AllocInfo filename = %ws origin filesize = %I64d new filesize = %I64d.\n",
        //         __FUNCTION__,
        //         StreamContext->FileName,
        //         StreamContext->FileSize,
        //         Info->AllocationSize.QuadPart));*/

        //     FltSetCallbackDataDirty(Data);
        // }

        break;
    }
    case FileDispositionInformation:
    {
        ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);

        StreamContext->FileSize = 0;
        StreamContext->IsCipherText = FALSE;

        ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);
    }
    }

EXIT:

    Status = FLT_PREOP_SUCCESS_WITH_CALLBACK;

    if (NULL != StreamContext)
    {
        FltReleaseContext(StreamContext);
        StreamContext = NULL;
    }

    return Status;
}

FLT_POSTOP_CALLBACK_STATUS
PocPostSetInformationOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags)
{
    FLT_POSTOP_CALLBACK_STATUS Status = FLT_POSTOP_FINISHED_PROCESSING;

    if (!FltDoCompletionProcessingWhenSafe(Data,
                                           FltObjects,
                                           CompletionContext,
                                           Flags,
                                           PocPostSetInformationOperationWhenSafe,
                                           &Status))
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
                     ("%s->FltDoCompletionProcessingWhenSafe failed. Status = 0x%x.\n",
                      __FUNCTION__,
                      Status));
    }

    return Status;
}

FLT_POSTOP_CALLBACK_STATUS
PocPostSetInformationOperationWhenSafe(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(Flags);

    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    PFILE_OBJECT TargetFileObject = NULL;
    PFILE_RENAME_INFORMATION Buffer = NULL;

    WCHAR NewFileName[POC_MAX_NAME_LENGTH] = {0};

    PPOC_STREAM_CONTEXT StreamContext = NULL;
    BOOLEAN ContextCreated = FALSE;

    // UNICODE_STRING uFileName = { 0 };
    // OBJECT_ATTRIBUTES ObjectAttributes = { 0 };
    HANDLE FileHandle = NULL;
    // IO_STATUS_BLOCK IoStatusBlock = { 0 };

    WCHAR ProcessName[POC_MAX_NAME_LENGTH] = {0};
    PocGetProcessName(Data, ProcessName);

    PAGED_CODE();

    if (STATUS_SUCCESS != Data->IoStatus.Status)
    {
        Status = FLT_POSTOP_FINISHED_PROCESSING;
        goto EXIT;
    }

    switch (Data->Iopb->Parameters.SetFileInformation.FileInformationClass)
    {
    case FileRenameInformation:
    case FileRenameInformationEx:
    {

        TargetFileObject = Data->Iopb->Parameters.SetFileInformation.ParentOfTarget;

        if (NULL == TargetFileObject)
        {
            Buffer = Data->Iopb->Parameters.SetFileInformation.InfoBuffer;

            if (Buffer->FileNameLength < sizeof(NewFileName))
            {
                RtlMoveMemory(NewFileName, Buffer->FileName, Buffer->FileNameLength);
            }
        }
        else
        {
            if (NULL != TargetFileObject->FileName.Buffer &&
                TargetFileObject->FileName.MaximumLength < sizeof(NewFileName))
            {
                RtlMoveMemory(NewFileName,
                              TargetFileObject->FileName.Buffer,
                              TargetFileObject->FileName.MaximumLength);
            }
            else
            {
                goto EXIT;
            }
        }

        Status = PocBypassIrrelevantBy_PathAndExtension(Data);

        if (POC_IS_TARGET_FILE_EXTENSION == Status)
        {
            Status = PocFindOrCreateStreamContext(
                Data->Iopb->TargetInstance,
                Data->Iopb->TargetFileObject,
                TRUE,
                &StreamContext,
                &ContextCreated);
            if (Status == STATUS_SUCCESS)
            {
                if (ContextCreated)
                {
                    PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
                                 ("%s@%d PocFindOrCreateStreamContext create new success.\n",
                                  __FUNCTION__, __LINE__));

                    ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);

                    RtlZeroMemory(StreamContext->FileName, POC_MAX_NAME_LENGTH * sizeof(WCHAR));

                    if (wcslen(NewFileName) < POC_MAX_NAME_LENGTH)
                        RtlMoveMemory(StreamContext->FileName, NewFileName, wcslen(NewFileName) * sizeof(WCHAR));

                    StreamContext->OriginSectionObjectPointers = FltObjects->FileObject->SectionObjectPointer;

                    StreamContext->Volume = FltObjects->Volume;
                    StreamContext->Instance = FltObjects->Instance;

                    ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);

                    NTSTATUS ProcessType = PocIsUnauthorizedProcess(ProcessName);

                    if (POC_IS_AUTHORIZED_PROCESS == ProcessType &&
                        FlagOn(Data->Iopb->Parameters.Create.SecurityContext->DesiredAccess,
                               (FILE_WRITE_DATA | FILE_APPEND_DATA)) &&
                        NULL == StreamContext->FlushFileObject)
                    {
                        Status = PocInitFlushFileObject(
                            StreamContext->FileName,
                            &StreamContext->FlushFileObject);
                    }

                    if (FlagOn(Data->Iopb->Parameters.Create.SecurityContext->DesiredAccess, (FILE_READ_DATA)) &&
                        POC_IS_AUTHORIZED_PROCESS != ProcessType)
                    {
                        if (NULL == StreamContext->FlushFileObject)
                        {
                            Status = PocInitFlushFileObject(
                                StreamContext->FileName,
                                &StreamContext->FlushFileObject);
                        }

                        Status = PocFlushOriginalCache(
                            FltObjects->Instance,
                            StreamContext->FileName);
                    }
                }
                else
                {
                    PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
                                 ("%s@%d PocFindOrCreateStreamContext already exists.\n",
                                  __FUNCTION__, __LINE__));
                }
                PocUpdateFlagInStreamContext(StreamContext, POC_RENAME_TO_ENCRYPT);
                Status = PocAppendEncTailerLazy(StreamContext); // ���õ����ظ����ܺ��ظ�����ļ���ʶβ�����⡣�������Զ������
                if (Status == STATUS_SUCCESS)
                {
                    PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
                                 ("%s@%d PocAppendEncTailerLazy success. ProcessName = %ws, FileName = %ws\n",
                                  __FUNCTION__, __LINE__, ProcessName, StreamContext->FileName));
                }
                else
                {
                    PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
                                 ("%s@%d PocAppendEncTailerLazy Failed. ProcessName = %ws, FileName = %ws\n",
                                  __FUNCTION__, __LINE__, ProcessName, StreamContext->FileName));
                }
            }
            else
            {
                PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
                             ("%s@%d PocFindOrCreateStreamContext failed. Status = 0x%x.\n",
                              __FUNCTION__, __LINE__, Status));
            }
        }

        break;
    }
    }

EXIT:

    // if (NULL != StreamContext)
    // {
    //     FltReleaseContext(StreamContext);
    //     StreamContext = NULL;
    // }

    if (NULL != FileHandle)
    {
        FltClose(FileHandle);
        FileHandle = NULL;
    }

    return FLT_POSTOP_FINISHED_PROCESSING;
}
