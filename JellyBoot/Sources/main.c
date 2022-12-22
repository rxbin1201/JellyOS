/*

    JellyBoot - main.c
    JellyBoot.efi

*/

// Include libraries
#include <JellyBoot/main.h>

EFI_STATUS
EFIAPI
UefiMain(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) {

    // Define variables
    FRAME_BUFFER *NewFrameBuffer = InitGraphicsOutputProtocol(SystemTable);

    // Load font file
    PSF1_FONT *DefaultFont = LoadFont(NULL, L"\\EFI\\JellyBoot\\Fonts\\DefaultFont.psf", ImageHandle, SystemTable);
    if(DefaultFont == NULL) {
        Print(L"Font is not valid or not found!\n");
    }

    // Print with new font
    PrintLine(NewFrameBuffer, DefaultFont, 0xffffffff, "Hello");

    while(1){};

    return EFI_SUCCESS;
}