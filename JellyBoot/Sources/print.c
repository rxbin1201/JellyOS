/*

    JellyBoot - print.c
    JellyBoot.efi

*/

#include <JellyBoot/print.h>

FRAME_BUFFER FrameBuffer;
Point CursorPosition;

FRAME_BUFFER* InitGraphicsOutputProtocol(IN EFI_SYSTEM_TABLE *SystemTable) {

    // Define variables
    EFI_STATUS Status;
    EFI_GUID GraphicsOutputProtocolGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *GraphicsOutputProtocol;

    // Get the graphics output protocol and text output protocol
    Status = SystemTable->BootServices->LocateProtocol(
        &GraphicsOutputProtocolGuid,
        NULL,
        (VOID **)&GraphicsOutputProtocol
    );
    if(EFI_ERROR(Status)) {
        Print(L"ERROR! Failed to get GOP! Reason: %r\n", Status);
        return NULL;
    }

    FrameBuffer.BaseAddress = (VOID **)GraphicsOutputProtocol->Mode->FrameBufferBase;
    FrameBuffer.BufferSize = GraphicsOutputProtocol->Mode->FrameBufferSize;
    FrameBuffer.Width = GraphicsOutputProtocol->Mode->Info->HorizontalResolution;
    FrameBuffer.Height = GraphicsOutputProtocol->Mode->Info->VerticalResolution;
    FrameBuffer.PixelsPerScanLine = GraphicsOutputProtocol->Mode->Info->PixelsPerScanLine;

    // Set max mode
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Information;
    UINT32 MaxModeNumber;
    UINT32 MaxWidth = 0;
    UINT32 MaxHeight = 0;
    UINTN Size;
    for(UINT32 i = 0; i < GraphicsOutputProtocol->Mode->MaxMode; i++) {
        Status = GraphicsOutputProtocol->QueryMode(
            GraphicsOutputProtocol,
            i,
            &Size,
            &Information
        );

        if(Information->HorizontalResolution > MaxWidth || (Information->HorizontalResolution == MaxWidth && Information->VerticalResolution > MaxHeight)) {
            MaxWidth = Information->HorizontalResolution;
            MaxHeight = Information->HorizontalResolution;
            MaxModeNumber = i;
        }
    }

    Status = GraphicsOutputProtocol->SetMode(
        GraphicsOutputProtocol,
        MaxModeNumber
    );

    return &FrameBuffer;
}

VOID PrintChar(FRAME_BUFFER* framebuffer, PSF1_FONT* psf1_font, unsigned int colour, char chr, unsigned int xOff, unsigned int yOff) {
    unsigned int* pixPtr = (unsigned int*)framebuffer->BaseAddress;
    char* fontPtr = psf1_font->GlyphBuffer + (chr * psf1_font->PsfHeader->CharSize);
    for (unsigned long y = yOff; y < yOff + 18; y++){
        for (unsigned long x = xOff; x < xOff+8; x++){
            if ((*fontPtr & (0b10000000 >> (x - xOff))) > 0){
                    *(unsigned int*)(pixPtr + x + (y * framebuffer->PixelsPerScanLine)) = colour;
                }

        }
        fontPtr++;
    }
}

VOID PrintLine(FRAME_BUFFER* framebuffer, PSF1_FONT* psf1_font, unsigned int colour, char* str) {
    
    char* chr = str;
    while(*chr != 0){
        PrintChar(framebuffer, psf1_font, colour, *chr, CursorPosition.X, CursorPosition.Y);
        CursorPosition.X+=8;
        if(CursorPosition.X + 8 > framebuffer->Width)
        {
            CursorPosition.X = 0;
            CursorPosition.Y += 18;
        }
        chr++;
    }
}

VOID LogError(FRAME_BUFFER* framebuffer, PSF1_FONT* psf1_font, unsigned int colour, char* str) {
    
    char* chr = str;
    PrintChar(framebuffer, psf1_font, 0xffffffff, '[', CursorPosition.X, CursorPosition.Y);
    CursorPosition.X+=8;
    if(CursorPosition.X + 8 > framebuffer->Width)
    {
        CursorPosition.X = 0;
        CursorPosition.Y += 18;
    }
    PrintChar(framebuffer, psf1_font, 0xFFDC2626, 'E', CursorPosition.X, CursorPosition.Y);
    CursorPosition.X+=8;
    if(CursorPosition.X + 8 > framebuffer->Width)
    {
        CursorPosition.X = 0;
        CursorPosition.Y += 18;
    }
    PrintChar(framebuffer, psf1_font, 0xFFDC2626, 'R', CursorPosition.X, CursorPosition.Y);
    CursorPosition.X+=8;
    if(CursorPosition.X + 8 > framebuffer->Width)
    {
        CursorPosition.X = 0;
        CursorPosition.Y += 18;
    }
    PrintChar(framebuffer, psf1_font, 0xFFDC2626, 'R', CursorPosition.X, CursorPosition.Y);
    CursorPosition.X+=8;
    if(CursorPosition.X + 8 > framebuffer->Width)
    {
        CursorPosition.X = 0;
        CursorPosition.Y += 18;
    }
    PrintChar(framebuffer, psf1_font, 0xFFDC2626, 'O', CursorPosition.X, CursorPosition.Y);
    CursorPosition.X+=8;
    if(CursorPosition.X + 8 > framebuffer->Width)
    {
        CursorPosition.X = 0;
        CursorPosition.Y += 18;
    }
    PrintChar(framebuffer, psf1_font, 0xFFDC2626, 'R', CursorPosition.X, CursorPosition.Y);
    CursorPosition.X+=8;
    if(CursorPosition.X + 8 > framebuffer->Width)
    {
        CursorPosition.X = 0;
        CursorPosition.Y += 18;
    }
    PrintChar(framebuffer, psf1_font, 0xffffffff, ']', CursorPosition.X, CursorPosition.Y);
    CursorPosition.X+=8;
    if(CursorPosition.X + 8 > framebuffer->Width)
    {
        CursorPosition.X = 0;
        CursorPosition.Y += 18;
    }
    while(*chr != 0){
        PrintChar(framebuffer, psf1_font, colour, *chr, CursorPosition.X, CursorPosition.Y);
        CursorPosition.X+=8;
        if(CursorPosition.X + 8 > framebuffer->Width)
        {
            CursorPosition.X = 0;
            CursorPosition.Y += 18;
        }
        chr++;
    }
}

VOID LogOK(FRAME_BUFFER* framebuffer, PSF1_FONT* psf1_font, unsigned int colour, char* str) {
    
    char* chr = str;
    PrintChar(framebuffer, psf1_font, 0xffffffff, '[', CursorPosition.X, CursorPosition.Y);
    CursorPosition.X+=8;
    if(CursorPosition.X + 8 > framebuffer->Width)
    {
        CursorPosition.X = 0;
        CursorPosition.Y += 18;
    }
    PrintChar(framebuffer, psf1_font, 0xFF22C55E, 'O', CursorPosition.X, CursorPosition.Y);
    CursorPosition.X+=8;
    if(CursorPosition.X + 8 > framebuffer->Width)
    {
        CursorPosition.X = 0;
        CursorPosition.Y += 18;
    }
    PrintChar(framebuffer, psf1_font, 0xFF22C55E, 'K', CursorPosition.X, CursorPosition.Y);
    CursorPosition.X+=8;
    if(CursorPosition.X + 8 > framebuffer->Width)
    {
        CursorPosition.X = 0;
        CursorPosition.Y += 18;
    }
    PrintChar(framebuffer, psf1_font, 0xffffffff, ']', CursorPosition.X, CursorPosition.Y);
    CursorPosition.X+=8;
    if(CursorPosition.X + 8 > framebuffer->Width)
    {
        CursorPosition.X = 0;
        CursorPosition.Y += 18;
    }
    while(*chr != 0){
        PrintChar(framebuffer, psf1_font, colour, *chr, CursorPosition.X, CursorPosition.Y);
        CursorPosition.X+=8;
        if(CursorPosition.X + 8 > framebuffer->Width)
        {
            CursorPosition.X = 0;
            CursorPosition.Y += 18;
        }
        chr++;
    }
}