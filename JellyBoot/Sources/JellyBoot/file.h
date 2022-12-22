/*

    JellyBoot - file.h
    JellyBoot.efi

*/

#ifndef __FILE_H
#define __FILE_H

// Include libraries
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <JellyBoot/file.h>

EFI_FILE* LoadFile(EFI_FILE *Directory, CHAR16 *Path, IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable);

#endif // End of file