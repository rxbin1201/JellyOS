#include "firmware.h"

#include "log.h"

static void *find_config_table(EFI_SYSTEM_TABLE *st, EFI_GUID *guid)
{
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        if (CompareGuid(&st->ConfigurationTable[i].VendorGuid, guid) == 0)
            return st->ConfigurationTable[i].VendorTable;
    }
    return NULL;
}

static void collect_acpi(EFI_SYSTEM_TABLE *st, boot_acpi_info_t *acpi)
{
    EFI_GUID acpi20 = ACPI_20_TABLE_GUID;
    EFI_GUID acpi10 = ACPI_TABLE_GUID;
    const UINT8 *rsdp = find_config_table(st, &acpi20);

    if (!rsdp)
        rsdp = find_config_table(st, &acpi10);
    if (!rsdp)
        return;

    acpi->rsdp_phys = (uint64_t)(UINTN)rsdp;
    acpi->revision = rsdp[15];
}

static void collect_smbios(EFI_SYSTEM_TABLE *st, boot_smbios_info_t *smbios)
{
    EFI_GUID smbios3 = SMBIOS3_TABLE_GUID;
    EFI_GUID smbios2 = SMBIOS_TABLE_GUID;
    void *entry;

    if ((entry = find_config_table(st, &smbios3))) {
        smbios->major = 3;
    } else if ((entry = find_config_table(st, &smbios2))) {
        smbios->major = 2;
    } else {
        return;
    }
    smbios->entry_phys = (uint64_t)(UINTN)entry;
}

static void collect_uefi(EFI_SYSTEM_TABLE *st, boot_uefi_info_t *uefi)
{
    uefi->system_table_phys = (uint64_t)(UINTN)st;
    uefi->uefi_revision = st->Hdr.Revision;
    uefi->firmware_revision = st->FirmwareRevision;

    UINTN i = 0;
    if (st->FirmwareVendor) {
        for (; i < sizeof(uefi->firmware_vendor) - 1 && st->FirmwareVendor[i]; i++) {
            CHAR16 c = st->FirmwareVendor[i];
            uefi->firmware_vendor[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
    }
    uefi->firmware_vendor[i] = '\0';
}

static bool secure_boot_enabled(void)
{
    UINT8 value = 0;
    UINTN size = sizeof(value);

    if (EFI_ERROR(RT->GetVariable(L"SecureBoot", &EfiGlobalVariable, NULL, &size, &value)))
        return false;
    return size == sizeof(value) && value == 1;
}

void firmware_collect(EFI_SYSTEM_TABLE *st, boot_info_t *info)
{
    collect_acpi(st, &info->acpi);
    collect_smbios(st, &info->smbios);
    collect_uefi(st, &info->uefi);
    if (secure_boot_enabled())
        info->flags |= BOOT_FLAG_SECURE_BOOT;
}

static void mask_to_shift(UINT32 mask, uint8_t *shift, uint8_t *size)
{
    uint8_t s = 0, n = 0;

    while (mask && !(mask & 1)) {
        mask >>= 1;
        s++;
    }
    while (mask & 1) {
        mask >>= 1;
        n++;
    }
    *shift = s;
    *size = n;
}

void firmware_get_framebuffer(boot_framebuffer_t *fb)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;

    if (EFI_ERROR(LibLocateProtocol(&GraphicsOutputProtocol, (void **)&gop))) {
        log_warn(L"No graphics output protocol, booting without framebuffer");
        return;
    }

    const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mode = gop->Mode->Info;

    switch (mode->PixelFormat) {
    case PixelRedGreenBlueReserved8BitPerColor:
        fb->red_shift = 0;
        fb->green_shift = 8;
        fb->blue_shift = 16;
        fb->red_size = fb->green_size = fb->blue_size = 8;
        break;
    case PixelBlueGreenRedReserved8BitPerColor:
        fb->blue_shift = 0;
        fb->green_shift = 8;
        fb->red_shift = 16;
        fb->red_size = fb->green_size = fb->blue_size = 8;
        break;
    case PixelBitMask:
        mask_to_shift(mode->PixelInformation.RedMask, &fb->red_shift, &fb->red_size);
        mask_to_shift(mode->PixelInformation.GreenMask, &fb->green_shift, &fb->green_size);
        mask_to_shift(mode->PixelInformation.BlueMask, &fb->blue_shift, &fb->blue_size);
        break;
    default:
        log_warn(L"GOP mode has no linear framebuffer, booting without framebuffer");
        return;
    }

    fb->phys_base = gop->Mode->FrameBufferBase;
    fb->size = gop->Mode->FrameBufferSize;
    fb->width = mode->HorizontalResolution;
    fb->height = mode->VerticalResolution;
    fb->bpp = 32;
    fb->pitch = mode->PixelsPerScanLine * 4;
}
