/*

    JellyBoot - print.h
    JellyBoot.efi

*/

#ifndef __PRINT_H
#define __PRINT_H

typedef unsigned long long size_t;

typedef struct {
    VOID *BaseAddress;
    size_t BufferSize;
    UINTN Width;
    UINTN Height;
    UINTN PixelsPerScanLine;
} FRAME_BUFFER;

typedef struct {
    unsigned int X;
    unsigned int Y;
} Point;

// Include libraries
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <JellyBoot/psf1.h>


FRAME_BUFFER* InitGraphicsOutputProtocol(IN EFI_SYSTEM_TABLE *SystemTable);
VOID PrintChar(FRAME_BUFFER* framebuffer, PSF1_FONT* psf1_font, unsigned int colour, char chr, unsigned int xOff, unsigned int yOff);
VOID PrintLine(FRAME_BUFFER* framebuffer, PSF1_FONT* psf1_font, unsigned int colour, char* str);

#endif // End of file