/** @file
  SMI handler profile support.

Copyright (c) 2017, Intel Corporation. All rights reserved.<BR>
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include <PiSmm.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/SmmServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>
#include <Library/DevicePathLib.h>
#include <Library/PeCoffGetEntryPointLib.h>
#include <Library/DxeServicesLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SmmAccess2.h>
#include <Protocol/SmmReadyToLock.h>
#include <Protocol/SmmEndOfDxe.h>

#include <Guid/SmiHandlerProfile.h>

#include "PiSmmCore.h"

typedef struct {
  EFI_GUID FileGuid;
  UINTN    ImageRef;
  UINTN    EntryPoint;
  UINTN    ImageBase;
  UINTN    ImageSize;
  UINTN    PdbStringSize;
  CHAR8    *PdbString;
} IMAGE_STRUCT;

/**
  Register SMI handler profile handler.
**/
VOID
RegisterSmiHandlerProfileHandler(
  VOID
  );

/**
  Retrieves and returns a pointer to the entry point to a PE/COFF image that has been loaded
  into system memory with the PE/COFF Loader Library functions.

  Retrieves the entry point to the PE/COFF image specified by Pe32Data and returns this entry
  point in EntryPoint.  If the entry point could not be retrieved from the PE/COFF image, then
  return RETURN_INVALID_PARAMETER.  Otherwise return RETURN_SUCCESS.
  If Pe32Data is NULL, then ASSERT().
  If EntryPoint is NULL, then ASSERT().

  @param  Pe32Data                  The pointer to the PE/COFF image that is loaded in system memory.
  @param  EntryPoint                The pointer to entry point to the PE/COFF image to return.

  @retval RETURN_SUCCESS            EntryPoint was returned.
  @retval RETURN_INVALID_PARAMETER  The entry point could not be found in the PE/COFF image.

**/
RETURN_STATUS
InternalPeCoffGetEntryPoint (
  IN  VOID  *Pe32Data,
  OUT VOID  **EntryPoint
  );

extern LIST_ENTRY  mSmiEntryList;
extern LIST_ENTRY  mHardwareSmiEntryList;
extern SMI_ENTRY   mRootSmiEntry;

extern SMI_HANDLER_PROFILE_PROTOCOL  mSmiHandlerProfile;

GLOBAL_REMOVE_IF_UNREFERENCED LIST_ENTRY      mHardwareSmiEntryList = INITIALIZE_LIST_HEAD_VARIABLE (mHardwareSmiEntryList);

GLOBAL_REMOVE_IF_UNREFERENCED LIST_ENTRY      mRootSmiEntryList = INITIALIZE_LIST_HEAD_VARIABLE (mRootSmiEntryList);

GLOBAL_REMOVE_IF_UNREFERENCED LIST_ENTRY      *mSmmCoreRootSmiEntryList = &mRootSmiEntryList;
GLOBAL_REMOVE_IF_UNREFERENCED LIST_ENTRY      *mSmmCoreSmiEntryList = &mSmiEntryList;
GLOBAL_REMOVE_IF_UNREFERENCED LIST_ENTRY      *mSmmCoreHardwareSmiEntryList = &mHardwareSmiEntryList;

GLOBAL_REMOVE_IF_UNREFERENCED IMAGE_STRUCT  *mImageStruct;
GLOBAL_REMOVE_IF_UNREFERENCED UINTN         mImageStructCountMax;
GLOBAL_REMOVE_IF_UNREFERENCED UINTN         mImageStructCount;

GLOBAL_REMOVE_IF_UNREFERENCED VOID   *mSmiHandlerProfileDatabase;
GLOBAL_REMOVE_IF_UNREFERENCED UINTN  mSmiHandlerProfileDatabaseSize;

GLOBAL_REMOVE_IF_UNREFERENCED UINTN  mSmmImageDatabaseSize;
GLOBAL_REMOVE_IF_UNREFERENCED UINTN  mSmmRootSmiDatabaseSize;
GLOBAL_REMOVE_IF_UNREFERENCED UINTN  mSmmSmiDatabaseSize;
GLOBAL_REMOVE_IF_UNREFERENCED UINTN  mSmmHardwareSmiDatabaseSize;

GLOBAL_REMOVE_IF_UNREFERENCED BOOLEAN  mSmiHandlerProfileRecordingStatus;

GLOBAL_REMOVE_IF_UNREFERENCED SMI_HANDLER_PROFILE_PROTOCOL  mSmiHandlerProfile = {
  SmiHandlerProfileRegisterHandler,
  SmiHandlerProfileUnregisterHandler,
};

/**
  This function dump raw data.

  @param  Data  raw data
  @param  Size  raw data size
**/
VOID
InternalDumpData (
  IN UINT8  *Data,
  IN UINTN  Size
  )
{
  UINTN  Index;
  for (Index = 0; Index < Size; Index++) {
    DEBUG ((DEBUG_INFO, "%02x ", (UINTN)Data[Index]));
  }
}

/**
  Get GUID name for an image.

  @param[in]  LoadedImage LoadedImage protocol.
  @param[out] Guid        Guid of the FFS
**/
VOID
GetDriverGuid (
  IN  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage,
  OUT EFI_GUID                   *Guid
  )
{
  EFI_GUID                    *FileName;

  FileName = NULL;
  if ((DevicePathType(LoadedImage->FilePath) == MEDIA_DEVICE_PATH) &&
      (DevicePathSubType(LoadedImage->FilePath) == MEDIA_PIWG_FW_FILE_DP)) {
    FileName = &((MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *)LoadedImage->FilePath)->FvFileName;
  }
  if (FileName != NULL) {
    CopyGuid(Guid, FileName);
  } else {
    ZeroMem(Guid, sizeof(EFI_GUID));
  }
}

/**
  Add image structure.

  @param  ImageBase         image base
  @param  ImageSize         image size
  @param  EntryPoint        image entry point
  @param  Guid              FFS GUID of the image
  @param  PdbString         image PDB string
**/
VOID
AddImageStruct(
  IN UINTN     ImageBase,
  IN UINTN     ImageSize,
  IN UINTN     EntryPoint,
  IN EFI_GUID  *Guid,
  IN CHAR8     *PdbString
  )
{
  UINTN  PdbStringSize;

  if (mImageStructCount >= mImageStructCountMax) {
    ASSERT(FALSE);
    return;
  }

  CopyGuid(&mImageStruct[mImageStructCount].FileGuid, Guid);
  mImageStruct[mImageStructCount].ImageRef = mImageStructCount;
  mImageStruct[mImageStructCount].ImageBase = ImageBase;
  mImageStruct[mImageStructCount].ImageSize = ImageSize;
  mImageStruct[mImageStructCount].EntryPoint = EntryPoint;
  if (PdbString != NULL) {
    PdbStringSize = AsciiStrSize(PdbString);
    mImageStruct[mImageStructCount].PdbString = AllocateCopyPool (PdbStringSize, PdbString);
    if (mImageStruct[mImageStructCount].PdbString != NULL) {
      mImageStruct[mImageStructCount].PdbStringSize = PdbStringSize;
    }
  }

  mImageStructCount++;
}

/**
  return an image structure based upon image address.

  @param  Address  image address

  @return image structure
**/
IMAGE_STRUCT *
AddressToImageStruct(
  IN UINTN  Address
  )
{
  UINTN  Index;

  for (Index = 0; Index < mImageStructCount; Index++) {
    if ((Address >= mImageStruct[Index].ImageBase) &&
        (Address < mImageStruct[Index].ImageBase + mImageStruct[Index].ImageSize)) {
      return &mImageStruct[Index];
    }
  }
  return NULL;
}

/**
  return an image reference index based upon image address.

  @param  Address  image address

  @return image reference index
**/
UINTN
AddressToImageRef(
  IN UINTN  Address
  )
{
  IMAGE_STRUCT *ImageStruct;

  ImageStruct = AddressToImageStruct(Address);
  if (ImageStruct != NULL) {
    return ImageStruct->ImageRef;
  }
  return (UINTN)-1;
}

/**
  Collect SMM image information based upon loaded image protocol.
**/
VOID
GetSmmLoadedImage(
  VOID
  )
{
  EFI_STATUS                 Status;
  UINTN                      NoHandles;
  UINTN                      HandleBufferSize;
  EFI_HANDLE                 *HandleBuffer;
  UINTN                      Index;
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;
  CHAR16                     *PathStr;
  EFI_SMM_DRIVER_ENTRY       *LoadedImagePrivate;
  UINTN                      EntryPoint;
  VOID                       *EntryPointInImage;
  EFI_GUID                   Guid;
  CHAR8                      *PdbString;
  UINTN                      RealImageBase;

  HandleBufferSize = 0;
  HandleBuffer = NULL;
  Status = gSmst->SmmLocateHandle(
                    ByProtocol,
                    &gEfiLoadedImageProtocolGuid,
                    NULL,
                    &HandleBufferSize,
                    HandleBuffer
                    );
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return;
  }
  HandleBuffer = AllocateZeroPool (HandleBufferSize);
  if (HandleBuffer == NULL) {
    return;
  }
  Status = gSmst->SmmLocateHandle(
                    ByProtocol,
                    &gEfiLoadedImageProtocolGuid,
                    NULL,
                    &HandleBufferSize,
                    HandleBuffer
                    );
  if (EFI_ERROR(Status)) {
    return;
  }

  NoHandles = HandleBufferSize/sizeof(EFI_HANDLE);
  mImageStructCountMax = NoHandles;
  mImageStruct = AllocateZeroPool(mImageStructCountMax * sizeof(IMAGE_STRUCT));
  if (mImageStruct == NULL) {
    goto Done;
  }

  for (Index = 0; Index < NoHandles; Index++) {
    Status = gSmst->SmmHandleProtocol(
                      HandleBuffer[Index],
                      &gEfiLoadedImageProtocolGuid,
                      (VOID **)&LoadedImage
                      );
    if (EFI_ERROR(Status)) {
      continue;
    }
    PathStr = ConvertDevicePathToText(LoadedImage->FilePath, TRUE, TRUE);
    GetDriverGuid(LoadedImage, &Guid);
    DEBUG ((DEBUG_INFO, "Image: %g ", &Guid));

    EntryPoint = 0;
    LoadedImagePrivate = BASE_CR(LoadedImage, EFI_SMM_DRIVER_ENTRY, SmmLoadedImage);
    RealImageBase = (UINTN)LoadedImage->ImageBase;
    if (LoadedImagePrivate->Signature == EFI_SMM_DRIVER_ENTRY_SIGNATURE) {
      EntryPoint = (UINTN)LoadedImagePrivate->ImageEntryPoint;
      if ((EntryPoint != 0) && ((EntryPoint < (UINTN)LoadedImage->ImageBase) || (EntryPoint >= ((UINTN)LoadedImage->ImageBase + (UINTN)LoadedImage->ImageSize)))) {
        //
        // If the EntryPoint is not in the range of image buffer, it should come from emulation environment.
        // So patch ImageBuffer here to align the EntryPoint.
        //
        Status = InternalPeCoffGetEntryPoint(LoadedImage->ImageBase, &EntryPointInImage);
        ASSERT_EFI_ERROR(Status);
        RealImageBase = (UINTN)LoadedImage->ImageBase + EntryPoint - (UINTN)EntryPointInImage;
      }
    }
    DEBUG ((DEBUG_INFO, "(0x%x - 0x%x", RealImageBase, (UINTN)LoadedImage->ImageSize));
    if (EntryPoint != 0) {
      DEBUG ((DEBUG_INFO, ", EntryPoint:0x%x", EntryPoint));
    }
    DEBUG ((DEBUG_INFO, ")\n"));

    if (RealImageBase != 0) {
      PdbString = PeCoffLoaderGetPdbPointer ((VOID*) (UINTN) RealImageBase);
      DEBUG ((DEBUG_INFO, "       pdb - %a\n", PdbString));
    } else {
      PdbString = NULL;
    }
    DEBUG ((DEBUG_INFO, "       (%s)\n", PathStr));

    AddImageStruct((UINTN)RealImageBase, (UINTN)LoadedImage->ImageSize, EntryPoint, &Guid, PdbString);
  }

Done:
  FreePool(HandleBuffer);
  return;
}

/**
  Dump SMI child context.

  @param HandlerType  the handler type
  @param Context      the handler context
  @param ContextSize  the handler context size
**/
VOID
DumpSmiChildContext (
  IN EFI_GUID   *HandlerType,
  IN VOID       *Context,
  IN UINTN      ContextSize
  )
{
  if (CompareGuid (HandlerType, &gEfiSmmSwDispatch2ProtocolGuid)) {
    DEBUG ((DEBUG_INFO, "  SwSmi - 0x%x\n", ((EFI_SMM_SW_REGISTER_CONTEXT *)Context)->SwSmiInputValue));
  } else if (CompareGuid (HandlerType, &gEfiSmmSxDispatch2ProtocolGuid)) {
    DEBUG ((DEBUG_INFO, "  SxType - 0x%x\n", ((EFI_SMM_SX_REGISTER_CONTEXT *)Context)->Type));
    DEBUG ((DEBUG_INFO, "  SxPhase - 0x%x\n", ((EFI_SMM_SX_REGISTER_CONTEXT *)Context)->Phase));
  } else if (CompareGuid (HandlerType, &gEfiSmmPowerButtonDispatch2ProtocolGuid)) {
    DEBUG ((DEBUG_INFO, "  PowerButtonPhase - 0x%x\n", ((EFI_SMM_POWER_BUTTON_REGISTER_CONTEXT *)Context)->Phase));
  } else if (CompareGuid (HandlerType, &gEfiSmmStandbyButtonDispatch2ProtocolGuid)) {
    DEBUG ((DEBUG_INFO, "  StandbyButtonPhase - 0x%x\n", ((EFI_SMM_STANDBY_BUTTON_REGISTER_CONTEXT *)Context)->Phase));
  } else if (CompareGuid (HandlerType, &gEfiSmmPeriodicTimerDispatch2ProtocolGuid)) {
    DEBUG ((DEBUG_INFO, "  PeriodicTimerPeriod - %ld\n", ((EFI_SMM_PERIODIC_TIMER_REGISTER_CONTEXT *)Context)->Period));
    DEBUG ((DEBUG_INFO, "  PeriodicTimerSmiTickInterval - %ld\n", ((EFI_SMM_PERIODIC_TIMER_REGISTER_CONTEXT *)Context)->SmiTickInterval));
  } else if (CompareGuid (HandlerType, &gEfiSmmGpiDispatch2ProtocolGuid)) {
    DEBUG ((DEBUG_INFO, "  GpiNum - 0x%lx\n", ((EFI_SMM_GPI_REGISTER_CONTEXT *)Context)->GpiNum));
  } else if (CompareGuid (HandlerType, &gEfiSmmIoTrapDispatch2ProtocolGuid)) {
    DEBUG ((DEBUG_INFO, "  IoTrapAddress - 0x%x\n", ((EFI_SMM_IO_TRAP_REGISTER_CONTEXT *)Context)->Address));
    DEBUG ((DEBUG_INFO, "  IoTrapLength - 0x%x\n", ((EFI_SMM_IO_TRAP_REGISTER_CONTEXT *)Context)->Length));
    DEBUG ((DEBUG_INFO, "  IoTrapType - 0x%x\n", ((EFI_SMM_IO_TRAP_REGISTER_CONTEXT *)Context)->Type));
  } else if (CompareGuid (HandlerType, &gEfiSmmUsbDispatch2ProtocolGuid)) {
    DEBUG ((DEBUG_INFO, "  UsbType - 0x%x\n", ((SMI_HANDLER_PROFILE_USB_REGISTER_CONTEXT *)Context)->Type));
    DEBUG ((DEBUG_INFO, "  UsbDevicePath - %s\n", ConvertDevicePathToText((EFI_DEVICE_PATH_PROTOCOL *)(((SMI_HANDLER_PROFILE_USB_REGISTER_CONTEXT *)Context) + 1), TRUE, TRUE)));
  } else {
    DEBUG ((DEBUG_INFO, "  Context - "));
    InternalDumpData (Context, ContextSize);
    DEBUG ((DEBUG_INFO, "\n"));
  }
}

/**
  Dump all SMI handlers associated with SmiEntry.

  @param SmiEntry  SMI entry.
**/
VOID
DumpSmiHandlerOnSmiEntry(
  IN SMI_ENTRY       *SmiEntry
  )
{
  LIST_ENTRY      *ListEntry;
  SMI_HANDLER     *SmiHandler;
  IMAGE_STRUCT    *ImageStruct;

  ListEntry = &SmiEntry->SmiHandlers;
  for (ListEntry = ListEntry->ForwardLink;
       ListEntry != &SmiEntry->SmiHandlers;
       ListEntry = ListEntry->ForwardLink) {
    SmiHandler = CR(ListEntry, SMI_HANDLER, Link, SMI_HANDLER_SIGNATURE);
    ImageStruct = AddressToImageStruct((UINTN)SmiHandler->Handler);
    if (ImageStruct != NULL) {
      DEBUG ((DEBUG_INFO, " Module - %g", &ImageStruct->FileGuid));
    }
    if ((ImageStruct != NULL) && (ImageStruct->PdbString[0] != 0)) {
      DEBUG ((DEBUG_INFO, " (Pdb - %a)", ImageStruct->PdbString));
    }
    DEBUG ((DEBUG_INFO, "\n"));
    if (SmiHandler->ContextSize != 0) {
      DumpSmiChildContext (&SmiEntry->HandlerType, SmiHandler->Context, SmiHandler->ContextSize);
    }
    DEBUG ((DEBUG_INFO, "  Handler - 0x%x", SmiHandler->Handler));
    if (ImageStruct != NULL) {
      DEBUG ((DEBUG_INFO, " <== RVA - 0x%x", (UINTN)SmiHandler->Handler - ImageStruct->ImageBase));
    }
    DEBUG ((DEBUG_INFO, "\n"));
    DEBUG ((DEBUG_INFO, "  CallerAddr - 0x%x", SmiHandler->CallerAddr));
    if (ImageStruct != NULL) {
      DEBUG ((DEBUG_INFO, " <== RVA - 0x%x", SmiHandler->CallerAddr - ImageStruct->ImageBase));
    }
    DEBUG ((DEBUG_INFO, "\n"));
  }

  return;
}

/**
  Dump all SMI entry on the list.

  @param SmiEntryList a list of SMI entry.
**/
VOID
DumpSmiEntryList(
  IN LIST_ENTRY      *SmiEntryList
  )
{
  LIST_ENTRY      *ListEntry;
  SMI_ENTRY       *SmiEntry;

  ListEntry = SmiEntryList;
  for (ListEntry = ListEntry->ForwardLink;
       ListEntry != SmiEntryList;
       ListEntry = ListEntry->ForwardLink) {
    SmiEntry = CR(ListEntry, SMI_ENTRY, AllEntries, SMI_ENTRY_SIGNATURE);
    DEBUG ((DEBUG_INFO, "SmiEntry - %g\n", &SmiEntry->HandlerType));
    DumpSmiHandlerOnSmiEntry(SmiEntry);
  }

  return;
}

/**
  SMM Ready To Lock event notification handler.

  This function collects all SMM image information and build SmiHandleProfile database,
  and register SmiHandlerProfile SMI handler.

  @param[in] Protocol   Points to the protocol's unique identifier.
  @param[in] Interface  Points to the interface instance.
  @param[in] Handle     The handle on which the interface was installed.

  @retval EFI_SUCCESS   Notification handler runs successfully.
**/
EFI_STATUS
EFIAPI
SmmReadyToLockInSmiHandlerProfile (
  IN CONST EFI_GUID  *Protocol,
  IN VOID            *Interface,
  IN EFI_HANDLE      Handle
  )
{
  //
  // Dump all image
  //
  DEBUG ((DEBUG_INFO, "##################\n"));
  DEBUG ((DEBUG_INFO, "# IMAGE DATABASE #\n"));
  DEBUG ((DEBUG_INFO, "##################\n"));
  GetSmmLoadedImage ();
  DEBUG ((DEBUG_INFO, "\n"));

  //
  // Dump SMI Handler
  //
  DEBUG ((DEBUG_INFO, "########################\n"));
  DEBUG ((DEBUG_INFO, "# SMI Handler DATABASE #\n"));
  DEBUG ((DEBUG_INFO, "########################\n"));

  DEBUG ((DEBUG_INFO, "# 1. ROOT SMI Handler #\n"));
  DEBUG_CODE (
    DumpSmiEntryList(mSmmCoreRootSmiEntryList);
  );

  DEBUG ((DEBUG_INFO, "# 2. GUID SMI Handler #\n"));
  DEBUG_CODE (
    DumpSmiEntryList(mSmmCoreSmiEntryList);
  );

  DEBUG ((DEBUG_INFO, "# 3. Hardware SMI Handler #\n"));
  DEBUG_CODE (
    DumpSmiEntryList(mSmmCoreHardwareSmiEntryList);
  );

  DEBUG ((DEBUG_INFO, "\n"));

  RegisterSmiHandlerProfileHandler();

  if (mImageStruct != NULL) {
    FreePool(mImageStruct);
  }

  return EFI_SUCCESS;
}

/**
  returns SMM image data base size.

  @return SMM image data base size.
**/
UINTN
GetSmmImageDatabaseSize(
  VOID
  )
{
  UINTN  Size;
  UINTN  Index;

  Size = (sizeof(SMM_CORE_IMAGE_DATABASE_STRUCTURE)) * mImageStructCount;
  for (Index = 0; Index < mImageStructCount; Index++) {
    Size += mImageStruct[Index].PdbStringSize;
  }
  return Size;
}

/**
  returns all SMI handlers' size associated with SmiEntry.

  @param SmiEntry  SMI entry.

  @return all SMI handlers' size associated with SmiEntry.
**/
UINTN
GetSmmSmiHandlerSizeOnSmiEntry(
  IN SMI_ENTRY       *SmiEntry
  )
{
  LIST_ENTRY      *ListEntry;
  SMI_HANDLER     *SmiHandler;
  UINTN           Size;

  Size = 0;
  ListEntry = &SmiEntry->SmiHandlers;
  for (ListEntry = ListEntry->ForwardLink;
       ListEntry != &SmiEntry->SmiHandlers;
       ListEntry = ListEntry->ForwardLink) {
    SmiHandler = CR(ListEntry, SMI_HANDLER, Link, SMI_HANDLER_SIGNATURE);
    Size += sizeof(SMM_CORE_SMI_HANDLER_STRUCTURE) + SmiHandler->ContextSize;
  }

  return Size;
}

/**
  return all SMI handler database size on the SMI entry list.

  @param SmiEntryList a list of SMI entry.

  @return all SMI handler database size on the SMI entry list.
**/
UINTN
GetSmmSmiDatabaseSize(
  IN LIST_ENTRY      *SmiEntryList
  )
{
  LIST_ENTRY      *ListEntry;
  SMI_ENTRY       *SmiEntry;
  UINTN           Size;

  Size = 0;
  ListEntry = SmiEntryList;
  for (ListEntry = ListEntry->ForwardLink;
       ListEntry != SmiEntryList;
       ListEntry = ListEntry->ForwardLink) {
    SmiEntry = CR(ListEntry, SMI_ENTRY, AllEntries, SMI_ENTRY_SIGNATURE);
    Size += sizeof(SMM_CORE_SMI_DATABASE_STRUCTURE);
    Size += GetSmmSmiHandlerSizeOnSmiEntry(SmiEntry);
  }
  return Size;
}

/**
  return SMI handler profile database size.

  @return SMI handler profile database size.
**/
UINTN
GetSmiHandlerProfileDatabaseSize (
  VOID
  )
{
  mSmmImageDatabaseSize = GetSmmImageDatabaseSize();
  mSmmRootSmiDatabaseSize = GetSmmSmiDatabaseSize(mSmmCoreRootSmiEntryList);
  mSmmSmiDatabaseSize = GetSmmSmiDatabaseSize(mSmmCoreSmiEntryList);
  mSmmHardwareSmiDatabaseSize = GetSmmSmiDatabaseSize(mSmmCoreHardwareSmiEntryList);

  return mSmmImageDatabaseSize + mSmmSmiDatabaseSize + mSmmRootSmiDatabaseSize + mSmmHardwareSmiDatabaseSize;
}

/**
  get SMM image database.

  @param Data           The buffer to hold SMM image database
  @param ExpectedSize   The expected size of the SMM image database

  @return SMM image data base size.
**/
UINTN
GetSmmImageDatabaseData (
  IN OUT VOID  *Data,
  IN     UINTN ExpectedSize
  )
{
  SMM_CORE_IMAGE_DATABASE_STRUCTURE   *ImageStruct;
  UINTN                               Size;
  UINTN                               Index;

  ImageStruct = Data;
  Size = 0;
  for (Index = 0; Index < mImageStructCount; Index++) {
    if (Size >= ExpectedSize) {
      return 0;
    }
    if (sizeof(SMM_CORE_IMAGE_DATABASE_STRUCTURE) + mImageStruct[Index].PdbStringSize > ExpectedSize - Size) {
      return 0;
    }
    ImageStruct->Header.Signature = SMM_CORE_IMAGE_DATABASE_SIGNATURE;
    ImageStruct->Header.Length = (UINT32)(sizeof(SMM_CORE_IMAGE_DATABASE_STRUCTURE) + mImageStruct[Index].PdbStringSize);
    ImageStruct->Header.Revision = SMM_CORE_IMAGE_DATABASE_REVISION;
    CopyGuid(&ImageStruct->FileGuid, &mImageStruct[Index].FileGuid);
    ImageStruct->ImageRef = mImageStruct[Index].ImageRef;
    ImageStruct->EntryPoint = mImageStruct[Index].EntryPoint;
    ImageStruct->ImageBase = mImageStruct[Index].ImageBase;
    ImageStruct->ImageSize = mImageStruct[Index].ImageSize;
    ImageStruct->PdbStringOffset = sizeof(SMM_CORE_IMAGE_DATABASE_STRUCTURE);
    CopyMem ((VOID *)((UINTN)ImageStruct + ImageStruct->PdbStringOffset), mImageStruct[Index].PdbString, mImageStruct[Index].PdbStringSize);
    ImageStruct = (SMM_CORE_IMAGE_DATABASE_STRUCTURE *)((UINTN)ImageStruct + ImageStruct->Header.Length);
    Size += sizeof(SMM_CORE_IMAGE_DATABASE_STRUCTURE) + mImageStruct[Index].PdbStringSize;
  }

  if (ExpectedSize != Size) {
    return 0;
  }
  return Size;
}

/**
  get all SMI handler data associated with SmiEntry.

  @param SmiEntry       SMI entry.
  @param Data           The buffer to hold all SMI handler data
  @param MaxSize        The max size of the SMM image database
  @param Count          The count of the SMI handler.

  @return SMM image data base size.
**/
UINTN
GetSmmSmiHandlerDataOnSmiEntry(
  IN     SMI_ENTRY       *SmiEntry,
  IN OUT VOID            *Data,
  IN     UINTN           MaxSize,
     OUT UINTN           *Count
  )
{
  SMM_CORE_SMI_HANDLER_STRUCTURE   *SmiHandlerStruct;
  LIST_ENTRY                       *ListEntry;
  SMI_HANDLER                      *SmiHandler;
  UINTN                            Size;

  SmiHandlerStruct = Data;
  Size = 0;
  *Count = 0;
  ListEntry = &SmiEntry->SmiHandlers;
  for (ListEntry = ListEntry->ForwardLink;
       ListEntry != &SmiEntry->SmiHandlers;
       ListEntry = ListEntry->ForwardLink) {
    SmiHandler = CR(ListEntry, SMI_HANDLER, Link, SMI_HANDLER_SIGNATURE);
    if (Size >= MaxSize) {
      *Count = 0;
      return 0;
    }
    if (sizeof(SMM_CORE_SMI_HANDLER_STRUCTURE) + SmiHandler->ContextSize > MaxSize - Size) {
      *Count = 0;
      return 0;
    }
    SmiHandlerStruct->Length = (UINT32)(sizeof(SMM_CORE_SMI_HANDLER_STRUCTURE) + SmiHandler->ContextSize);
    SmiHandlerStruct->CallerAddr = (UINTN)SmiHandler->CallerAddr;
    SmiHandlerStruct->Handler = (UINTN)SmiHandler->Handler;
    SmiHandlerStruct->ImageRef = AddressToImageRef((UINTN)SmiHandler->Handler);
    SmiHandlerStruct->ContextBufferSize = (UINT32)SmiHandler->ContextSize;
    if (SmiHandler->ContextSize != 0) {
      SmiHandlerStruct->ContextBufferOffset = sizeof(SMM_CORE_SMI_HANDLER_STRUCTURE);
      CopyMem ((UINT8 *)SmiHandlerStruct + SmiHandlerStruct->ContextBufferOffset, SmiHandler->Context, SmiHandler->ContextSize);
    } else {
      SmiHandlerStruct->ContextBufferOffset = 0;
    }
    Size += sizeof(SMM_CORE_SMI_HANDLER_STRUCTURE) + SmiHandler->ContextSize;
    SmiHandlerStruct = (SMM_CORE_SMI_HANDLER_STRUCTURE *)((UINTN)SmiHandlerStruct + SmiHandlerStruct->Length);
    *Count = *Count + 1;
  }

  return Size;
}

/**
  get all SMI handler database on the SMI entry list.

  @param SmiEntryList     a list of SMI entry.
  @param HandlerCategory  The handler category
  @param Data             The buffer to hold all SMI handler database
  @param ExpectedSize     The expected size of the SMM image database

  @return all SMI database size on the SMI entry list.
**/
UINTN
GetSmmSmiDatabaseData(
  IN     LIST_ENTRY      *SmiEntryList,
  IN     UINT32          HandlerCategory,
  IN OUT VOID            *Data,
  IN     UINTN           ExpectedSize
  )
{
  SMM_CORE_SMI_DATABASE_STRUCTURE   *SmiStruct;
  LIST_ENTRY                        *ListEntry;
  SMI_ENTRY                         *SmiEntry;
  UINTN                             Size;
  UINTN                             SmiHandlerSize;
  UINTN                             SmiHandlerCount;

  SmiStruct = Data;
  Size = 0;
  ListEntry = SmiEntryList;
  for (ListEntry = ListEntry->ForwardLink;
       ListEntry != SmiEntryList;
       ListEntry = ListEntry->ForwardLink) {
    SmiEntry = CR(ListEntry, SMI_ENTRY, AllEntries, SMI_ENTRY_SIGNATURE);
    if (Size >= ExpectedSize) {
      return 0;
    }
    if (sizeof(SMM_CORE_SMI_DATABASE_STRUCTURE) > ExpectedSize - Size) {
      return 0;
    }

    SmiStruct->Header.Signature = SMM_CORE_SMI_DATABASE_SIGNATURE;
    SmiStruct->Header.Length = sizeof(SMM_CORE_SMI_DATABASE_STRUCTURE);
    SmiStruct->Header.Revision = SMM_CORE_SMI_DATABASE_REVISION;
    SmiStruct->HandlerCategory = HandlerCategory;
    CopyGuid(&SmiStruct->HandlerType, &SmiEntry->HandlerType);
    Size += sizeof(SMM_CORE_SMI_DATABASE_STRUCTURE);
    SmiHandlerSize = GetSmmSmiHandlerDataOnSmiEntry(SmiEntry, (UINT8 *)SmiStruct + SmiStruct->Header.Length, ExpectedSize - Size, &SmiHandlerCount);
    SmiStruct->HandlerCount = SmiHandlerCount;
    Size += SmiHandlerSize;
    SmiStruct->Header.Length += (UINT32)SmiHandlerSize;
    SmiStruct = (VOID *)((UINTN)SmiStruct + SmiStruct->Header.Length);
  }
  if (ExpectedSize != Size) {
    return 0;
  }
  return Size;
}

/**
  Get SMI handler profile database.

  @param Data the buffer to hold SMI handler profile database

  @retval EFI_SUCCESS            the database is got.
  @retval EFI_INVALID_PARAMETER  the database size mismatch.
**/
EFI_STATUS
GetSmiHandlerProfileDatabaseData(
  IN OUT VOID *Data
  )
{
  UINTN  SmmImageDatabaseSize;
  UINTN  SmmSmiDatabaseSize;
  UINTN  SmmRootSmiDatabaseSize;
  UINTN  SmmHardwareSmiDatabaseSize;

  DEBUG((DEBUG_VERBOSE, "GetSmiHandlerProfileDatabaseData\n"));
  SmmImageDatabaseSize = GetSmmImageDatabaseData(Data, mSmmImageDatabaseSize);
  if (SmmImageDatabaseSize != mSmmImageDatabaseSize) {
    DEBUG((DEBUG_ERROR, "GetSmiHandlerProfileDatabaseData - SmmImageDatabaseSize mismatch!\n"));
    return EFI_INVALID_PARAMETER;
  }
  SmmRootSmiDatabaseSize = GetSmmSmiDatabaseData(mSmmCoreRootSmiEntryList, SmmCoreSmiHandlerCategoryRootHandler, (UINT8 *)Data + SmmImageDatabaseSize, mSmmRootSmiDatabaseSize);
  if (SmmRootSmiDatabaseSize != mSmmRootSmiDatabaseSize) {
    DEBUG((DEBUG_ERROR, "GetSmiHandlerProfileDatabaseData - SmmRootSmiDatabaseSize mismatch!\n"));
    return EFI_INVALID_PARAMETER;
  }
  SmmSmiDatabaseSize = GetSmmSmiDatabaseData(mSmmCoreSmiEntryList, SmmCoreSmiHandlerCategoryGuidHandler, (UINT8 *)Data + SmmImageDatabaseSize + mSmmRootSmiDatabaseSize, mSmmSmiDatabaseSize);
  if (SmmSmiDatabaseSize != mSmmSmiDatabaseSize) {
    DEBUG((DEBUG_ERROR, "GetSmiHandlerProfileDatabaseData - SmmSmiDatabaseSize mismatch!\n"));
    return EFI_INVALID_PARAMETER;
  }
  SmmHardwareSmiDatabaseSize = GetSmmSmiDatabaseData(mSmmCoreHardwareSmiEntryList, SmmCoreSmiHandlerCategoryHardwareHandler, (UINT8 *)Data + SmmImageDatabaseSize + SmmRootSmiDatabaseSize + SmmSmiDatabaseSize, mSmmHardwareSmiDatabaseSize);
  if (SmmHardwareSmiDatabaseSize != mSmmHardwareSmiDatabaseSize) {
    DEBUG((DEBUG_ERROR, "GetSmiHandlerProfileDatabaseData - SmmHardwareSmiDatabaseSize mismatch!\n"));
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

/**
  build SMI handler profile database.
**/
VOID
BuildSmiHandlerProfileDatabase(
  VOID
  )
{
  EFI_STATUS  Status;
  mSmiHandlerProfileDatabaseSize = GetSmiHandlerProfileDatabaseSize();
  mSmiHandlerProfileDatabase = AllocatePool(mSmiHandlerProfileDatabaseSize);
  if (mSmiHandlerProfileDatabase == NULL) {
    return;
  }
  Status = GetSmiHandlerProfileDatabaseData(mSmiHandlerProfileDatabase);
  if (EFI_ERROR(Status)) {
    FreePool(mSmiHandlerProfileDatabase);
    mSmiHandlerProfileDatabase = NULL;
  }
}

/**
  Copy SMI handler profile data.

  @param DataBuffer  The buffer to hold SMI handler profile data.
  @param DataSize    On input, data buffer size.
                     On output, actual data buffer size copied.
  @param DataOffset  On input, data buffer offset to copy.
                     On output, next time data buffer offset to copy.

**/
VOID
SmiHandlerProfileCopyData(
  OUT VOID      *DataBuffer,
  IN OUT UINT64 *DataSize,
  IN OUT UINT64 *DataOffset
  )
{
  if (*DataOffset >= mSmiHandlerProfileDatabaseSize) {
    *DataOffset = mSmiHandlerProfileDatabaseSize;
    return;
  }
  if (mSmiHandlerProfileDatabaseSize - *DataOffset < *DataSize) {
    *DataSize = mSmiHandlerProfileDatabaseSize - *DataOffset;
  }

  CopyMem(
    DataBuffer,
    (UINT8 *)mSmiHandlerProfileDatabase + *DataOffset,
    (UINTN)*DataSize
    );
  *DataOffset = *DataOffset + *DataSize;
}

/**
  SMI handler profile handler to get info.

  @param SmiHandlerProfileParameterGetInfo The parameter of SMI handler profile get info.

**/
VOID
SmiHandlerProfileHandlerGetInfo(
  IN SMI_HANDLER_PROFILE_PARAMETER_GET_INFO   *SmiHandlerProfileParameterGetInfo
  )
{
  BOOLEAN                       SmiHandlerProfileRecordingStatus;

  SmiHandlerProfileRecordingStatus = mSmiHandlerProfileRecordingStatus;
  mSmiHandlerProfileRecordingStatus = FALSE;

  SmiHandlerProfileParameterGetInfo->DataSize = mSmiHandlerProfileDatabaseSize;
  SmiHandlerProfileParameterGetInfo->Header.ReturnStatus = 0;

  mSmiHandlerProfileRecordingStatus = SmiHandlerProfileRecordingStatus;
}

/**
  SMI handler profile handler to get data by offset.

  @param SmiHandlerProfileParameterGetDataByOffset   The parameter of SMI handler profile get data by offset.

**/
VOID
SmiHandlerProfileHandlerGetDataByOffset(
  IN SMI_HANDLER_PROFILE_PARAMETER_GET_DATA_BY_OFFSET     *SmiHandlerProfileParameterGetDataByOffset
  )
{
  SMI_HANDLER_PROFILE_PARAMETER_GET_DATA_BY_OFFSET    SmiHandlerProfileGetDataByOffset;
  BOOLEAN                                             SmiHandlerProfileRecordingStatus;

  SmiHandlerProfileRecordingStatus = mSmiHandlerProfileRecordingStatus;
  mSmiHandlerProfileRecordingStatus = FALSE;

  CopyMem(&SmiHandlerProfileGetDataByOffset, SmiHandlerProfileParameterGetDataByOffset, sizeof(SmiHandlerProfileGetDataByOffset));

  //
  // Sanity check
  //
  if (!SmmIsBufferOutsideSmmValid((UINTN)SmiHandlerProfileGetDataByOffset.DataBuffer, (UINTN)SmiHandlerProfileGetDataByOffset.DataSize)) {
    DEBUG((DEBUG_ERROR, "SmiHandlerProfileHandlerGetDataByOffset: SMI handler profile get data in SMRAM or overflow!\n"));
    SmiHandlerProfileParameterGetDataByOffset->Header.ReturnStatus = (UINT64)(INT64)(INTN)EFI_ACCESS_DENIED;
    goto Done;
  }

  SmiHandlerProfileCopyData((VOID *)(UINTN)SmiHandlerProfileGetDataByOffset.DataBuffer, &SmiHandlerProfileGetDataByOffset.DataSize, &SmiHandlerProfileGetDataByOffset.DataOffset);
  CopyMem(SmiHandlerProfileParameterGetDataByOffset, &SmiHandlerProfileGetDataByOffset, sizeof(SmiHandlerProfileGetDataByOffset));
  SmiHandlerProfileParameterGetDataByOffset->Header.ReturnStatus = 0;

Done:
  mSmiHandlerProfileRecordingStatus = SmiHandlerProfileRecordingStatus;
}

/**
  Dispatch function for a Software SMI handler.

  Caution: This function may receive untrusted input.
  Communicate buffer and buffer size are external input, so this function will do basic validation.

  @param DispatchHandle  The unique handle assigned to this handler by SmiHandlerRegister().
  @param Context         Points to an optional handler context which was specified when the
                         handler was registered.
  @param CommBuffer      A pointer to a collection of data in memory that will
                         be conveyed from a non-SMM environment into an SMM environment.
  @param CommBufferSize  The size of the CommBuffer.

  @retval EFI_SUCCESS Command is handled successfully.
**/
EFI_STATUS
EFIAPI
SmiHandlerProfileHandler(
  IN EFI_HANDLE  DispatchHandle,
  IN CONST VOID  *Context         OPTIONAL,
  IN OUT VOID    *CommBuffer      OPTIONAL,
  IN OUT UINTN   *CommBufferSize  OPTIONAL
  )
{
  SMI_HANDLER_PROFILE_PARAMETER_HEADER           *SmiHandlerProfileParameterHeader;
  UINTN                                    TempCommBufferSize;

  DEBUG((DEBUG_ERROR, "SmiHandlerProfileHandler Enter\n"));

  if (mSmiHandlerProfileDatabase == NULL) {
    return EFI_SUCCESS;
  }

  //
  // If input is invalid, stop processing this SMI
  //
  if (CommBuffer == NULL || CommBufferSize == NULL) {
    return EFI_SUCCESS;
  }

  TempCommBufferSize = *CommBufferSize;

  if (TempCommBufferSize < sizeof(SMI_HANDLER_PROFILE_PARAMETER_HEADER)) {
    DEBUG((DEBUG_ERROR, "SmiHandlerProfileHandler: SMM communication buffer size invalid!\n"));
    return EFI_SUCCESS;
  }

  if (!SmmIsBufferOutsideSmmValid((UINTN)CommBuffer, TempCommBufferSize)) {
    DEBUG((DEBUG_ERROR, "SmiHandlerProfileHandler: SMM communication buffer in SMRAM or overflow!\n"));
    return EFI_SUCCESS;
  }

  SmiHandlerProfileParameterHeader = (SMI_HANDLER_PROFILE_PARAMETER_HEADER *)((UINTN)CommBuffer);
  SmiHandlerProfileParameterHeader->ReturnStatus = (UINT64)-1;

  switch (SmiHandlerProfileParameterHeader->Command) {
  case SMI_HANDLER_PROFILE_COMMAND_GET_INFO:
    DEBUG((DEBUG_ERROR, "SmiHandlerProfileHandlerGetInfo\n"));
    if (TempCommBufferSize != sizeof(SMI_HANDLER_PROFILE_PARAMETER_GET_INFO)) {
      DEBUG((DEBUG_ERROR, "SmiHandlerProfileHandler: SMM communication buffer size invalid!\n"));
      return EFI_SUCCESS;
    }
    SmiHandlerProfileHandlerGetInfo((SMI_HANDLER_PROFILE_PARAMETER_GET_INFO *)(UINTN)CommBuffer);
    break;
  case SMI_HANDLER_PROFILE_COMMAND_GET_DATA_BY_OFFSET:
    DEBUG((DEBUG_ERROR, "SmiHandlerProfileHandlerGetDataByOffset\n"));
    if (TempCommBufferSize != sizeof(SMI_HANDLER_PROFILE_PARAMETER_GET_DATA_BY_OFFSET)) {
      DEBUG((DEBUG_ERROR, "SmiHandlerProfileHandler: SMM communication buffer size invalid!\n"));
      return EFI_SUCCESS;
    }
    SmiHandlerProfileHandlerGetDataByOffset((SMI_HANDLER_PROFILE_PARAMETER_GET_DATA_BY_OFFSET *)(UINTN)CommBuffer);
    break;
  default:
    break;
  }

  DEBUG((DEBUG_ERROR, "SmiHandlerProfileHandler Exit\n"));

  return EFI_SUCCESS;
}

/**
  Register SMI handler profile handler.
**/
VOID
RegisterSmiHandlerProfileHandler (
  VOID
  )
{
  EFI_STATUS    Status;
  EFI_HANDLE    DispatchHandle;

  Status = gSmst->SmiHandlerRegister (
                    SmiHandlerProfileHandler,
                    &gSmiHandlerProfileGuid,
                    &DispatchHandle
                    );
  ASSERT_EFI_ERROR (Status);

  BuildSmiHandlerProfileDatabase();
}

/**
  Finds the SMI entry for the requested handler type.

  @param  HandlerType            The type of the interrupt
  @param  Create                 Create a new entry if not found

  @return SMI entry
**/
SMI_ENTRY  *
SmmCoreFindHardwareSmiEntry (
  IN EFI_GUID  *HandlerType,
  IN BOOLEAN   Create
  )
{
  LIST_ENTRY  *Link;
  SMI_ENTRY   *Item;
  SMI_ENTRY   *SmiEntry;

  //
  // Search the SMI entry list for the matching GUID
  //
  SmiEntry = NULL;
  for (Link = mHardwareSmiEntryList.ForwardLink;
       Link != &mHardwareSmiEntryList;
       Link = Link->ForwardLink) {

    Item = CR (Link, SMI_ENTRY, AllEntries, SMI_ENTRY_SIGNATURE);
    if (CompareGuid (&Item->HandlerType, HandlerType)) {
      //
      // This is the SMI entry
      //
      SmiEntry = Item;
      break;
    }
  }

  //
  // If the protocol entry was not found and Create is TRUE, then
  // allocate a new entry
  //
  if ((SmiEntry == NULL) && Create) {
    SmiEntry = AllocatePool (sizeof(SMI_ENTRY));
    if (SmiEntry != NULL) {
      //
      // Initialize new SMI entry structure
      //
      SmiEntry->Signature = SMI_ENTRY_SIGNATURE;
      CopyGuid ((VOID *)&SmiEntry->HandlerType, HandlerType);
      InitializeListHead (&SmiEntry->SmiHandlers);

      //
      // Add it to SMI entry list
      //
      InsertTailList (&mHardwareSmiEntryList, &SmiEntry->AllEntries);
    }
  }
  return SmiEntry;
}

/**
  This function is called by SmmChildDispatcher module to report
  a new SMI handler is registered, to SmmCore.

  @param This            The protocol instance
  @param HandlerGuid     The GUID to identify the type of the handler.
                         For the SmmChildDispatch protocol, the HandlerGuid
                         must be the GUID of SmmChildDispatch protocol.
  @param Handler         The SMI handler.
  @param CallerAddress   The address of the module who registers the SMI handler.
  @param Context         The context of the SMI handler.
                         For the SmmChildDispatch protocol, the Context
                         must match the one defined for SmmChildDispatch protocol.
  @param ContextSize     The size of the context in bytes.
                         For the SmmChildDispatch protocol, the Context
                         must match the one defined for SmmChildDispatch protocol.

  @retval EFI_SUCCESS           The information is recorded.
  @retval EFI_OUT_OF_RESOURCES  There is no enough resource to record the information.
**/
EFI_STATUS
EFIAPI
SmiHandlerProfileRegisterHandler (
  IN SMI_HANDLER_PROFILE_PROTOCOL   *This,
  IN EFI_GUID                       *HandlerGuid,
  IN EFI_SMM_HANDLER_ENTRY_POINT2   Handler,
  IN PHYSICAL_ADDRESS               CallerAddress,
  IN VOID                           *Context, OPTIONAL
  IN UINTN                          ContextSize OPTIONAL
  )
{
  SMI_HANDLER  *SmiHandler;
  SMI_ENTRY    *SmiEntry;
  LIST_ENTRY   *List;

  SmiHandler = AllocateZeroPool (sizeof (SMI_HANDLER));
  if (SmiHandler == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  SmiHandler->Signature = SMI_HANDLER_SIGNATURE;
  SmiHandler->Handler = Handler;
  SmiHandler->CallerAddr = (UINTN)CallerAddress;
  if (ContextSize != 0 && Context != NULL) {
    if (CompareGuid (HandlerGuid, &gEfiSmmUsbDispatch2ProtocolGuid)) {
      EFI_SMM_USB_REGISTER_CONTEXT              *UsbContext;
      UINTN                                     DevicePathSize;
      SMI_HANDLER_PROFILE_USB_REGISTER_CONTEXT  *SmiHandlerUsbContext;

      ASSERT (ContextSize == sizeof(EFI_SMM_USB_REGISTER_CONTEXT));

      UsbContext = (EFI_SMM_USB_REGISTER_CONTEXT *)Context;
      DevicePathSize = GetDevicePathSize (UsbContext->Device);
      SmiHandlerUsbContext = AllocatePool (sizeof (SMI_HANDLER_PROFILE_USB_REGISTER_CONTEXT) + DevicePathSize);
      if (SmiHandlerUsbContext != NULL) {
        SmiHandlerUsbContext->Type = UsbContext->Type;
        SmiHandlerUsbContext->DevicePathSize = (UINT32)DevicePathSize;
        CopyMem (SmiHandlerUsbContext + 1, UsbContext->Device, DevicePathSize);
        SmiHandler->Context = SmiHandlerUsbContext;
      }
    } else {
      SmiHandler->Context = AllocateCopyPool (ContextSize, Context);
    }
  }
  if (SmiHandler->Context != NULL) {
    SmiHandler->ContextSize = ContextSize;
  }

  SmiEntry = SmmCoreFindHardwareSmiEntry (HandlerGuid, TRUE);
  if (SmiEntry == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  List = &SmiEntry->SmiHandlers;

  SmiHandler->SmiEntry = SmiEntry;
  InsertTailList (List, &SmiHandler->Link);

  return EFI_SUCCESS;
}

/**
  This function is called by SmmChildDispatcher module to report
  an existing SMI handler is unregistered, to SmmCore.

  @param This            The protocol instance
  @param HandlerGuid     The GUID to identify the type of the handler.
                         For the SmmChildDispatch protocol, the HandlerGuid
                         must be the GUID of SmmChildDispatch protocol.
  @param Handler         The SMI handler.

  @retval EFI_SUCCESS           The original record is removed.
  @retval EFI_NOT_FOUND         There is no record for the HandlerGuid and handler.
**/
EFI_STATUS
EFIAPI
SmiHandlerProfileUnregisterHandler (
  IN SMI_HANDLER_PROFILE_PROTOCOL   *This,
  IN EFI_GUID                       *HandlerGuid,
  IN EFI_SMM_HANDLER_ENTRY_POINT2   Handler
  )
{
  LIST_ENTRY   *Link;
  LIST_ENTRY   *Head;
  SMI_HANDLER  *SmiHandler;
  SMI_ENTRY    *SmiEntry;
  SMI_HANDLER  *TargetSmiHandler;

  SmiEntry = SmmCoreFindHardwareSmiEntry (HandlerGuid, FALSE);
  if (SmiEntry == NULL) {
    return EFI_NOT_FOUND;
  }

  TargetSmiHandler = NULL;
  Head = &SmiEntry->SmiHandlers;
  for (Link = Head->ForwardLink; Link != Head; Link = Link->ForwardLink) {
    SmiHandler = CR (Link, SMI_HANDLER, Link, SMI_HANDLER_SIGNATURE);
    if (SmiHandler->Handler == Handler) {
      TargetSmiHandler = SmiHandler;
      break;
    }
  }
  if (TargetSmiHandler == NULL) {
    return EFI_NOT_FOUND;
  }
  SmiHandler = TargetSmiHandler;

  RemoveEntryList (&SmiHandler->Link);
  FreePool (SmiHandler);

  if (IsListEmpty (&SmiEntry->SmiHandlers)) {
    RemoveEntryList (&SmiEntry->AllEntries);
    FreePool (SmiEntry);
  }

  return EFI_SUCCESS;
}

/**
  Initialize SmiHandler profile feature.
**/
VOID
SmmCoreInitializeSmiHandlerProfile (
  VOID
  )
{
  EFI_STATUS  Status;
  VOID        *Registration;
  EFI_HANDLE  Handle;

  if ((PcdGet8 (PcdSmiHandlerProfilePropertyMask) & 0x1) != 0) {
    InsertTailList (&mRootSmiEntryList, &mRootSmiEntry.AllEntries);

    Status = gSmst->SmmRegisterProtocolNotify (
                      &gEfiSmmReadyToLockProtocolGuid,
                      SmmReadyToLockInSmiHandlerProfile,
                      &Registration
                      );
    ASSERT_EFI_ERROR (Status);

    Handle = NULL;
    Status = gSmst->SmmInstallProtocolInterface (
                      &Handle,
                      &gSmiHandlerProfileGuid,
                      EFI_NATIVE_INTERFACE,
                      &mSmiHandlerProfile
                      );
    ASSERT_EFI_ERROR (Status);
  }
}

