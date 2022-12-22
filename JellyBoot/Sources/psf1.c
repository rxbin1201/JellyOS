/*

    JellyBoot - psf1.c
    JellyBoot.efi

*/

#include <JellyBoot/psf1.h>

PSF1_FONT* LoadFont(EFI_FILE *Directory, CHAR16 *Path, IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) {

    // Define variables
    EFI_STATUS Status;
    EFI_FILE *FontFile = LoadFile(Directory, Path, ImageHandle, SystemTable);
    PSF1_HEADER *FontHeader;
    PSF1_FONT *FinalFont;
    UINTN Size;
    UINTN GlyphBufferSize;
    
    if(FontFile == NULL) return NULL;

    Status = SystemTable->BootServices->AllocatePool(
        EfiLoaderData,
        sizeof(PSF1_HEADER),
        (VOID **)&FontHeader
    );
    if(EFI_ERROR(Status)) {
        return NULL;
    }

    Size = sizeof(PSF1_HEADER);
    FontFile->Read(FontFile, &Size, FontHeader);

    if(FontHeader->Magic[0] != PSF1_MAGIC0 || FontHeader->Magic[1] != PSF1_MAGIC1) {
        return NULL;
    }

    GlyphBufferSize = FontHeader->CharSize * 256;
    if(FontHeader->Mode == 1) {
        GlyphBufferSize = FontHeader->CharSize * 512;
    }

    VOID *GlyphBuffer; {
        FontFile->SetPosition(FontFile, sizeof(PSF1_HEADER));
        Status = SystemTable->BootServices->AllocatePool(
            EfiLoaderData,
            GlyphBufferSize,
            (VOID **)&GlyphBuffer
        );
        if(EFI_ERROR(Status)) {
            return NULL;
        }
        FontFile->Read(FontFile, &GlyphBufferSize, GlyphBuffer);
    }

    Status = SystemTable->BootServices->AllocatePool(
        EfiLoaderData,
        sizeof(PSF1_FONT),
        (VOID **)&FinalFont
    );
    if(EFI_ERROR(Status)) {
        return NULL;
    }

    FinalFont->PsfHeader = FontHeader;
    FinalFont->GlyphBuffer = GlyphBuffer;

    return FinalFont;
}