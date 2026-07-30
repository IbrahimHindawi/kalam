#ifndef KALAM_STANDALONE_STANDALONE_HOST_H
#define KALAM_STANDALONE_STANDALONE_HOST_H

#include <stdint.h>

#include "game_api.h"

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * Converts one Win32 virtual-key message into the engine-owned Game_Key ABI.
 * scan_code is the eight-bit WM_KEY* scan code and is_extended is the message
 * extended-key bit. The conversion is explicit; Game_Key values never alias
 * or persist Win32 virtual-key ordinals.
 */
Game_Key standalone_translate_win32_key(
    uint32_t virtual_key,
    uint32_t scan_code,
    uint32_t is_extended);

/*
 * Console-subsystem entry used by game.exe and by executable-level smoke
 * tests. Arguments and file paths remain UTF-16 until file I/O.
 */
int standalone_run(int argument_count, wchar_t** arguments);

#if defined(__cplusplus)
}
#endif

#endif /* KALAM_STANDALONE_STANDALONE_HOST_H */
