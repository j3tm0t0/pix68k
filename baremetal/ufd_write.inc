
// ---- pix68k (#386) ----
// Pi 4 / Pi 5 のビルドで、tools/circle_ufd_patch.sh が usbfloppydevice.cpp の終わりに足す。
// 元の Write()、TryWrite()、Command() は読み取り専用のままで、WRITE(10) を送れるのはこの関数だけ。
// 送ってよいコマンドは platform::UfiGate が決め、pix68k.cfg の experimental_usb_fdd_write= で許していなければ何も送らない。失敗した段階を返し、書き直しとリセットはしない
boolean CUSBFloppyDiskDevice::Pix68kArmWrite (u32 nBlock, u32 nCount)
{
	if (   !platform::UfiGate::enabled ((int) m_nDeviceNumber - 1)
	    || m_nBlockShift != 10
	    || GetInterfaceProtocol () != 0
	    || !m_Pix68kGate.arm ((int) m_nDeviceNumber - 1, nBlock, nCount))
	{
		return FALSE;
	}

	m_bPix68kArmed = TRUE;

	return TRUE;
}

// 返す値: 0 = 終わった (pStatus に割り込みの 2 バイト、*pBytes に転送した数)、1 = 送っていない、2 = コマンドを断られた、3 = 転送の失敗
int CUSBFloppyDiskDevice::Pix68kCommand (const u8 *pCmdBlk, void *pBuffer, size_t nBufLen, boolean bIn,
					 u8 *pStatus, int *pBytes)
{
	PIX68K_LONG_TIMEOUT;

	pStatus[0] = pStatus[1] = 0;
	*pBytes = 0;

	// 転送が時間切れになったあとも、USB のコントローラがバッファを使うことがあるので、装置ごとに持ち、解放しない
	// 終わりを割り込みで知らせる装置 (CBI、プロトコル 0) だけ。ほかの装置は、書けたかどうかが分からない
	if (   !platform::UfiGate::enabled ((int) m_nDeviceNumber - 1)
	    || m_bPix68kBroken
	    || GetInterfaceProtocol () != 0
	    || m_pEndpointInterrupt == 0
	    || !m_Pix68kGate.allow ((int) m_nDeviceNumber - 1, pCmdBlk, nBufLen, !!bIn)
	    || nBufLen > platform::UfiGate::MAX_BLOCKS * platform::UfiGate::BLOCK_BYTES)
	{
		return 1;
	}

	if (m_pPix68kBuffer == 0)
	{
		m_pPix68kBuffer = new (HEAP_DMA30) u8[platform::UfiGate::MAX_BLOCKS * platform::UfiGate::BLOCK_BYTES];
		if (m_pPix68kBuffer == 0)
		{
			return 1;
		}
	}

	// コマンド (12 バイト) と、64 バイト目から割り込みの 2 バイト
	if (m_pPix68kSmall == 0)
	{
		m_pPix68kSmall = new (HEAP_DMA30) u8[128];
		if (m_pPix68kSmall == 0)
		{
			return 1;
		}
	}

	CUSBHostController *pHost = GetHost ();
	assert (pHost != 0);

	memcpy (m_pPix68kSmall, pCmdBlk, 12);

	// 制御転送は、断られた (STALL) のか、ほかの失敗 (時間切れ、抜去) なのかを見分けるため、自分で出す
	// (SETUP の 8 バイトは TRB の中に入るので、Setup は待ち終わったあとで使われない)
	TSetupData Setup;
	Setup.bmRequestType = REQUEST_OUT | REQUEST_CLASS | REQUEST_TO_INTERFACE;
	Setup.bRequest = 0;
	Setup.wValue = 0;
	Setup.wIndex = GetInterfaceNumber ();
	Setup.wLength = 12;
	CUSBRequest URB (GetEndpoint0 (), m_pPix68kSmall, 12, &Setup);
	if (!pHost->SubmitBlockingRequest (&URB))
	{
		// まだ一度も書き込みを許していなければ (開いている途中など)、STALL のときだけ、次のコマンドのために入口を戻す。
		// 書き込みを許したあとは、戻したことを確かめられないので (ResetFromHalted() は xHCI の命令の失敗も成功と返す)、
		// 使うのをやめる
		if (   !m_bPix68kArmed
		    && URB.GetUSBError () == USBErrorStall
		    && GetEndpoint0 ()->GetXHCIEndpoint ()->ResetFromHalted ())
		{
			return 2;
		}

		m_bPix68kBroken = TRUE;

		return 3;
	}

	if (nBufLen > 0)
	{
		int nResult = -1;
		if (bIn)
		{
			CUSBRequest DataURB (m_pEndpointIn, m_pPix68kBuffer, nBufLen);
			if (pHost->SubmitBlockingRequest (&DataURB))
			{
				nResult = DataURB.GetResultLength ();
			}
			// 読めないセクタを、データの転送の STALL で知らせるドライブ。まだ一度も書き込みを許していなければ、
			// 入口を戻して、センスを読みに行く (読めないセクタのあるディスクの、読めるところを読むため)。
			// 割り込みの 2 バイトは待たない
			else if (   !m_bPix68kArmed
				 && DataURB.GetUSBError () == USBErrorStall
				 && pHost->ControlMessage (GetEndpoint0 (), REQUEST_TO_ENDPOINT | REQUEST_OUT, CLEAR_FEATURE,
							   ENDPOINT_HALT, m_pEndpointIn->GetNumber () | 0x80, 0, 0) >= 0
				 && m_pEndpointIn->GetXHCIEndpoint ()->ResetFromHalted ())
			{
				return 2;
			}
		}
		else
		{
			memcpy (m_pPix68kBuffer, pBuffer, nBufLen);
			nResult = pHost->Transfer (m_pEndpointOut, m_pPix68kBuffer, nBufLen);
		}

		if (nResult < 0)
		{
			m_bPix68kBroken = TRUE;

			return 3;
		}

		if (bIn)
		{
			memcpy (pBuffer, m_pPix68kBuffer, nResult);
		}

		*pBytes = nResult;
	}

	if (pHost->Transfer (m_pEndpointInterrupt, m_pPix68kSmall + 64, 2) != 2)
	{
		m_bPix68kBroken = TRUE;

		return 3;
	}

	pStatus[0] = m_pPix68kSmall[64];
	pStatus[1] = m_pPix68kSmall[65];

	return 0;
}
