#include "firmware.h"

#include "log.h"
#include "text.h"

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

bool firmware_secure_boot_enabled(void)
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
    if (firmware_secure_boot_enabled())
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

/* A graphics output with a framebuffer we can draw into after the firmware is gone. */
static BOOLEAN gop_is_linear(const EFI_GRAPHICS_OUTPUT_PROTOCOL *gop)
{
    if (!gop->Mode || !gop->Mode->Info || !gop->Mode->FrameBufferBase)
        return FALSE;
    UINT32 format = gop->Mode->Info->PixelFormat;
    return format == PixelRedGreenBlueReserved8BitPerColor || format == PixelBlueGreenRedReserved8BitPerColor ||
           format == PixelBitMask;
}

/*
 * Firmware often has several graphics outputs: one per connector, and a
 * virtual one of the console that may have no framebuffer at all. The first
 * one found is not necessarily the screen the user looks at. Prefer an
 * output with a linear framebuffer whose monitor answered (active EDID),
 * then a real device (device path), then any linear one.
 */
static EFI_GRAPHICS_OUTPUT_PROTOCOL *find_gop(bool report, EFI_HANDLE *handle)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *best = NULL;
    EFI_HANDLE *handles = NULL;
    UINTN count = 0, best_score = 0, linear = 0;

    if (EFI_ERROR(LibLocateHandle(ByProtocol, &GraphicsOutputProtocol, NULL, &count, &handles)) || !count) {
        if (report)
            log_warn(L"No graphics output protocol, booting without framebuffer");
        return NULL;
    }
    for (UINTN i = 0; i < count; i++) {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
        EFI_EDID_ACTIVE_PROTOCOL *edid;
        void *path;
        if (EFI_ERROR(BS->HandleProtocol(handles[i], &GraphicsOutputProtocol, (void **)&gop)) || !gop_is_linear(gop))
            continue;
        linear++;
        UINTN score = 1;
        if (!EFI_ERROR(BS->HandleProtocol(handles[i], &DevicePathProtocol, &path)))
            score += 1;
        if (!EFI_ERROR(BS->HandleProtocol(handles[i], &EdidActiveProtocol, (void **)&edid)) && edid->SizeOfEdid)
            score += 2;
        if (score > best_score) {
            best_score = score;
            best = gop;
            if (handle)
                *handle = handles[i];
        }
    }
    FreePool(handles);
    if (!report)
        return best;
    if (!best)
        log_warn(L"None of %d graphics outputs has a linear framebuffer: the kernel will have no screen", count);
    else
        log_info(L"Screen: %dx%d, framebuffer at 0x%lx (%d graphics outputs, %d usable)",
                 best->Mode->Info->HorizontalResolution, best->Mode->Info->VerticalResolution,
                 best->Mode->FrameBufferBase, count, linear);
    return best;
}

static bool mode_is_linear(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info)
{
    return info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ||
           info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor || info->PixelFormat == PixelBitMask;
}

/* "1920x1080" */
static bool parse_resolution(const char *text, UINT32 *width, UINT32 *height)
{
    UINT32 value[2] = { 0, 0 };
    int part = 0;

    for (const char *c = text; *c; c++) {
        if (*c == 'x' && part == 0 && c != text) {
            part = 1;
        } else if (*c >= '0' && *c <= '9' && value[part] < 100000) {
            value[part] = value[part] * 10 + (UINT32)(*c - '0');
        } else {
            return false;
        }
    }
    *width = value[0];
    *height = value[1];
    return part == 1 && value[0] && value[1];
}

void firmware_select_resolution(const char *setting)
{
    EFI_HANDLE handle = NULL;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_EDID_ACTIVE_PROTOCOL *edid;
    UINT32 wanted_w = 0, wanted_h = 0, native_w = 0, native_h = 0;
    UINT32 best = 0, best_w = 0, best_h = 0, largest_w = 0, largest_h = 0, modes = 0;
    bool found = false;

    if (text_equal(setting, "keep"))
        return;
    if (!text_equal(setting, "max") && !parse_resolution(setting, &wanted_w, &wanted_h)) {
        log_warn(L"resolution=%a is not max, keep or WIDTHxHEIGHT; using max", setting);
    }
    gop = find_gop(false, &handle);
    if (!gop)
        return;

    /* The monitor's native resolution: the first detailed timing of its EDID. */
    if (!EFI_ERROR(BS->HandleProtocol(handle, &EdidActiveProtocol, (void **)&edid)) && edid->SizeOfEdid >= 128 &&
        edid->Edid && (edid->Edid[54] || edid->Edid[55])) {
        const UINT8 *timing = edid->Edid + 54;
        native_w = timing[2] | (UINT32)(timing[4] & 0xF0) << 4;
        native_h = timing[5] | (UINT32)(timing[7] & 0xF0) << 4;
    }

    /* A monitor that does not say what it can show gets at most Full HD: every current screen handles that. */
    UINT32 limit_w = native_w ? native_w : 1920, limit_h = native_w ? native_h : 1080;

    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        UINTN size = 0;
        if (EFI_ERROR(gop->QueryMode(gop, m, &size, &info)) || !info)
            continue;
        UINT32 w = info->HorizontalResolution, h = info->VerticalResolution;
        bool usable = mode_is_linear(info);
        FreePool(info);
        if (!usable)
            continue;
        modes++;
        if ((UINT64)w * h > (UINT64)largest_w * largest_h) {
            largest_w = w;
            largest_h = h;
        }
        bool better;
        if (wanted_w)
            better = w == wanted_w && h == wanted_h;
        else if (w > limit_w || h > limit_h)
            better = false; /* more than the monitor shows */
        else
            better = (UINT64)w * h > (UINT64)best_w * best_h;
        if (better) {
            best = m;
            best_w = w;
            best_h = h;
            found = true;
        }
    }

    if (wanted_w && !found) {
        log_warn(L"The firmware has no %dx%d mode (largest of %d: %dx%d); keeping the current one", wanted_w, wanted_h,
                 modes, largest_w, largest_h);
        return;
    }
    if (!found)
        return;
    /* Firmware without a driver for the graphics card's full range offers only a few small modes. */
    if (!wanted_w && native_w && (best_w != native_w || best_h != native_h))
        log_warn(L"The monitor has %dx%d, but the firmware offers at most %dx%d (%d modes)", native_w, native_h,
                 best_w, best_h, modes);
    if (best == gop->Mode->Mode)
        return;
    EFI_STATUS status = gop->SetMode(gop, best);
    if (EFI_ERROR(status))
        log_warn(L"Cannot switch the screen to %dx%d: %r", best_w, best_h, status);
}

void firmware_get_framebuffer(boot_framebuffer_t *fb)
{
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = find_gop(true, NULL);

    if (!gop)
        return;

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
