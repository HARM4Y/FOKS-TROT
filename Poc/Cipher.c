

#include "cipher.h"
#include "global.h"

AES_INIT_VARIABLES AesInitVar;


NTSTATUS PocInitAesECBKey()
/*
* �����㷨�ĳ�ʼ����AES-128 ECBģʽ����Կ��rgbAES128Key
*/
{
	NTSTATUS Status = STATUS_UNSUCCESSFUL;

	ULONG cbData = 0, cbKeyObject = 0;

	UCHAR rgbAES128Key[] =
	{
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
	};

	RtlZeroMemory(&AesInitVar, sizeof(AES_INIT_VARIABLES));

	Status = BCryptOpenAlgorithmProvider(&AesInitVar.hAesAlg, BCRYPT_AES_ALGORITHM, NULL, BCRYPT_PROV_DISPATCH);

	if (!NT_SUCCESS(Status))
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocInitAesECBKey->BCryptOpenAlgorithmProvider failed. Status = 0x%x.\n", Status));
		goto ERROR;
	}
	
	Status = BCryptGetProperty(AesInitVar.hAesAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&cbKeyObject, sizeof(ULONG), &cbData, 0);

	if (!NT_SUCCESS(Status))
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocInitAesECBKey->BCryptGetProperty failed. Status = 0x%x.\n", Status));
		goto ERROR;
	}

	AesInitVar.pbKeyObject = ExAllocatePoolWithTag(NonPagedPool, cbKeyObject, KEY_OBJECT_BUFFER);

	if (NULL == AesInitVar.pbKeyObject)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocInitAesECBKey->ExAllocatePoolWithTag pbKeyObject failed.\n"));
		goto ERROR;
	}

	Status = BCryptSetProperty(AesInitVar.hAesAlg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0);

	if (!NT_SUCCESS(Status))
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocInitAesECBKey->BCryptSetProperty failed. Status = 0x%x.\n", Status));
		goto ERROR;
	}

	Status = BCryptGenerateSymmetricKey(AesInitVar.hAesAlg, &AesInitVar.hKey, AesInitVar.pbKeyObject, cbKeyObject, rgbAES128Key, sizeof(rgbAES128Key), 0);

	if (!NT_SUCCESS(Status))
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocInitAesECBKey->BCryptGenerateSymmetricKey failed. Status = 0x%x.\n", Status));
		goto ERROR;
	}

	Status = STATUS_SUCCESS;
	AesInitVar.Flag = TRUE;
	goto EXIT;

ERROR:

	if (NULL != AesInitVar.hKey)
	{
		BCryptDestroyKey(AesInitVar.hKey);
		AesInitVar.hKey = NULL;
	}

	if (NULL != AesInitVar.pbKeyObject)
	{
		ExFreePoolWithTag(AesInitVar.pbKeyObject, KEY_OBJECT_BUFFER);
		AesInitVar.pbKeyObject = NULL;
	}

	if (NULL != AesInitVar.hAesAlg)
	{
		BCryptCloseAlgorithmProvider(AesInitVar.hAesAlg, 0);
		AesInitVar.hAesAlg = 0;
	}

	AesInitVar.Flag = FALSE;


EXIT:

	return Status;
}


VOID PocAesCleanup()
{
	if (!AesInitVar.Flag)
	{
		return;
	}

	if (NULL != AesInitVar.hKey)
	{
		BCryptDestroyKey(AesInitVar.hKey);
		AesInitVar.hKey = NULL;
	}

	if (NULL != AesInitVar.pbKeyObject)
	{
		ExFreePoolWithTag(AesInitVar.pbKeyObject, KEY_OBJECT_BUFFER);
		AesInitVar.pbKeyObject = NULL;
	}

	if (NULL != AesInitVar.hAesAlg)
	{
		BCryptCloseAlgorithmProvider(AesInitVar.hAesAlg, 0);
		AesInitVar.hAesAlg = NULL;
	}

	AesInitVar.Flag = FALSE;
}


NTSTATUS PocAesECBEncrypt(
	IN PCHAR InBuffer, 
	IN ULONG InBufferSize, 
	IN OUT PCHAR InOutBuffer, 
	IN OUT PULONG LengthReturned)
{
	//LengthReturned�Ǹ��õģ��ڼ���ʱ������ΪInOutBuffer���ڴ��С���룬Ҳ��Ϊ���ܺ����Ĵ�С���

	if (!AesInitVar.Flag)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocAesECBEncrypt->AesInitVar.Flag = FALSE.\n"));
		return POC_STATUS_AES_INIT_FAILED;
	}

	if (NULL == InBuffer)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocAesECBEncrypt->InBuffer is NULL.\n"));
		return STATUS_INVALID_PARAMETER;
	}

	if (NULL == LengthReturned)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocAesECBEncrypt->LengthReturned is NULL.\n"));
		return STATUS_INVALID_PARAMETER;
	}


	NTSTATUS Status = STATUS_UNSUCCESSFUL;
	

	Status = BCryptEncrypt(AesInitVar.hKey, (PUCHAR)InBuffer, InBufferSize,
		NULL, NULL, 0, (PUCHAR)InOutBuffer, *LengthReturned, LengthReturned, 0);

	if (STATUS_SUCCESS != Status)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocAesECBEncrypt->BCryptEncrypt encrypt plaintext failed. Status = 0x%x\n", Status));
	}

	return Status;
}


NTSTATUS PocAesECBDecrypt(
	IN PCHAR InBuffer, 
	IN ULONG InBufferSize, 
	IN OUT PCHAR InOutBuffer, 
	IN OUT PULONG LengthReturned)
{

	if (!AesInitVar.Flag)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocAesECBDecrypt->AesInitVar.Flag = FALSE.\n"));
		return POC_STATUS_AES_INIT_FAILED;
	}

	if (NULL == InBuffer)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocAesECBDecrypt->InBuffer is NULL.\n"));
		return STATUS_INVALID_PARAMETER;
	}

	if (NULL == LengthReturned)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocAesECBDecrypt->LengthReturned is NULL.\n"));
		return STATUS_INVALID_PARAMETER;
	}


	NTSTATUS Status = STATUS_UNSUCCESSFUL;


	Status = BCryptDecrypt(AesInitVar.hKey, (PUCHAR)InBuffer, (ULONG)InBufferSize,
		NULL, NULL, 0, (PUCHAR)InOutBuffer, *LengthReturned, LengthReturned, 0);

	if (STATUS_SUCCESS != Status)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocAesECBDecrypt->BCryptDecrypt decrypt ciphertext failed. Status = 0x%x\n", Status));
	}

	return Status;
}


NTSTATUS PocAesECBEncrypt_CiphertextStealing(
	IN PCHAR InBuffer, 
	IN ULONG InBufferSize, 
	IN OUT PCHAR InOutBuffer)
{
	if (!AesInitVar.Flag)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocAesECBEncrypt_CiphertextStealing->AesInitVar.Flag = FALSE.\n"));
		return POC_STATUS_AES_INIT_FAILED;
	}

	if (NULL == InBuffer)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocAesECBEncrypt_CiphertextStealing->InBuffer is NULL.\n"));
		return STATUS_INVALID_PARAMETER;
	}

	if (NULL == InOutBuffer)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocAesECBEncrypt_CiphertextStealing->InOutBuffer is NULL.\n"));
		return STATUS_INVALID_PARAMETER;
	}

	if (InBufferSize % AES_BLOCK_SIZE == 0 || InBufferSize < AES_BLOCK_SIZE)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocAesECBEncrypt_CiphertextStealing->Buffer is aligned with block size.\n"));
		return STATUS_UNSUCCESSFUL;
	}


	NTSTATUS Status = STATUS_UNSUCCESSFUL;

	ULONG TailLength = InBufferSize % AES_BLOCK_SIZE;
	ULONG LengthReturned = 0;
	ULONG Pn_1Offset = 0, PnOffset = 0;

	CHAR Pn[AES_BLOCK_SIZE] = { 0 };
	CHAR Cn_1[AES_BLOCK_SIZE] = { 0 };
	CHAR Cpadding[AES_BLOCK_SIZE] = { 0 };

	PCHAR AlignedBuffer = NULL;

	AlignedBuffer = ExAllocatePoolWithTag(NonPagedPool, (SIZE_T)InBufferSize - (SIZE_T)TailLength, WRITE_BUFFER_TAG);

	if (NULL == AlignedBuffer)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocAesECBEncrypt_CiphertextStealing->ExAllocatePoolWithTag AlignedBuffer failed.\\n"));
		Status = STATUS_UNSUCCESSFUL;
		goto EXIT;
	}

	RtlZeroMemory(AlignedBuffer, InBufferSize - TailLength);

	RtlMoveMemory(AlignedBuffer, InBuffer, InBufferSize - TailLength);

	LengthReturned = InBufferSize - TailLength;
	Status = PocAesECBEncrypt(
		AlignedBuffer, 
		InBufferSize - TailLength, 
		InOutBuffer, 
		&LengthReturned);

	if (STATUS_SUCCESS != Status)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocAesECBEncrypt_CiphertextStealing->PocAesECBEncrypt1 failed. Status = 0x%x\n", Status));
		goto EXIT;
	}

	Pn_1Offset = InBufferSize - TailLength - AES_BLOCK_SIZE;
	PnOffset = Pn_1Offset + AES_BLOCK_SIZE;

	//InOutBuffer + Pn_1Offset == Cn
	RtlMoveMemory(InOutBuffer + PnOffset, InOutBuffer + Pn_1Offset, TailLength);

	RtlMoveMemory(Cpadding, InOutBuffer + Pn_1Offset + TailLength, AES_BLOCK_SIZE - TailLength);

	RtlZeroMemory(InOutBuffer + Pn_1Offset, AES_BLOCK_SIZE);

	

	RtlMoveMemory(Pn, InBuffer + PnOffset, TailLength);
	RtlMoveMemory(Pn + TailLength, Cpadding, AES_BLOCK_SIZE - TailLength);

	LengthReturned = AES_BLOCK_SIZE;
	Status = PocAesECBEncrypt(
		Pn,
		AES_BLOCK_SIZE,
		Cn_1,
		&LengthReturned);

	if (STATUS_SUCCESS != Status)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocAesECBEncrypt_CiphertextStealing->PocAesECBEncrypt2 failed. Status = 0x%x\n", Status));
		goto EXIT;
	}

	RtlMoveMemory(InOutBuffer + Pn_1Offset, Cn_1, AES_BLOCK_SIZE);

	Status = STATUS_SUCCESS;

EXIT:

	if (NULL != AlignedBuffer)
	{
		ExFreePoolWithTag(AlignedBuffer, WRITE_BUFFER_TAG);
		AlignedBuffer = NULL;
	}

	return Status;
}


NTSTATUS PocAesECBDecrypt_CiphertextStealing(
	IN PCHAR InBuffer,
	IN ULONG InBufferSize,
	IN OUT PCHAR InOutBuffer)
{
	if (!AesInitVar.Flag)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocAesECBDecrypt_CiphertextStealing->AesInitVar.Flag = FALSE.\n"));
		return POC_STATUS_AES_INIT_FAILED;
	}

	if (NULL == InBuffer)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocAesECBDecrypt_CiphertextStealing->InBuffer is NULL.\n"));
		return STATUS_INVALID_PARAMETER;
	}

	if (NULL == InOutBuffer)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocAesECBDecrypt_CiphertextStealing->InOutBuffer is NULL.\n"));
		return STATUS_INVALID_PARAMETER;
	}

	if (InBufferSize % AES_BLOCK_SIZE == 0 || InBufferSize < AES_BLOCK_SIZE)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocAesECBDecrypt_CiphertextStealing->Buffer is aligned with block size.\n"));
		return STATUS_UNSUCCESSFUL;
	}


	NTSTATUS Status = STATUS_UNSUCCESSFUL;

	ULONG TailLength = InBufferSize % AES_BLOCK_SIZE;
	ULONG LengthReturned = 0;
	ULONG Cn_1Offset = 0, CnOffset = 0;

	CHAR Cn[AES_BLOCK_SIZE] = { 0 };
	CHAR Pn_1[AES_BLOCK_SIZE] = { 0 };
	CHAR Cpadding[AES_BLOCK_SIZE] = { 0 };

	PCHAR AlignedBuffer = NULL;

	AlignedBuffer = ExAllocatePoolWithTag(NonPagedPool, (SIZE_T)InBufferSize - (SIZE_T)TailLength, READ_BUFFER_TAG);

	if (NULL == AlignedBuffer)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocAesECBDecrypt_CiphertextStealing->ExAllocatePoolWithTag AlignedBuffer failed.\n"));
		Status = STATUS_UNSUCCESSFUL;
		goto EXIT;
	}

	RtlZeroMemory(AlignedBuffer, InBufferSize - TailLength);

	RtlMoveMemory(AlignedBuffer, InBuffer, InBufferSize - TailLength);

	LengthReturned = InBufferSize - TailLength;
	Status = PocAesECBDecrypt(
		AlignedBuffer,
		InBufferSize - TailLength,
		InOutBuffer,
		&LengthReturned);

	if (STATUS_SUCCESS != Status)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocAesECBDecrypt_CiphertextStealing->PocAesECBDecrypt1 failed. Status = 0x%x\n", Status));
		goto EXIT;
	}

	Cn_1Offset = InBufferSize - TailLength - AES_BLOCK_SIZE;
	CnOffset = Cn_1Offset + AES_BLOCK_SIZE;

	//InOutBuffer + Cn_1Offset == Pn
	RtlMoveMemory(InOutBuffer + CnOffset, InOutBuffer + Cn_1Offset, TailLength);

	RtlMoveMemory(Cpadding, InOutBuffer + Cn_1Offset + TailLength, AES_BLOCK_SIZE - TailLength);

	RtlZeroMemory(InOutBuffer + Cn_1Offset, AES_BLOCK_SIZE);



	RtlMoveMemory(Cn, InBuffer + CnOffset, TailLength);
	RtlMoveMemory(Cn + TailLength, Cpadding, AES_BLOCK_SIZE - TailLength);

	LengthReturned = AES_BLOCK_SIZE;
	Status = PocAesECBDecrypt(
		Cn,
		AES_BLOCK_SIZE,
		Pn_1,
		&LengthReturned);

	if (STATUS_SUCCESS != Status)
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("PocAesECBDecrypt_CiphertextStealing->PocAesECBDecrypt2 failed. Status = 0x%x\n", Status));
		goto EXIT;
	}

	RtlMoveMemory(InOutBuffer + Cn_1Offset, Pn_1, AES_BLOCK_SIZE);

	Status = STATUS_SUCCESS;

EXIT:

	if (NULL != AlignedBuffer)
	{
		ExFreePoolWithTag(AlignedBuffer, READ_BUFFER_TAG);
		AlignedBuffer = NULL;
	}

	return Status;
}


NTSTATUS PocComputeHash(
	IN PUCHAR Data, 
	IN ULONG DataLength, 
	IN OUT PUCHAR* DataDigestPointer, 
	IN OUT ULONG* DataDigestLengthPointer)
{

	NTSTATUS Status = 0;

	BCRYPT_ALG_HANDLE HashAlgHandle = NULL;
	BCRYPT_HASH_HANDLE HashHandle = NULL;

	PUCHAR HashDigest = NULL;
	ULONG HashDigestLength = 0;

	ULONG ResultLength = 0;

	*DataDigestPointer = NULL;
	*DataDigestLengthPointer = 0;



	Status = BCryptOpenAlgorithmProvider(
		&HashAlgHandle,
		BCRYPT_SHA256_ALGORITHM,
		NULL,
		0);
	if (!NT_SUCCESS(Status))
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
			("%s->BCryptOpenAlgorithmProvider failed. Status = 0x%x.\n", __FUNCTION__, Status));
		goto cleanup;
	}




	Status = BCryptGetProperty(
		HashAlgHandle,
		BCRYPT_HASH_LENGTH,
		(PUCHAR)&HashDigestLength,
		sizeof(HashDigestLength),
		&ResultLength,
		0);
	if (!NT_SUCCESS(Status))
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
			("%s->BCryptGetProperty failed. Status = 0x%x.\n", __FUNCTION__, Status));
		goto cleanup;
	}


	HashDigest = (PUCHAR)ExAllocatePoolWithTag(PagedPool, HashDigestLength, READ_BUFFER_TAG);

	if (NULL == HashDigest)
	{
		Status = STATUS_NO_MEMORY;
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
			("%s->ExAllocatePoolWithTag failed. Status = 0x%x.\n", __FUNCTION__, Status));
		goto cleanup;
	}

	RtlZeroMemory(HashDigest, HashDigestLength);



	Status = BCryptCreateHash(
		HashAlgHandle,
		&HashHandle,
		NULL,
		0,
		NULL,
		0,
		0);
	if (!NT_SUCCESS(Status))
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
			("%s->BCryptCreateHash failed. Status = 0x%x.\n", __FUNCTION__, Status));
		goto cleanup;
	}



	Status = BCryptHashData(
		HashHandle,
		(PUCHAR)Data,
		DataLength,
		0);
	if (!NT_SUCCESS(Status))
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
			("%s->BCryptHashData failed. Status = 0x%x.\n", __FUNCTION__, Status));
		goto cleanup;
	}



	Status = BCryptFinishHash(
		HashHandle,
		HashDigest,
		HashDigestLength,
		0);
	if (!NT_SUCCESS(Status))
	{
		PT_DBG_PRINT(PTDBG_TRACE_ROUTINES,
			("%s->BCryptFinishHash failed. Status = 0x%x.\n", __FUNCTION__, Status));
		goto cleanup;
	}

	*DataDigestPointer = HashDigest;
	HashDigest = NULL;
	*DataDigestLengthPointer = HashDigestLength;

	Status = STATUS_SUCCESS;

cleanup:

	if (NULL != HashDigest)
	{
		ExFreePool(HashDigest);
		HashDigest = NULL;
	}

	if (NULL != HashHandle)
	{
		Status = BCryptDestroyHash(HashHandle);
		HashHandle = NULL;
	}

	if (NULL != HashAlgHandle)
	{
		BCryptCloseAlgorithmProvider(HashAlgHandle, 0);
		HashAlgHandle = NULL;
	}

	return Status;
}


/**
 * @Author: wangzhankun
 * @Date: 2022-06-22 20:22:32
 * @LastEditors: wangzhankun
 * @update:
 * @brief �� read_buffer ���� AES ���ܣ��������ܺ������д�뵽 write_buffer �С�
 * ����� read_buffer �ĳ��Ⱥ�file_size�ĳ����Զ�ѡ����Ӧ�Ľ��ܺ�������padding���ܻ�������Ų�ý��ܵȣ���
 * �����Ҫ����Ų�õĻ����� file_size > AES_BLOCK_SIZE �� file_size % AES_BLOCK_SIZE !=0 �� ��ǰread_buffer�е����ݾ����ļ�����ĩβ�����ݣ���
 * �Ǿ���Ҫ��֤ bytesRead > AES_BLOCK_SIZE������ͻ���������޷���������Ų�õ��µĴ���
 * @param [in] {PCHAR} read_buffer �����ܵ�����
 * @param [in] {ULONG} bytesRead ������ǽ��ܺ�����ĵ���ʵ���ȡ�����ĳ�����ָ���ĵĳ��ȡ�
 * ���� ������ĳ���ֻ��18���ֽڣ����Ǽ��ܺ���� 2 * AES_BLOCK_SIZE �ĳ��ȣ���ô��ʱ bytesRead ���� 18�ֽڡ�
 * @param [in] {PCHAR} write_buffer ���ܺ������
 * @param [out] {ULONG*} �������write_buffer�������Ĵ�С����������bytesWrite ���ܺ�����ĳ��ȡ���������˴������file_size == 0����ô bytesWrite������ֵΪ0
 * @param [in] {ULONG} file_size �ļ��ĳ��ȡ�Ҫ�����ļ���ʵ�����ݣ�����ǰ���Ĵ�С�����ܰ����ļ���ʶβ�ĳ��ȡ�
 * ��Ϊ�ļ����ܺ�ĳ��ȿ϶��� AES_BLOCK_SIZE ������������˲�����ʹ�ü��ܺ���ļ���С������ʵ�ʵļ���ǰ���ļ���С��
 * @return {NTSTATUS} STATUS_SUCCESS if successfule
 */
NTSTATUS PocManualDecrypt(PCHAR read_buffer,
								 IN ULONG bytesRead,
								 IN OUT PCHAR write_buffer,
								 OUT ULONG *bytesWrite,
								 IN const LONGLONG file_size)
{
	NTSTATUS Status = STATUS_SUCCESS;

	PCHAR cipher_text = read_buffer;
	PCHAR plain_text = write_buffer;
	if (file_size < AES_BLOCK_SIZE) //����ʹ��file_size�����ж�
	{
		// ��˼��ܺ���ļ����ٻ��� AES_BLOCK_SIZE �Ĵ�С���������������޸ġ�
		bytesRead = AES_BLOCK_SIZE;
		Status = PocAesECBDecrypt(cipher_text, bytesRead, plain_text, bytesWrite);
		if (STATUS_SUCCESS != Status)
		{
			PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s@%s@%d PocAesECBDecrypt failed: 0x%x\n", __FUNCTION__, __FILE__, __LINE__, Status));
			goto EXIT;
		}
		*bytesWrite = (ULONG)file_size;
	}
	else if (bytesRead % AES_BLOCK_SIZE == 0) //����ʹ��bytesRead�����ж�
	{
		Status = PocAesECBDecrypt(cipher_text, bytesRead, plain_text, bytesWrite);
		if (STATUS_SUCCESS != Status)
		{
			PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s@%s@%d PocAesECBDecrypt failed: 0x%x\n", __FUNCTION__, __FILE__, __LINE__, Status));
			goto EXIT;
		}
	}
	else
	{
		// bytesRead �������⴦����֤ bytesRead > AES_BLOCK_SIZE������������Ų�õĽ��ܲ���

		// ��Ҫ��������Ų�ã��Ҵ�ʱ������һ�������һ�ζ�ȡ��������
		// Status = PocAesECBDecrypt_CiphertextStealing(cipher_text, bytesRead, plain_text);
		bytesRead = ROUND_TO_SIZE(bytesRead, AES_BLOCK_SIZE);
		// *bytesWrite = ROUND_TO_SIZE(*bytesWrite, AES_BLOCK_SIZE);
		Status = PocAesECBDecrypt(cipher_text, bytesRead, plain_text, bytesWrite);

		if (STATUS_SUCCESS != Status)
		{
			PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s@%s@%d PocAesECBDecrypt_CiphertextStealing failed: 0x%x\n", __FUNCTION__, __FILE__, __LINE__, Status));
			goto EXIT;
		}
		*bytesWrite = bytesRead;
	}

EXIT:
	if (STATUS_SUCCESS != Status)
	{
		*bytesWrite = 0;
	}
	return Status;
}

/**
 * @Author: wangzhankun
 * @Date: 2022-06-22 20:22:32
 * @LastEditors: wangzhankun
 * @update:
 * @brief �� read_buffer ���� AES ���ܣ��������ܺ������д�뵽 write_buffer �С�
 * ����� read_buffer �ĳ��Ⱥ� file_size �ĳ����Զ�ѡ���Ƿ�Խ��� padding ��������Ų�� ���߲������⴦��
 * �����Ҫ����Ų�õĻ����� file_size > AES_BLOCK_SIZE �� file_size % AES_BLOCK_SIZE !=0 �� ��ǰread_buffer�е����ݾ����ļ������Ҫ����д������ݣ���
 * �Ǿ���Ҫ��֤ bytesRead > AES_BLOCK_SIZE������������޷���������Ų�ö����ִ���
 * @param [in] {PCHAR} read_buffer �����ܵ�����
 * @param [in] {ULONG} bytesRead �����ܵ����ݵĳ��ȡ����ܺ�����ݵĳ���Ҳ���ɴ˴�����
 * �ڲ�ʹ�õ���ECB����˼��ܺ�����ݳ��Ⱦ��� bytesREAD ���� AES_BLOCK_SIZE ȡ����ĳ��ȡ�
 * @param [in] {PCHAR} write_buffer ���ܺ������
 * @param [in|out] {ULONG*} �������write_buffer�������Ĵ�С����������bytesWrite ���ܺ�����ݵĳ��ȣ��϶��� AES_BLOCK_SIZE ������������������˴������file_size == 0����ô bytesWrite������ֵΪ0
 * @param [in] {ULONG} file_size �ļ��ĳ��ȡ�Ҫ�����ļ���ʵ�����ݣ�����ǰ���Ĵ�С�����ܰ����ļ���ʶβ�ĳ��ȡ�
 * @return {NTSTATUS} STATUS_SUCCESS if successfule
 */
NTSTATUS PocManualEncrypt(PCHAR read_buffer,
								 IN ULONG bytesRead,
								 IN OUT PCHAR write_buffer,
								 OUT ULONG *bytesWrite,
								 IN const LONGLONG file_size)
{
	NTSTATUS Status = STATUS_SUCCESS;

	PCHAR plain_text = read_buffer;
	PCHAR cipher_text = write_buffer;
	if (file_size < AES_BLOCK_SIZE) //����ʹ��file_size�����ж�
	{
		bytesRead = AES_BLOCK_SIZE; // padding

		Status = PocAesECBEncrypt(plain_text, bytesRead, cipher_text, bytesWrite); // bytesRead ���Զ������ܺ�������Ϊ���ĵĴ�С
		if (STATUS_SUCCESS != Status)
		{
			PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s@%s@%d PocAesECBEncrypt failed: 0x%x\n", __FUNCTION__, __FILE__, __LINE__, Status));
			goto EXIT;
		}
	}
	else if (bytesRead % AES_BLOCK_SIZE == 0) //����ʹ��bytesRead�����ж�
	{
		Status = PocAesECBEncrypt(plain_text, bytesRead, cipher_text, bytesWrite);
		if (STATUS_SUCCESS != Status)
		{
			PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s@%s@%d PocAesECBEncrypt failed: 0x%x\n", __FUNCTION__, __FILE__, __LINE__, Status));
			goto EXIT;
		}
	}
	else
	{
		// bytesRead �������⴦����֤ bytesRead > AES_BLOCK_SIZE������������Ų��

		// ��Ҫ��������Ų�ã��Ҵ�ʱ������һ�������һ�ζ�ȡ��������
		// Status = PocAesECBEncrypt_CiphertextStealing(plain_text, bytesRead, cipher_text);
		bytesRead = ROUND_TO_SIZE(bytesRead, AES_BLOCK_SIZE);
		// *bytesWrite = ROUND_TO_SIZE(*bytesWrite, AES_BLOCK_SIZE);
		Status = PocAesECBEncrypt(plain_text, bytesRead, cipher_text, bytesWrite);

		if (STATUS_SUCCESS != Status)
		{
			PT_DBG_PRINT(PTDBG_TRACE_ROUTINES, ("%s@%s@%d PocAesECBEncrypt_CiphertextStealing failed: 0x%x\n", __FUNCTION__, __FILE__, __LINE__, Status));
			goto EXIT;
		}
		*bytesWrite = ROUND_TO_SIZE(bytesRead, AES_BLOCK_SIZE);
	}

EXIT:
	if (STATUS_SUCCESS != Status)
	{
		*bytesWrite = 0;
	}
	return Status;
}