#include "file.h"

#include <jelly/boot_layout.h>

static EFI_STATUS open_file(EFI_HANDLE image, const CHAR16 *path, EFI_FILE_HANDLE *file, UINTN *size)
{
    EFI_LOADED_IMAGE *loaded;
    EFI_FILE_HANDLE root;
    EFI_FILE_INFO *info;
    EFI_STATUS status;

    status = BS->HandleProtocol(image, &LoadedImageProtocol, (void **)&loaded);
    if (EFI_ERROR(status))
        return status;

    root = LibOpenRoot(loaded->DeviceHandle);
    if (!root)
        return EFI_NOT_FOUND;

    status = root->Open(root, file, (CHAR16 *)path, EFI_FILE_MODE_READ, 0);
    root->Close(root);
    if (EFI_ERROR(status))
        return status;

    info = LibFileInfo(*file);
    if (!info) {
        (*file)->Close(*file);
        return EFI_DEVICE_ERROR;
    }
    *size = info->FileSize;
    FreePool(info);
    return EFI_SUCCESS;
}

static EFI_STATUS read_and_close(EFI_FILE_HANDLE file, void *buffer, UINTN size)
{
    UINTN read = size;
    EFI_STATUS status = file->Read(file, &read, buffer);

    file->Close(file);
    if (!EFI_ERROR(status) && read != size)
        status = EFI_END_OF_FILE;
    return status;
}

EFI_STATUS file_read_all(EFI_HANDLE image, const CHAR16 *path, void **data, UINTN *size)
{
    EFI_FILE_HANDLE file;
    UINTN file_size;
    EFI_STATUS status;

    status = open_file(image, path, &file, &file_size);
    if (EFI_ERROR(status))
        return status;

    UINT8 *buffer = AllocatePool(file_size + 1);
    if (!buffer) {
        file->Close(file);
        return EFI_OUT_OF_RESOURCES;
    }

    status = read_and_close(file, buffer, file_size);
    if (EFI_ERROR(status)) {
        FreePool(buffer);
        return status;
    }

    buffer[file_size] = 0;
    *data = buffer;
    *size = file_size;
    return EFI_SUCCESS;
}

EFI_STATUS file_load_pages(EFI_HANDLE image, const CHAR16 *path, EFI_MEMORY_TYPE type,
                           void **data, UINTN *size)
{
    EFI_FILE_HANDLE file;
    UINTN file_size;
    EFI_STATUS status;

    status = open_file(image, path, &file, &file_size);
    if (EFI_ERROR(status))
        return status;

    UINTN pages = align_up(file_size ? file_size : 1, BOOT_PAGE_SIZE) / BOOT_PAGE_SIZE;
    void *buffer = boot_alloc_pages(pages, type);
    if (!buffer) {
        file->Close(file);
        return EFI_OUT_OF_RESOURCES;
    }

    status = read_and_close(file, buffer, file_size);
    if (EFI_ERROR(status))
        return status; /* pages are released with the failed boot attempt */

    *data = buffer;
    *size = file_size;
    return EFI_SUCCESS;
}
