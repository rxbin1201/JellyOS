#include "menu.h"

#include "diagnostics.h"

#include <jelly/boot_info.h>

#define MAX_ITEMS   (CONFIG_MAX_ENTRIES + 4)
#define ONE_SECOND  10000000ULL /* in 100 ns units */

typedef enum {
    ITEM_ENTRY,
    ITEM_FALLBACK,
    ITEM_DIAGNOSTICS,
    ITEM_REBOOT,
    ITEM_EXIT,
} item_kind_t;

typedef struct {
    item_kind_t kind;
    UINTN       entry;
} item_t;

static UINTN build_items(const boot_config_t *config, item_t *items)
{
    UINTN n = 0;

    for (UINTN i = 0; i < config->entry_count; i++)
        items[n++] = (item_t){ ITEM_ENTRY, i };
    if (config->fallback_kernel[0])
        items[n++] = (item_t){ ITEM_FALLBACK, config->default_index };
    items[n++] = (item_t){ ITEM_DIAGNOSTICS, 0 };
    items[n++] = (item_t){ ITEM_REBOOT, 0 };
    items[n++] = (item_t){ ITEM_EXIT, 0 };
    return n;
}

static UINTN preselected_item(const item_t *items, UINTN count, const boot_decision_t *decision)
{
    item_kind_t kind = decision->mode == BOOT_MODE_FALLBACK ? ITEM_FALLBACK : ITEM_ENTRY;

    for (UINTN i = 0; i < count; i++) {
        if (items[i].kind == kind && items[i].entry == decision->entry)
            return i;
    }
    return 0;
}

static uint32_t entry_mode(const boot_config_t *config, UINTN entry)
{
    if (entry == config->default_index)
        return BOOT_MODE_NORMAL;
    if (entry == config->recovery_index)
        return BOOT_MODE_RECOVERY;
    return BOOT_MODE_MANUAL;
}

/* Wait for a key, at most one second if timed. Returns false on timeout. */
static bool wait_key(EFI_EVENT timer, bool timed, EFI_INPUT_KEY *key)
{
    EFI_EVENT events[2] = { ST->ConIn->WaitForKey, timer };
    UINTN index;

    if (timed)
        BS->SetTimer(timer, TimerRelative, ONE_SECOND);
    if (EFI_ERROR(BS->WaitForEvent(timed ? 2 : 1, events, &index)) || index != 0)
        return false;
    return !EFI_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, key));
}

static void print_item(const boot_config_t *config, const item_t *item)
{
    const char *name = config->entries[item->entry].name;

    switch (item->kind) {
    case ITEM_ENTRY:
        Print(L"%a", name);
        if (item->entry == config->default_index)
            Print(L" (default)");
        break;
    case ITEM_FALLBACK:
        Print(L"%a (previous kernel)", name);
        break;
    case ITEM_DIAGNOSTICS:
        Print(L"Diagnostics");
        break;
    case ITEM_REBOOT:
        Print(L"Reboot");
        break;
    case ITEM_EXIT:
        Print(L"Exit to firmware");
        break;
    }
}

static void draw(const boot_config_t *config, const item_t *items, UINTN count, UINTN selected,
                 const boot_decision_t *decision, UINTN countdown)
{
    ST->ConOut->ClearScreen(ST->ConOut);
    Print(L"JellyOS Boot Manager\r\n\r\n");
    if (decision->reason[0])
        Print(L"  ! %s\r\n\r\n", decision->reason);

    for (UINTN i = 0; i < count; i++) {
        Print(L"  %s %d. ", i == selected ? L">" : L" ", i + 1);
        print_item(config, &items[i]);
        Print(L"\r\n");
    }

    Print(L"\r\n  Up/Down select, Enter or 1-9 start, D diagnostics\r\n");
    if (countdown)
        Print(L"  Starting the selected item in %d s\r\n", countdown);
}

menu_action_t menu_run(EFI_HANDLE image, const boot_config_t *config, const boot_tracker_t *tracker,
                       const boot_decision_t *decision, UINTN countdown)
{
    item_t items[MAX_ITEMS];
    UINTN count = build_items(config, items);
    UINTN selected = preselected_item(items, count, decision);
    menu_action_t action = { .kind = ACTION_BOOT };
    EFI_EVENT timer = NULL;
    EFI_INPUT_KEY key;

    if (EFI_ERROR(BS->CreateEvent(EVT_TIMER, 0, NULL, NULL, &timer)))
        countdown = 0;

    for (;;) {
        draw(config, items, count, selected, decision, countdown);

        bool activate = false;
        if (!wait_key(timer, countdown > 0, &key)) {
            if (countdown == 0 || --countdown > 0)
                continue;
            activate = true; /* countdown expired: boot the preselected item */
        } else {
            action.chosen_by_user = true;
            countdown = 0;

            if (key.ScanCode == SCAN_UP) {
                selected = (selected + count - 1) % count;
            } else if (key.ScanCode == SCAN_DOWN) {
                selected = (selected + 1) % count;
            } else if (key.UnicodeChar >= L'1' && key.UnicodeChar <= L'9' &&
                       (UINTN)(key.UnicodeChar - L'1') < count) {
                selected = key.UnicodeChar - L'1';
                activate = true;
            } else if (key.UnicodeChar == L'd' || key.UnicodeChar == L'D') {
                diagnostics_show(image, config, tracker);
            } else if (key.UnicodeChar == CHAR_CARRIAGE_RETURN || key.UnicodeChar == CHAR_LINEFEED) {
                activate = true;
            }
        }
        if (!activate)
            continue;

        const item_t *item = &items[selected];
        switch (item->kind) {
        case ITEM_DIAGNOSTICS:
            diagnostics_show(image, config, tracker);
            continue;
        case ITEM_ENTRY:
            action.entry = item->entry;
            action.mode = entry_mode(config, item->entry);
            break;
        case ITEM_FALLBACK:
            action.entry = item->entry;
            action.mode = BOOT_MODE_FALLBACK;
            break;
        case ITEM_REBOOT:
            action.kind = ACTION_REBOOT;
            break;
        case ITEM_EXIT:
            action.kind = ACTION_EXIT;
            break;
        }
        break;
    }

    if (timer)
        BS->CloseEvent(timer);
    ST->ConOut->ClearScreen(ST->ConOut);
    return action;
}

bool menu_prompt(const boot_config_t *config, const boot_decision_t *decision)
{
    const char *name = config->entries[decision->entry].name;
    EFI_EVENT timer;
    EFI_INPUT_KEY key;
    bool pressed = false;

    if (EFI_ERROR(BS->CreateEvent(EVT_TIMER, 0, NULL, NULL, &timer)))
        return false;

    for (UINTN left = config->timeout; left > 0 && !pressed; left--) {
        Print(L"\rBooting '%a' in %d s, press any key for the boot menu ", name, left);
        pressed = wait_key(timer, true, &key);
    }
    Print(L"\r\n");

    BS->CloseEvent(timer);
    return pressed;
}
