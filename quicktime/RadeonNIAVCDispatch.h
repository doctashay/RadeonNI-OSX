/*
 * The selector table of the image decompressor, in the form ComponentDispatchHelper.c (from the
 * QuickTime SDK) includes twice: once to build the tables, once to build the range table. Every
 * selector of a range has an entry, in order: ComponentCall for what this component implements,
 * ComponentDelegate (the base image decompressor answers) for the rest. Selectors are in
 * <QuickTime/ImageCodec.h> (kImageCodec...Select) and <CarbonCore/Components.h>.
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

ComponentSelectorOffset (6)
ComponentRangeCount (3)
ComponentRangeShift (8)
ComponentRangeMask (FF)

/* the standard selectors, -6 to -1 */
ComponentRangeBegin (0)
	StdComponentCall (Target)
	StdComponentCall (Register)
	StdComponentCall (Version)
	StdComponentCall (CanDo)
	StdComponentCall (Close)
	StdComponentCall (Open)
ComponentRangeEnd (0)

/* 0x000 to 0x03C: the base image codec calls (GetCodecInfo at 0, the rest left to the base decompressor) */
ComponentRangeBegin (1)
	ComponentCall (GetCodecInfo)				/* 0x00 */
	ComponentDelegate (GetCompressionTime)			/* 0x01 */
	ComponentDelegate (GetMaxCompressionSize)		/* 0x02 */
	ComponentDelegate (PreCompress)				/* 0x03 */
	ComponentDelegate (BandCompress)			/* 0x04 */
	ComponentDelegate (PreDecompress)			/* 0x05 */
	ComponentDelegate (BandDecompress)			/* 0x06 */
	ComponentDelegate (Busy)				/* 0x07 */
	ComponentDelegate (GetCompressedImageSize)		/* 0x08 */
	ComponentDelegate (GetSimilarity)			/* 0x09 */
	ComponentDelegate (TrimImage)				/* 0x0A */
	ComponentDelegate (RequestSettings)			/* 0x0B */
	ComponentDelegate (GetSettings)				/* 0x0C */
	ComponentDelegate (SetSettings)				/* 0x0D */
	ComponentDelegate (Flush)				/* 0x0E */
	ComponentDelegate (SetTimeCode)				/* 0x0F */
	ComponentDelegate (IsImageDescriptionEquivalent)	/* 0x10 */
	ComponentDelegate (NewMemory)				/* 0x11 */
	ComponentDelegate (DisposeMemory)			/* 0x12 */
	ComponentDelegate (HitTestData)				/* 0x13 */
	ComponentDelegate (NewImageBufferMemory)		/* 0x14 */
	ComponentDelegate (ExtractAndCombineFields)		/* 0x15 */
	ComponentDelegate (GetMaxCompressionSizeWithSources)	/* 0x16 */
	ComponentDelegate (SetTimeBase)				/* 0x17 */
	ComponentDelegate (SourceChanged)			/* 0x18 */
	ComponentDelegate (FlushFrame)				/* 0x19 */
	ComponentDelegate (GetSettingsAsText)			/* 0x1A */
	ComponentDelegate (GetParameterListHandle)		/* 0x1B */
	ComponentDelegate (GetParameterList)			/* 0x1C */
	ComponentDelegate (CreateStandardParameterDialog)	/* 0x1D */
	ComponentDelegate (IsStandardParameterDialogEvent)	/* 0x1E */
	ComponentDelegate (DismissStandardParameterDialog)	/* 0x1F */
	ComponentDelegate (StandardParameterDialogDoAction)	/* 0x20 */
	ComponentDelegate (NewImageGWorld)			/* 0x21 */
	ComponentDelegate (DisposeImageGWorld)			/* 0x22 */
	ComponentDelegate (HitTestDataWithFlags)		/* 0x23 */
	ComponentDelegate (ValidateParameters)			/* 0x24 */
	ComponentDelegate (GetBaseMPWorkFunction)		/* 0x25 */
	ComponentDelegate (LockBits)				/* 0x26 */
	ComponentDelegate (UnlockBits)				/* 0x27 */
	ComponentDelegate (RequestGammaLevel)			/* 0x28 */
	ComponentDelegate (GetSourceDataGammaLevel)		/* 0x29 */
	ComponentDelegate (Unused2A)				/* 0x2A */
	ComponentCall (GetDecompressLatency)			/* 0x2B */
	ComponentDelegate (MergeFloatingImageOntoWindow)	/* 0x2C */
	ComponentDelegate (RemoveFloatingImage)			/* 0x2D */
	ComponentDelegate (GetDITLForSize)			/* 0x2E */
	ComponentDelegate (DITLInstall)				/* 0x2F */
	ComponentDelegate (DITLEvent)				/* 0x30 */
	ComponentDelegate (DITLItem)				/* 0x31 */
	ComponentDelegate (DITLRemove)				/* 0x32 */
	ComponentDelegate (DITLValidateInput)			/* 0x33 */
	ComponentDelegate (Unused34)				/* 0x34 */
	ComponentDelegate (Unused35)				/* 0x35 */
	ComponentDelegate (GetPreferredChunkSizeAndAlignment)	/* 0x36 */
	ComponentDelegate (PrepareToCompressFrames)		/* 0x37 */
	ComponentDelegate (EncodeFrame)				/* 0x38 */
	ComponentDelegate (CompleteFrame)			/* 0x39 */
	ComponentDelegate (BeginPass)				/* 0x3A */
	ComponentDelegate (EndPass)				/* 0x3B */
	ComponentDelegate (ProcessBetweenPasses)		/* 0x3C */
ComponentRangeEnd (1)

ComponentRangeUnused (2)

/* 0x200 to 0x20F: the sub-codec calls the decompressor implements */
ComponentRangeBegin (3)
	ComponentCall (Preflight)				/* 0x200 */
	ComponentCall (Initialize)				/* 0x201 */
	ComponentCall (BeginBand)				/* 0x202 */
	ComponentCall (DrawBand)				/* 0x203 */
	ComponentCall (EndBand)					/* 0x204 */
	ComponentCall (QueueStarting)				/* 0x205 */
	ComponentCall (QueueStopping)				/* 0x206 */
	ComponentDelegate (DroppingFrame)			/* 0x207 */
	ComponentDelegate (ScheduleFrame)			/* 0x208 */
	ComponentDelegate (CancelTrigger)			/* 0x209 */
	ComponentDelegate (Unused20A)				/* 0x20A */
	ComponentDelegate (Unused20B)				/* 0x20B */
	ComponentDelegate (Unused20C)				/* 0x20C */
	ComponentDelegate (Unused20D)				/* 0x20D */
	ComponentDelegate (Unused20E)				/* 0x20E */
	ComponentCall (DecodeBand)				/* 0x20F */
ComponentRangeEnd (3)
