#include "file.h"

EFI_STATUS file_read_all(EFI_HANDLE image, const CHAR16 *path, void **data, UINTN *size)
{
    EFI_LOADED_IMAGE *loaded;
    EFI_FILE_HANDLE root, file;
    EFI_FILE_INFO *info;
    EFI_STATUS status;

    status = BS->HandleProtocol(image, &LoadedImageProtocol, (void **)&loaded);
    if (EFI_ERROR(status))
        return status;

    root = LibOpenRoot(loaded->DeviceHandle);
    if (!root)
        return EFI_NOT_FOUND;

    status = root->Open(root, &file, (CHAR16 *)path, EFI_FILE_MODE_READ, 0);
    root->Close(root);
    if (EFI_ERROR(status))
        return status;

    info = LibFileInfo(file);
    if (!info) {
        file->Close(file);
        return EFI_DEVICE_ERROR;
    }
    UINTN file_size = info->FileSize;
    FreePool(info);

    void *buffer = AllocatePool(file_size ? file_size : 1);
    if (!buffer) {
        file->Close(file);
        return EFI_OUT_OF_RESOURCES;
    }

    UINTN read = file_size;
    status = file->Read(file, &read, buffer);
    file->Close(file);
    if (!EFI_ERROR(status) && read != file_size)
        status = EFI_END_OF_FILE;
    if (EFI_ERROR(status)) {
        FreePool(buffer);
        return status;
    }

    *data = buffer;
    *size = file_size;
    return EFI_SUCCESS;
}
