
# JellyOS 

JellyOS is a hobby project which is always being developed further.
I use GNU-EFI and EDK2 for the bootloader.
The operating system is written mainly with C.


## Features

- UEFI Bootloader
- Only supports x64 processors
- More coming soon
- UEFI Bootloader supports PSF1 Font File


## Feedback

If you have any ideas I should add let me know.


## JellyBoot Loader

The JellyBoot Loader is located at '\\EFI\\BOOT\\BOOTX64.EFI'.

It loads `\\EFI\\JellyBoot\\JellyBoot.EFI` located on the boot device such as a usb drive.

## JellyBoot

It is the main uefi bootloader that loads the system and has some other function.

It is located at '\\EFI\\JellyBoot' and to compile it I use edk2.
