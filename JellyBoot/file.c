/*

    JellyBoot - file.c
    JellyBoot.efi

*/

#include <JellyBoot/file.h>

EFI_FILE* LoadFile(EFI_FILE *Directory, CHAR16 *Path, IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) {

    // Define variables 
    EFI_STATUS Status;
    EFI_FILE *LoadedFile;
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
    EFI_GUID LoadedImageProtocolGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *SimpleFileSystem;

    // Handle loaded image protocol
    Status = SystemTable->BootServices->HandleProtocol(
        ImageHandle,
        &LoadedImageProtocolGuid,
        (VOID **)&LoadedImage
    ); 
    if(EFI_ERROR(Status)) {
        return NULL;
    }

    // Handle simple file system protocol
    Status = SystemTable->BootServices->HandleProtocol(
        LoadedImage->DeviceHandle,
        &gEfiSimpleFileSystemProtocolGuid,
        (VOID **)&SimpleFileSystem
    );
    if(EFI_ERROR(Status)) {
        return NULL;
    }   

    // Open volume
    if(Directory == NULL) {
        SimpleFileSystem->OpenVolume(
            SimpleFileSystem,
            &Directory
        );
    }

    // Open file
    Status = Directory->Open(
        Directory,
        &LoadedFile,
        Path,
        EFI_FILE_MODE_READ,
        EFI_FILE_READ_ONLY
    );
    if(EFI_ERROR(Status)) {
        return NULL;
    }

    return LoadedFile;
}