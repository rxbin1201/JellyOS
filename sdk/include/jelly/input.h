/*
 * JellyOS key and button codes for jelly_input_event_t.
 *
 * Key codes follow the Linux evdev numbering (which VirtIO input and most
 * HID translation tables already use), so drivers pass them through. They
 * name physical keys on a US layout; turning them into characters is the
 * job of the keyboard layout (display server).
 */

#ifndef JELLY_INPUT_H
#define JELLY_INPUT_H

#define JELLY_KEY_ESCAPE      1
#define JELLY_KEY_1           2
#define JELLY_KEY_2           3
#define JELLY_KEY_3           4
#define JELLY_KEY_4           5
#define JELLY_KEY_5           6
#define JELLY_KEY_6           7
#define JELLY_KEY_7           8
#define JELLY_KEY_8           9
#define JELLY_KEY_9           10
#define JELLY_KEY_0           11
#define JELLY_KEY_MINUS       12
#define JELLY_KEY_EQUAL       13
#define JELLY_KEY_BACKSPACE   14
#define JELLY_KEY_TAB         15
#define JELLY_KEY_Q           16
#define JELLY_KEY_W           17
#define JELLY_KEY_E           18
#define JELLY_KEY_R           19
#define JELLY_KEY_T           20
#define JELLY_KEY_Y           21
#define JELLY_KEY_U           22
#define JELLY_KEY_I           23
#define JELLY_KEY_O           24
#define JELLY_KEY_P           25
#define JELLY_KEY_LEFTBRACE   26
#define JELLY_KEY_RIGHTBRACE  27
#define JELLY_KEY_ENTER       28
#define JELLY_KEY_LEFTCTRL    29
#define JELLY_KEY_A           30
#define JELLY_KEY_S           31
#define JELLY_KEY_D           32
#define JELLY_KEY_F           33
#define JELLY_KEY_G           34
#define JELLY_KEY_H           35
#define JELLY_KEY_J           36
#define JELLY_KEY_K           37
#define JELLY_KEY_L           38
#define JELLY_KEY_SEMICOLON   39
#define JELLY_KEY_APOSTROPHE  40
#define JELLY_KEY_GRAVE       41
#define JELLY_KEY_LEFTSHIFT   42
#define JELLY_KEY_BACKSLASH   43
#define JELLY_KEY_Z           44
#define JELLY_KEY_X           45
#define JELLY_KEY_C           46
#define JELLY_KEY_V           47
#define JELLY_KEY_B           48
#define JELLY_KEY_N           49
#define JELLY_KEY_M           50
#define JELLY_KEY_COMMA       51
#define JELLY_KEY_DOT         52
#define JELLY_KEY_SLASH       53
#define JELLY_KEY_RIGHTSHIFT  54
#define JELLY_KEY_KPASTERISK  55
#define JELLY_KEY_LEFTALT     56
#define JELLY_KEY_SPACE       57
#define JELLY_KEY_CAPSLOCK    58
#define JELLY_KEY_F1          59
#define JELLY_KEY_F2          60
#define JELLY_KEY_F3          61
#define JELLY_KEY_F4          62
#define JELLY_KEY_F5          63
#define JELLY_KEY_F6          64
#define JELLY_KEY_F7          65
#define JELLY_KEY_F8          66
#define JELLY_KEY_F9          67
#define JELLY_KEY_F10         68
#define JELLY_KEY_F11         87
#define JELLY_KEY_F12         88
#define JELLY_KEY_102ND       86  /* the extra key left of Z on ISO keyboards */
#define JELLY_KEY_KPENTER     96
#define JELLY_KEY_RIGHTCTRL   97
#define JELLY_KEY_RIGHTALT    100
#define JELLY_KEY_HOME        102
#define JELLY_KEY_UP          103
#define JELLY_KEY_PAGEUP      104
#define JELLY_KEY_LEFT        105
#define JELLY_KEY_RIGHT       106
#define JELLY_KEY_END         107
#define JELLY_KEY_DOWN        108
#define JELLY_KEY_PAGEDOWN    109
#define JELLY_KEY_INSERT      110
#define JELLY_KEY_DELETE      111
#define JELLY_KEY_LEFTMETA    125
#define JELLY_KEY_RIGHTMETA   126
#define JELLY_KEY_MAX         255

#define JELLY_BUTTON_LEFT     1
#define JELLY_BUTTON_RIGHT    2
#define JELLY_BUTTON_MIDDLE   3

#endif
