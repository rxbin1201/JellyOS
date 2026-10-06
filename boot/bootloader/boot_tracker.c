#include "boot_tracker.h"

#include "log.h"

#include <jelly/boot_info.h>

#define VARIABLE_NAME       L"" BOOT_STATE_VARIABLE_NAME
#define VARIABLE_ATTRIBUTES (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | \
                             EFI_VARIABLE_RUNTIME_ACCESS)

static EFI_GUID vendor_guid = BOOT_STATE_VENDOR_GUID;

const CHAR16 *boot_state_name(uint32_t state)
{
    static const CHAR16 *const names[] = {
        [BOOT_STATE_NONE]           = L"NONE",
        [BOOT_STARTED]              = L"BOOT_STARTED",
        [BOOT_CONFIGURATION_LOADED] = L"BOOT_CONFIGURATION_LOADED",
        [KERNEL_LOADED]             = L"KERNEL_LOADED",
        [KERNEL_STARTED]            = L"KERNEL_STARTED",
        [BOOT_SUCCESS]              = L"BOOT_SUCCESS",
        [BOOT_FAILED]               = L"BOOT_FAILED",
        [RECOVERY_REQUESTED]        = L"RECOVERY_REQUESTED",
    };
    return state < ARRAY_SIZE(names) ? names[state] : L"UNKNOWN";
}

const CHAR16 *boot_mode_name(uint32_t mode)
{
    switch (mode) {
    case BOOT_MODE_NORMAL:   return L"normal";
    case BOOT_MODE_FALLBACK: return L"previous kernel";
    case BOOT_MODE_RECOVERY: return L"recovery";
    case BOOT_MODE_MANUAL:   return L"manual";
    default:                 return L"unknown";
    }
}

static void reset_record(boot_state_record_t *r)
{
    ZeroMem(r, sizeof(*r));
    r->version = BOOT_STATE_RECORD_VERSION;
    r->size = sizeof(*r);
}

static EFI_STATUS read_record(boot_state_record_t *r)
{
    UINTN size = sizeof(*r);
    UINT32 attributes;

    EFI_STATUS status = RT->GetVariable(VARIABLE_NAME, &vendor_guid, &attributes, &size, r);
    if (EFI_ERROR(status))
        return status;
    if (size != sizeof(*r) || r->version != BOOT_STATE_RECORD_VERSION || r->size != sizeof(*r))
        return EFI_INCOMPATIBLE_VERSION;
    return EFI_SUCCESS;
}

static EFI_STATUS write_record(const boot_state_record_t *r)
{
    return RT->SetVariable(VARIABLE_NAME, &vendor_guid, VARIABLE_ATTRIBUTES, sizeof(*r), (void *)r);
}

static void evaluate_previous_boot(boot_tracker_t *t)
{
    boot_state_record_t *r = &t->record;
    bool tracked = r->flags & BOOT_STATE_FLAG_TRACKED;

    switch (r->state) {
    case KERNEL_STARTED:
        if (!tracked) {
            r->last_result = BOOT_STATE_NONE;
            break;
        }
        t->previous_failed = true;
        r->last_result = BOOT_FAILED;
        if (r->mode == BOOT_MODE_NORMAL)
            r->normal_failures++;
        else if (r->mode == BOOT_MODE_FALLBACK)
            r->fallback_failures++;
        log_warn(L"Previous boot (%s) did not report success", boot_mode_name(r->mode));
        break;

    case BOOT_SUCCESS:
        r->last_result = BOOT_SUCCESS;
        /* A healthy current kernel clears all failures. A healthy fallback
         * only clears its own counter: the current kernel stays broken. */
        if (r->mode == BOOT_MODE_NORMAL)
            r->normal_failures = r->fallback_failures = 0;
        else if (r->mode == BOOT_MODE_FALLBACK)
            r->fallback_failures = 0;
        break;

    case RECOVERY_REQUESTED:
        t->recovery_requested = true;
        r->last_result = BOOT_STATE_NONE;
        log_info(L"Recovery was requested by the system");
        break;

    case BOOT_STARTED:
    case BOOT_CONFIGURATION_LOADED:
    case KERNEL_LOADED:
        log_info(L"Previous boot stopped in the boot manager (%s)", boot_state_name(r->state));
        break;

    default:
        break;
    }
}

void boot_tracker_init(boot_tracker_t *t)
{
    ZeroMem(t, sizeof(*t));

    EFI_STATUS status = read_record(&t->record);
    if (status == EFI_NOT_FOUND) {
        reset_record(&t->record);
    } else if (EFI_ERROR(status)) {
        log_warn(L"Boot state record unreadable (%r), starting fresh", status);
        reset_record(&t->record);
    }

    t->previous_state = t->record.state;
    evaluate_previous_boot(t);
    t->record.boot_count++;
    t->record.state = BOOT_STARTED;

    status = write_record(&t->record);
    t->available = !EFI_ERROR(status);
    if (!t->available)
        log_warn(L"Boot state cannot be stored (%r), automatic rollback disabled", status);
}

void boot_tracker_begin(boot_tracker_t *t, uint32_t mode, bool kernel_reports_success)
{
    t->record.mode = mode;
    t->record.flags = kernel_reports_success ? BOOT_STATE_FLAG_TRACKED : 0;
}

void boot_tracker_set(boot_tracker_t *t, boot_state_t state)
{
    if (!t->available || t->record.state == (uint32_t)state)
        return;

    t->record.state = state;
    EFI_STATUS status = write_record(&t->record);
    if (EFI_ERROR(status)) {
        t->available = false;
        log_warn(L"Boot state cannot be stored (%r), automatic rollback disabled", status);
    }
}

void boot_tracker_decide(const boot_tracker_t *t, const boot_config_t *config, boot_decision_t *d)
{
    const boot_state_record_t *r = &t->record;
    UINTN max = config->max_attempts;

    d->entry = config->default_index;
    d->mode = BOOT_MODE_NORMAL;
    d->show_menu = false;
    d->reason[0] = L'\0';

    if (t->recovery_requested) {
        if (config->recovery_index != CONFIG_NO_ENTRY) {
            d->entry = config->recovery_index;
            d->mode = BOOT_MODE_RECOVERY;
            SPrint(d->reason, sizeof(d->reason), L"Recovery was requested by the system");
        } else {
            d->show_menu = true;
            SPrint(d->reason, sizeof(d->reason), L"Recovery was requested, but no recovery entry exists");
        }
        return;
    }

    if (r->normal_failures < max) {
        if (t->previous_failed)
            SPrint(d->reason, sizeof(d->reason), L"Previous boot failed (%d of %d attempts used)",
                   r->normal_failures, max);
        return;
    }

    if (config->fallback_kernel[0] && r->fallback_failures < max) {
        d->mode = BOOT_MODE_FALLBACK;
        SPrint(d->reason, sizeof(d->reason), L"Current kernel failed %d times, rolling back to %a",
               r->normal_failures, config->fallback_kernel);
        return;
    }

    d->show_menu = true;
    if (config->recovery_index != CONFIG_NO_ENTRY) {
        d->entry = config->recovery_index;
        d->mode = BOOT_MODE_RECOVERY;
        SPrint(d->reason, sizeof(d->reason), L"Current and previous kernel failed, selecting recovery");
    } else {
        SPrint(d->reason, sizeof(d->reason), L"Current and previous kernel failed, no recovery entry exists");
    }
}
