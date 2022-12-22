/*

    JellyBoot - psf1.h
    JellyBoot.efi

*/

#ifndef __PSF1_H
#define __PSF1_H

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

#define PSF1_MAGIC0 0x36
#define PSF1_MAGIC1 0x04

typedef struct {
    unsigned char Magic[2];
    unsigned char Mode;
    unsigned char CharSize;
} PSF1_HEADER;

typedef struct {
    PSF1_HEADER *PsfHeader;
    VOID *GlyphBuffer;
} PSF1_FONT;

PSF1_FONT* LoadFont(EFI_FILE *Directory, CHAR16 *Path, IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable);

#endif // End of file