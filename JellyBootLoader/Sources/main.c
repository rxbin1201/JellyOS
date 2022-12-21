/*

    JellyBootLoader - main.c
    BOOTX64.EFI

*/

// Import libraries from GNU-EFI
#include <efi.h>
#include <efilib.h>

EFI_STATUS
EFIAPI
efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {

    // Initialize the UEFI
    InitializeLib(ImageHandle, SystemTable);

    // Define variables
    EFI_STATUS Status;    
    EFI_HANDLE LoadedImageHandle;
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
    EFI_DEVICE_PATH *DevicePath;

    CHAR16 *FilePath = L"\\EFI\\JellyBoot\\JellyBoot.efi";

    // Get the loaded image protocol for the current image
    Status = uefi_call_wrapper(
        SystemTable->BootServices->OpenProtocol,
        6,
        ImageHandle,
        &LoadedImageProtocol,
        (VOID **)&LoadedImage,
        ImageHandle,
        NULL,
        EFI_OPEN_PROTOCOL_GET_PROTOCOL
    );
    if(EFI_ERROR(Status)) {
        Print(L"Failed to get LoadedImageProtocol!\nReason: %r\n", Status);
    }

    // Create a device path for the file
    DevicePath = FileDevicePath(
        LoadedImage->DeviceHandle,
        FilePath
    );
    if(DevicePath == NULL) {
        Print(L"Failed to create device path for file!\n");
    }

    // Load JellyBoot.EFI
    Status = uefi_call_wrapper(
        SystemTable->BootServices->LoadImage,
        6,
        FALSE,
        ImageHandle,
        DevicePath,
        NULL, 
        0,
        &LoadedImageHandle
    );
    if(EFI_ERROR(Status)) {
        Print(L"Failed to load the JellyBoot.EFI!\nReason: %r\n", Status);
    }

    // Start JellyBoot.EFI and check if it started successfully
    Status = uefi_call_wrapper(
        SystemTable->BootServices->StartImage,
        3, 
        LoadedImageHandle,
        NULL,
        NULL
    );
    if(EFI_ERROR(Status)) {
        Print(L"Failed to start the JellyBoot.EFI!\nReason %r\n", Status);
    } else {
        uefi_call_wrapper(
            SystemTable->RuntimeServices->ResetSystem,
            4,
            EfiResetWarm,
            EFI_SUCCESS,
            0,
            NULL
        );
    }

    return EFI_SUCCESS;
}

// End of file
