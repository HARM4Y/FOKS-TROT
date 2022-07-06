
#include "write.h"
#include "utils.h"
#include "cipher.h"
#include "filefuncs.h"
#include "process.h"
#include "context.h"


FLT_PREOP_CALLBACK_STATUS
PocPreWriteOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_CompletionContext_Outptr_ PVOID* CompletionContext
)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);

    NTSTATUS Status;

    WCHAR ProcessName[POC_MAX_NAME_LENGTH] = { 0 };

    PPOC_STREAM_CONTEXT StreamContext = NULL;
    BOOLEAN ContextCreated = FALSE;

    BOOLEAN NonCachedIo = FALSE;
    BOOLEAN PagingIo = FALSE;

    PCHAR OrigBuffer = NULL, NewBuffer = NULL;
    PMDL NewMdl = NULL;
    LONGLONG NewBufferLength = 0;

    PFSRTL_ADVANCED_FCB_HEADER AdvancedFcbHeader = NULL;
    LONGLONG LengthReturned = 0;

    PPOC_VOLUME_CONTEXT VolumeContext = NULL;
    ULONG SectorSize = 0;
    
    PPOC_SWAP_BUFFER_CONTEXT SwapBufferContext = NULL;
    
    const LONGLONG ByteCount = Data->Iopb->Parameters.Write.Length;
    const LONGLONG StartingVbo = Data->Iopb->Parameters.Write.ByteOffset.QuadPart;

    AdvancedFcbHeader = FltObjects->FileObject->FsContext;
    const LONGLONG FileSize = AdvancedFcbHeader->FileSize.QuadPart;

    NonCachedIo = BooleanFlagOn(Data->Iopb->IrpFlags, IRP_NOCACHE);
    PagingIo = BooleanFlagOn(Data->Iopb->IrpFlags, IRP_PAGING_IO);

    if (FlagOn(Data->Flags, FLTFL_CALLBACK_DATA_FAST_IO_OPERATION))
    {
        Status = FLT_PREOP_SUCCESS_NO_CALLBACK;
        goto ERROR;
    }
    if (0 == ByteCount)
    {
        Status = FLT_PREOP_SUCCESS_NO_CALLBACK;
        goto ERROR;
    }


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
            PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s->PocFindOrCreateStreamContext failed. Status = 0x%x.\n",
                __FUNCTION__,
                Status));
        }
        Status = FLT_PREOP_SUCCESS_NO_CALLBACK;
        goto ERROR;
    }

    Status = PocGetProcessName(Data, ProcessName);


    //PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, 
    //    ("\nPocPreWriteOperation->enter StartingVbo = %I64d Length = %d FileSize = %I64d ProcessName = %ws File = %ws.\n NonCachedIo = %d PagingIo = %d\n",
    //    Data->Iopb->Parameters.Write.ByteOffset.QuadPart,
    //    Data->Iopb->Parameters.Write.Length,
    //    FileSize,
    //    ProcessName, StreamContext->FileName,
    //    NonCachedIo,
    //    PagingIo));

    if (POC_RENAME_TO_ENCRYPT == StreamContext->Flag && NonCachedIo)
    {
        /*
        * δ���ܵ�doc,docx,ppt,pptx,xls,xlsx�ļ�������ֱ��д�������ļ�ʱ�����Զ����ܣ�
        * ���ǻ��ڸý��̹ر��Ժ�����ȥ�ж��Ƿ�Ӧ�ü��ܸ����ļ���
        */
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, 
            ("%s->Leave PostClose will encrypt the file. StartingVbo = %I64d Length = %I64d ProcessName = %ws File = %ws.\n",
                __FUNCTION__,
                Data->Iopb->Parameters.Write.ByteOffset.QuadPart,
                ByteCount,
                ProcessName, 
                StreamContext->FileName));

        Status = FLT_PREOP_SUCCESS_NO_CALLBACK;
        goto ERROR;
    }


    if (FltObjects->FileObject->SectionObjectPointer == 
        StreamContext->ShadowSectionObjectPointers)
    {
        /*
        * ������д�����Ļ��壬������NonCachedIo����������
        */
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
            ("%s->Block NonCachedIo = %d chipertext cachemap StartingVbo = %I64d Length = %I64d ProcessName = %ws File = %ws.",
                __FUNCTION__,
                NonCachedIo ? 1 : 0,
                Data->Iopb->Parameters.Write.ByteOffset.QuadPart,
                ByteCount,
                ProcessName,
                StreamContext->FileName));

        Data->IoStatus.Status = STATUS_SUCCESS;
        Data->IoStatus.Information = Data->Iopb->Parameters.Write.Length;

        Status = FLT_PREOP_COMPLETE;
        goto ERROR;
    }

    if(FileSize < StartingVbo)
    {
        Data->IoStatus.Status = STATUS_SUCCESS;
        Data->IoStatus.Information = 0;

        Status = FLT_PREOP_COMPLETE;
        goto ERROR;
    }


    SwapBufferContext = ExAllocatePoolWithTag(NonPagedPool, sizeof(POC_SWAP_BUFFER_CONTEXT), WRITE_BUFFER_TAG);

    if (NULL == SwapBufferContext)
    {
        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocPreWriteOperation->ExAllocatePoolWithTag SwapBufferContext failed.\n"));
        Data->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        Data->IoStatus.Information = 0;
        Status = FLT_PREOP_COMPLETE;
        goto ERROR;
    }

    RtlZeroMemory(SwapBufferContext, sizeof(POC_SWAP_BUFFER_CONTEXT));
            
    SwapBufferContext->OriginalLength = Data->Iopb->Parameters.Write.Length;
    SwapBufferContext->byte_offset = Data->Iopb->Parameters.Write.ByteOffset;


    if (!NonCachedIo)
    {
        /*
        * 16���ֽ�������չ�ļ���С������һ����PreSetInfo��������Ӧ����if (!PagingIo)����
        * NonCachedIoҪ��Length > SectorSize������if (!NonCachedIo)���С�
        */

        //������ byteoffset �Ƿ����� FileSize�����ڵĻ��ͻ����
        // Data->Iopb->Parameters.Write.ByteOffset.QuadPart = SwapBufferContext->byte_offset.QuadPart & ((LONGLONG)-16);
        // Data->Iopb->Parameters.Write.Length += (ULONG)(SwapBufferContext->byte_offset.QuadPart & 0x0f);

        ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);
        StreamContext->original_write_byteoffset = Data->Iopb->Parameters.Write.ByteOffset.QuadPart;
        StreamContext->original_write_length = Data->Iopb->Parameters.Write.Length;
        ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);
        FltSetCallbackDataDirty(Data);

    }


    if (!PagingIo)
    {
        /*
        * ��Ҫ��PostWrite�޸����Ļ���Ĵ�С
        */
        if (StartingVbo + ByteCount > FileSize)
        {
            SwapBufferContext->IsCacheExtend = TRUE;
        }
    }


    if (NonCachedIo)
    {
        Status = FltGetVolumeContext(FltObjects->Filter, FltObjects->Volume, &VolumeContext);

        if (!NT_SUCCESS(Status) || 0 == VolumeContext->SectorSize)
        {
            PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocPostReadOperation->FltGetVolumeContext failed. Status = 0x%x\n", Status));
            Data->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
            Data->IoStatus.Information = 0;
            Status = FLT_PREOP_COMPLETE;
            goto ERROR;
        }

        SectorSize = VolumeContext->SectorSize;

        if (NULL != VolumeContext)
        {
            FltReleaseContext(VolumeContext);
            VolumeContext = NULL;
        }


        //LengthReturned�Ǳ���Write������Ҫд������ // FILE_FLAG_WRITE_THROUGH ���иñ�־������£���һ��д��ʱ����֣�FileSize == 0, StartingVbo == 0���������ByteCount������ʵ��д��Ĵ�С�����ǻ������Ĵ�С�����ʵ��LengthReturned Ӧ���ڻ���io�ж�ȡ
        // if (!PagingIo || FileSize >= StartingVbo + ByteCount)
        // {
        //     LengthReturned = ByteCount;
        // }
        // else
        // {
        //     LengthReturned = FileSize - StartingVbo;
        // }
        LengthReturned = StreamContext->original_write_length;

        //PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocPreWriteOperation->RealToWrite = %I64d.\n", LengthReturned));
        
        if (Data->Iopb->Parameters.Write.MdlAddress != NULL) 
        {

            FLT_ASSERT(((PMDL)Data->Iopb->Parameters.Write.MdlAddress)->Next == NULL);

            OrigBuffer = MmGetSystemAddressForMdlSafe(Data->Iopb->Parameters.Write.MdlAddress,
                NormalPagePriority | MdlMappingNoExecute);

            if (OrigBuffer == NULL) 
            {
                PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocPreWriteOperation->Failed to get system address for MDL: %p\n",
                    Data->Iopb->Parameters.Write.MdlAddress));

                Data->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
                Data->IoStatus.Information = 0;
                Status = FLT_PREOP_COMPLETE;
                goto ERROR;
            }

        }
        else
        {
            OrigBuffer = Data->Iopb->Parameters.Write.WriteBuffer;
        }





        if (FALSE == StreamContext->IsCipherText &&
            FileSize % SectorSize == 0 &&
            FileSize > PAGE_SIZE &&
            NonCachedIo)
        {
            /*
            * �����ļ����ظ�������
            */
            if (StartingVbo <= FileSize - PAGE_SIZE &&
                StartingVbo + ByteCount >= FileSize - PAGE_SIZE + SectorSize)
            {
                if (strncmp(
                    ((PPOC_ENCRYPTION_TAILER)(OrigBuffer + FileSize - PAGE_SIZE - StartingVbo))->Flag, 
                    EncryptionTailer.Flag,
                    strlen(EncryptionTailer.Flag)) == 0)
                {

                    ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);

                    StreamContext->IsReEncrypted = TRUE;

                    ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);

                    PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
                        ("%s->File has been repeatedly encrypted. StartingVbo = %I64d Length = %I64d ProcessName = %ws File = %ws.",
                            __FUNCTION__,
                            Data->Iopb->Parameters.Write.ByteOffset.QuadPart,
                            ByteCount,
                            ProcessName,
                            StreamContext->FileName));

                }
            }
        }


        


        if (FileSize > AES_BLOCK_SIZE &&
            LengthReturned < AES_BLOCK_SIZE)
        {
            NewBufferLength = SectorSize + ByteCount;
        }
        else
        {
            NewBufferLength = ByteCount;
        }
        NewBufferLength = ROUND_TO_PAGES(NewBufferLength + PAGE_SIZE);//��֤ NewBufer ��ԭʼ�����
        NewBuffer = FltAllocatePoolAlignedWithTag(FltObjects->Instance, NonPagedPool, NewBufferLength, WRITE_BUFFER_TAG);

        if (NULL == NewBuffer)
        {
            PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocPreWriteOperation->FltAllocatePoolAlignedWithTag NewBuffer failed.\n"));
            Data->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
            Data->IoStatus.Information = 0;
            Status = FLT_PREOP_COMPLETE;
            goto ERROR;
        }

        RtlZeroMemory(NewBuffer, NewBufferLength);

        if (FlagOn(Data->Flags, FLTFL_CALLBACK_DATA_IRP_OPERATION)) 
        {

            NewMdl = IoAllocateMdl(NewBuffer, (ULONG)NewBufferLength, FALSE, FALSE, NULL);

            if (NewMdl == NULL) 
            {
                PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocPreWriteOperation->IoAllocateMdl NewMdl failed.\n"));
                Data->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
                Data->IoStatus.Information = 0;
                Status = FLT_PREOP_COMPLETE;
                goto ERROR;
            }

            MmBuildMdlForNonPagedPool(NewMdl);
        }
        


        try
        {

            {//��֪���Ƿ�Ҫ��  Data->Iopb->Parameters.Write ���е���
            // Ҳ��֪���Ƿ�Ҫ�� LengthReturned ���е���
                // �� noncachedio �� Data->Iopb->Parameters.Write.ByteOffset �� Write.Length �ƺ��ֱ��� ��������ʼλ���� �ļ��е�ƫ�� �� �������ĳ���
                // ����Ҫд���λ�����ļ��е�ƫ�ƺ�ʵ��Ҫд������ݳ��ȣ�������Ѿ��� cachedio �еĲ���
                //Data->Iopb->Parameters.Write.ByteOffset.QuadPart = SwapBufferContext->byte_offset.QuadPart & (((LONGLONG)-16));

                //�����ƺ���ÿ��д���ʱ�� Data->Iopb->Parameters.Write.Length ���ǻ��������ȣ�������ʵ��Ҫд������ݳ���
                // ʵ��Ҫд������ݳ���ʱ cachedio �� Data->Iopb->Parameters.Write.Length

                // Data->Iopb->Parameters.Write.Length += offset;

                // LengthReturned += (ULONG)(SwapBufferContext->byte_offset.QuadPart & 0x0f); // LengthReturned ���� cachedio ʱ��¼��ʵ��Ҫд������ݳ��ȣ����������Ҫ����ƫ��
                // �������ƫ��֮��᲻�ᵼ�»�����Խ���أ�һ������
            }
            LengthReturned += StreamContext->original_write_byteoffset - StartingVbo;
            // ULONG len = LengthReturned + (ULONG)(StreamContext->original_write_byteoffset - StartingVbo);
            if(LengthReturned)
            {
                ULONG bytesEncrypt = ROUND_TO_SIZE(LengthReturned, AES_BLOCK_SIZE);
                bytesEncrypt = bytesEncrypt;
                //Status = PocManualEncrypt(OrigBuffer, (ULONG)LengthReturned, NewBuffer, &bytesEncrypt, FileSize);
                RtlCopyMemory(NewBuffer, OrigBuffer, 128);
                PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s@%d NewBuffer is %s\n", __FUNCTION__, __LINE__, NewBuffer));
                PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s@%d LengthReturned = %d\n", __FUNCTION__, __LINE__, LengthReturned));


                //static int ii = 0;
                //ii++;
                //NewBuffer[0] = 'a' + (char)ii;
                if (STATUS_SUCCESS != Status)
                {
                    PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s@%d PocManualDecrypt error, Status = 0x%x\n", __FUNCTION__, __LINE__, Status));
                }
                else
                {
                    if ( LengthReturned & 0x0f)
                    {
                        LONGLONG offset = LengthReturned & ((LONGLONG)-16);
                        for (int i = 0; i < AES_BLOCK_SIZE; i++)
                        {
                            StreamContext->cipher_buffer[i] = ((CHAR*)NewBuffer)[offset + i];
                        }
                    }
                    PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s@%d PocManualDecrypt Success!\n", __FUNCTION__, __LINE__));
                }
            }

        }
        except(EXCEPTION_EXECUTE_HANDLER)
        {
            Data->IoStatus.Status = GetExceptionCode();
            Data->IoStatus.Information = 0;
            Status = FLT_PREOP_COMPLETE;
            goto ERROR;
        }



        SwapBufferContext->NewBuffer = NewBuffer;
        SwapBufferContext->NewMdl = NewMdl;
        SwapBufferContext->StreamContext = StreamContext;
        *CompletionContext = SwapBufferContext;

        Data->Iopb->Parameters.Write.WriteBuffer = NewBuffer;
        Data->Iopb->Parameters.Write.MdlAddress = NewMdl;
        FltSetCallbackDataDirty(Data);


        PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocPreWriteOperation->Encrypt success. StartingVbo = %I64d Length = %d ProcessName = %ws File = %ws.\n\n",
            Data->Iopb->Parameters.Write.ByteOffset.QuadPart,
            (ULONG)LengthReturned,
            ProcessName,
            StreamContext->FileName));


        Status = FLT_PREOP_SUCCESS_WITH_CALLBACK;
        goto EXIT;
    }



    *CompletionContext = SwapBufferContext;
    SwapBufferContext->StreamContext = StreamContext;
    Status = FLT_PREOP_SUCCESS_WITH_CALLBACK;
    goto EXIT;

ERROR:

    if (NULL != StreamContext)
    {
        FltReleaseContext(StreamContext);
        StreamContext = NULL;
    }

    if (NULL != NewBuffer)
    {
        FltFreePoolAlignedWithTag(FltObjects->Instance, NewBuffer, WRITE_BUFFER_TAG);
        NewBuffer = NULL;
    }

    if (NULL != NewMdl)
    {
        IoFreeMdl(NewMdl);
        NewMdl = NULL;
    }

    if (NULL != SwapBufferContext)
    {
        ExFreePoolWithTag(SwapBufferContext, WRITE_BUFFER_TAG);
        SwapBufferContext = NULL;
    }

EXIT:

    return Status;
}


FLT_POSTOP_CALLBACK_STATUS
PocPostWriteOperation(
    _Inout_ PFLT_CALLBACK_DATA Data,
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_opt_ PVOID CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags
)
{
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(FltObjects);
    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(Flags);


    ASSERT(CompletionContext != NULL);
    ASSERT(((PPOC_SWAP_BUFFER_CONTEXT)CompletionContext)->StreamContext != NULL);

    PPOC_SWAP_BUFFER_CONTEXT SwapBufferContext = NULL;
    PPOC_STREAM_CONTEXT StreamContext = NULL;

    SwapBufferContext = CompletionContext;
    StreamContext = SwapBufferContext->StreamContext;
    const BOOLEAN NonCachedIo = BooleanFlagOn(Data->Iopb->IrpFlags, IRP_NOCACHE);
    //��ԭ
    //Data->Iopb->Parameters.Write.Length = SwapBufferContext->OriginalLength;
    //Data->Iopb->Parameters.Write.ByteOffset = SwapBufferContext->byte_offset;


    if(!NonCachedIo)
    {
    //    if (STATUS_SUCCESS == Data->IoStatus.Status)
    //        Data->IoStatus.Information = SwapBufferContext->OriginalLength;
    }


    if (BooleanFlagOn(Data->Iopb->IrpFlags, IRP_NOCACHE))
    {
        /*
        * �ļ����޸Ĺ����һ�δд���ļ���ʶβ����ֹ���ݽ��̶��ļ�
        */
        ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);

        StreamContext->IsDirty = TRUE;

        ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);
    }


    if (BooleanFlagOn(Data->Iopb->IrpFlags, IRP_NOCACHE) &&
        (TRUE != StreamContext->LessThanAesBlockSize || ((PFSRTL_ADVANCED_FCB_HEADER)FltObjects->FileObject->FsContext)->FileSize.QuadPart > AES_BLOCK_SIZE))
    {
        /*
        * ��¼�ļ������Ĵ�С��С��16���ֽڵ�StreamContext->FileSize�Ѿ������������¹��ˣ�
        * ���ﲻ���ٸ����ˣ���Ϊ�����FileSize�Ѿ���16���ֽ��ˡ�
        */
        // ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);

        // StreamContext->FileSize = ((PFSRTL_ADVANCED_FCB_HEADER)FltObjects->FileObject->FsContext)->FileSize.QuadPart;

        // ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);
    }


    /*
    * ��չ���Ļ���Ĵ�С����PostWrite����Ϊ��������Ҫ�������ļ�ϵͳ������Writeȥ��չAllocationSize��ֵ��
    * ����Щֵ��չ�Ժ����ǲ����������Ļ���Ĵ�С��
    */
    if (TRUE == SwapBufferContext->IsCacheExtend && 
        NULL != StreamContext->ShadowSectionObjectPointers &&
        NULL != StreamContext->ShadowSectionObjectPointers->SharedCacheMap &&
        NULL != StreamContext->ShadowFileObject)
    {
        ExAcquireResourceExclusiveLite(((PFSRTL_ADVANCED_FCB_HEADER)(FltObjects->FileObject->FsContext))->Resource, TRUE);

        CcSetFileSizes(StreamContext->ShadowFileObject, 
            (PCC_FILE_SIZES) & ((PFSRTL_ADVANCED_FCB_HEADER)(FltObjects->FileObject->FsContext))->AllocationSize);

        ExReleaseResourceLite(((PFSRTL_ADVANCED_FCB_HEADER)(FltObjects->FileObject->FsContext))->Resource);
    }


    if (0 != SwapBufferContext->OriginalLength)
    {
        /*
        * д�볤�ȱ��޸Ĺ���������ԭ
        */
        // Data->IoStatus.Information = SwapBufferContext->OriginalLength;
    }


    if (Data->Iopb->Parameters.Write.ByteOffset.QuadPart +
        Data->Iopb->Parameters.Write.Length >=
        ((PFSRTL_ADVANCED_FCB_HEADER)FltObjects->FileObject->FsContext)->FileSize.QuadPart
        && BooleanFlagOn(Data->Iopb->IrpFlags, IRP_NOCACHE))
    {
        if (TRUE == StreamContext->IsReEncrypted)
        {
            /*
            * �ļ����ظ������ˣ�������PostClose��������һ��
            */
            PocUpdateFlagInStreamContext(StreamContext, POC_TO_DECRYPT_FILE);
        }
        else
        {
            /*
            * �ļ������ܣ�������PostClose����д���ļ���ʶβ
            */
            PocUpdateFlagInStreamContext(StreamContext, POC_TO_APPEND_ENCRYPTION_TAILER);
        }

        /*
        * �����ļ��ѱ����ܣ�����Read�Ż����
        */
        ExEnterCriticalRegionAndAcquireResourceExclusive(StreamContext->Resource);

        StreamContext->IsCipherText = TRUE;

        // StreamContext->LessThanAesBlockSize = FALSE;

        ExReleaseResourceAndLeaveCriticalRegion(StreamContext->Resource);

        if (NULL != StreamContext->FlushFileObject)
        {
            ObDereferenceObject(StreamContext->FlushFileObject);
            StreamContext->FlushFileObject = NULL;
        }
    }


    if (NULL != SwapBufferContext->NewBuffer)
    {
        FltFreePoolAlignedWithTag(FltObjects->Instance, SwapBufferContext->NewBuffer, WRITE_BUFFER_TAG);
        SwapBufferContext->NewBuffer = NULL;
    }

    if (NULL != SwapBufferContext)
    {
        ExFreePoolWithTag(SwapBufferContext, WRITE_BUFFER_TAG);
        SwapBufferContext = NULL;
    }

    if (NULL != StreamContext)
    {
        FltReleaseContext(StreamContext);
        StreamContext = NULL;
    }
    FltSetCallbackDataDirty(Data);

    return FLT_POSTOP_FINISHED_PROCESSING;
}
