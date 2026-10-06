#include "diagnostics.h"

#include "cpu.h"
#include "firmware.h"
#include "memory_map.h"
#include "text.h"

/* EFI_MP_SERVICES_PROTOCOL (PI specification); only the first member is used. */
#define MP_SERVICES_PROTOCOL_GUID \
    { 0x3fdda605, 0xa76e, 0x4f46, { 0xad, 0x29, 0x12, 0xf4, 0x53, 0x1b, 0x3d, 0x08 } }

typedef struct mp_services mp_services_t;
struct mp_services {
    EFI_STATUS (EFIAPI *GetNumberOfProcessors)(mp_services_t *self, UINTN *total, UINTN *enabled);
};

static void copy_registers(char *dest, const uint32_t *regs, UINTN count)
{
    CopyMem(dest, regs, count * sizeof(uint32_t));
    dest[count * sizeof(uint32_t)] = '\0';
}

void diagnostics_collect_cpu(boot_cpu_info_t *cpu)
{
    uint32_t a, b, c, d;

    cpu_cpuid(0, &a, &b, &c, &d);
    uint32_t vendor[3] = { b, d, c };
    copy_registers(cpu->vendor, vendor, 3);

    cpu_cpuid(1, &a, &b, &c, &d);
    cpu->stepping = a & 0xF;
    cpu->model = (a >> 4) & 0xF;
    cpu->family = (a >> 8) & 0xF;
    if (cpu->family == 0xF)
        cpu->family += (a >> 20) & 0xFF;
    if (cpu->family == 0x6 || cpu->family >= 0xF)
        cpu->model += ((a >> 16) & 0xF) << 4;

    cpu_cpuid(0x80000000, &a, &b, &c, &d);
    if (a >= 0x80000004) {
        uint32_t brand[12];
        for (uint32_t i = 0; i < 3; i++)
            cpu_cpuid(0x80000002 + i, &brand[i * 4], &brand[i * 4 + 1], &brand[i * 4 + 2], &brand[i * 4 + 3]);
        char raw[sizeof(brand) + 1];
        copy_registers(raw, brand, 12);
        const char *start = raw;
        while (*start == ' ')
            start++;
        text_copy(cpu->brand, sizeof(cpu->brand), start);
    }

    EFI_GUID mp_guid = MP_SERVICES_PROTOCOL_GUID;
    mp_services_t *mp;
    UINTN total, enabled;
    if (!EFI_ERROR(LibLocateProtocol(&mp_guid, (void **)&mp)) &&
        !EFI_ERROR(mp->GetNumberOfProcessors(mp, &total, &enabled)))
        cpu->logical_cpus = (uint32_t)enabled;
}

void diagnostics_collect_boot_device(EFI_HANDLE image, boot_device_t *device)
{
    EFI_LOADED_IMAGE *loaded;

    if (EFI_ERROR(BS->HandleProtocol(image, &LoadedImageProtocol, (void **)&loaded)))
        return;

    EFI_DEVICE_PATH *path = DevicePathFromHandle(loaded->DeviceHandle);
    if (!path)
        return;

    CHAR16 *text = DevicePathToStr(path);
    if (text) {
        text_from_wide(device->device_path, sizeof(device->device_path), text);
        FreePool(text);
    }

    for (EFI_DEVICE_PATH *node = path; !IsDevicePathEnd(node); node = NextDevicePathNode(node)) {
        if (DevicePathType(node) != MEDIA_DEVICE_PATH || DevicePathSubType(node) != MEDIA_HARDDRIVE_DP)
            continue;

        const HARDDRIVE_DEVICE_PATH *hd = (const HARDDRIVE_DEVICE_PATH *)node;
        device->partition_number = hd->PartitionNumber;
        if (hd->SignatureType == SIGNATURE_TYPE_GUID) {
            device->partition_type = BOOT_PARTITION_GPT;
            CopyMem(device->partition_id, hd->Signature, 16);
        } else if (hd->SignatureType == SIGNATURE_TYPE_MBR) {
            device->partition_type = BOOT_PARTITION_MBR;
            CopyMem(device->partition_id, hd->Signature, 4);
        }
        break;
    }
}

static const CHAR16 *partition_type_name(uint32_t type)
{
    switch (type) {
    case BOOT_PARTITION_GPT: return L"GPT";
    case BOOT_PARTITION_MBR: return L"MBR";
    default:                 return L"unknown";
    }
}

static void print_flags(uint64_t flags)
{
    static const struct { uint64_t bit; const CHAR16 *name; } names[] = {
        { BOOT_FLAG_DEBUG, L"debug" },
        { BOOT_FLAG_SAFE_MODE, L"safe_mode" },
        { BOOT_FLAG_RECOVERY, L"recovery" },
        { BOOT_FLAG_SECURE_BOOT, L"secure_boot" },
        { BOOT_FLAG_ROLLBACK, L"rollback" },
    };
    bool any = false;

    for (UINTN i = 0; i < ARRAY_SIZE(names); i++) {
        if (flags & names[i].bit) {
            Print(L"%s%s", any ? L", " : L"", names[i].name);
            any = true;
        }
    }
    Print(any ? L"\r\n" : L"none\r\n");
}

void diagnostics_print(const boot_info_t *info, const boot_entry_t *entry, const char *kernel_path,
                       const boot_tracker_t *tracker)
{
    const boot_framebuffer_t *fb = &info->framebuffer;
    const boot_state_record_t *r = &tracker->record;

    Print(L"Firmware      %a (revision 0x%x), UEFI %d.%d\r\n", info->uefi.firmware_vendor,
          info->uefi.firmware_revision, info->uefi.uefi_revision >> 16, info->uefi.uefi_revision & 0xFFFF);
    Print(L"CPU           %a\r\n", info->cpu.brand[0] ? info->cpu.brand : info->cpu.vendor);
    Print(L"              %a family 0x%x model 0x%x stepping %d\r\n", info->cpu.vendor,
          info->cpu.family, info->cpu.model, info->cpu.stepping);
    if (info->cpu.logical_cpus)
        Print(L"CPU cores     %d logical CPUs\r\n", info->cpu.logical_cpus);
    else
        Print(L"CPU cores     unknown\r\n");
    Print(L"Memory        %ld MiB\r\n", memory_map_total_ram() / (1024 * 1024));
    Print(L"ACPI          RSDP 0x%lx, revision %d\r\n", info->acpi.rsdp_phys, info->acpi.revision);
    Print(L"SMBIOS        %d.x entry at 0x%lx\r\n", info->smbios.major, info->smbios.entry_phys);
    if (fb->phys_base)
        Print(L"Framebuffer   %dx%d, pitch %d, at 0x%lx\r\n", fb->width, fb->height, fb->pitch, fb->phys_base);
    else
        Print(L"Framebuffer   none\r\n");
    Print(L"Secure Boot   %s\r\n", (info->flags & BOOT_FLAG_SECURE_BOOT) ? L"enabled" : L"disabled");
    Print(L"Boot device   %s partition %d\r\n", partition_type_name(info->boot_device.partition_type),
          info->boot_device.partition_number);
    Print(L"              %a\r\n", info->boot_device.device_path);

    Print(L"Entry         %a\r\n", entry->name);
    Print(L"Kernel        %a\r\n", kernel_path);
    Print(L"Initramfs     %a\r\n", entry->initrd[0] ? entry->initrd : "none");
    Print(L"Modules       %d\r\n", entry->module_count);
    for (UINTN i = 0; i < entry->module_count; i++)
        Print(L"              %a\r\n", entry->modules[i]);
    Print(L"Command line  %a\r\n", entry->cmdline);
    Print(L"Boot flags    ");
    print_flags(info->flags);

    Print(L"Boot state    previous %s, last result %s, boot #%d\r\n", boot_state_name(tracker->previous_state),
          boot_state_name(r->last_result), r->boot_count);
    Print(L"Failures      current kernel %d, previous kernel %d%s\r\n", r->normal_failures,
          r->fallback_failures, tracker->available ? L"" : L" (state storage unavailable)");
}

void diagnostics_show(EFI_HANDLE image, const boot_config_t *config, const boot_tracker_t *tracker)
{
    static boot_info_t info;
    const boot_entry_t *entry = &config->entries[config->default_index];
    UINTN index;
    EFI_INPUT_KEY key;

    ZeroMem(&info, sizeof(info));
    firmware_collect(ST, &info);
    firmware_get_framebuffer(&info.framebuffer);
    diagnostics_collect_cpu(&info.cpu);
    diagnostics_collect_boot_device(image, &info.boot_device);
    if (text_has_token(entry->cmdline, "debug=1"))
        info.flags |= BOOT_FLAG_DEBUG;

    ST->ConOut->ClearScreen(ST->ConOut);
    Print(L"JellyOS Boot Manager - Diagnostics\r\n\r\n");
    diagnostics_print(&info, entry, entry->kernel, tracker);
    Print(L"\r\nPress any key to return to the boot menu.\r\n");

    BS->WaitForEvent(1, &ST->ConIn->WaitForKey, &index);
    ST->ConIn->ReadKeyStroke(ST->ConIn, &key);
}
