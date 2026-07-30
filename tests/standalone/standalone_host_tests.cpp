#include "standalone_host.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stdio.h>

static uint32_t test_failure_count;

static void
test_check(
    uint32_t expression,
    const char* expression_text,
    int line)
{
    if (!expression)
    {
        printf(
            "standalone_host_tests.cpp(%d): check failed: %s\n",
            line,
            expression_text);
        test_failure_count += 1;
    }
}

#define TEST_CHECK(expression) \
    test_check((expression) != 0, #expression, __LINE__)

static void
test_explicit_alphanumeric_and_function_keys(void)
{
    TEST_CHECK(
        standalone_translate_win32_key('A', 0, 0) ==
        Game_Key_A);
    TEST_CHECK(
        standalone_translate_win32_key('Z', 0, 0) ==
        Game_Key_Z);
    TEST_CHECK(
        standalone_translate_win32_key('0', 0, 0) ==
        Game_Key_Digit_0);
    TEST_CHECK(
        standalone_translate_win32_key('9', 0, 0) ==
        Game_Key_Digit_9);
    TEST_CHECK(
        standalone_translate_win32_key(VK_F6, 0, 0) ==
        Game_Key_F6);
    TEST_CHECK(
        standalone_translate_win32_key(VK_F24, 0, 0) ==
        Game_Key_F24);
}

static void
test_modifier_and_extended_key_disambiguation(void)
{
    TEST_CHECK(
        standalone_translate_win32_key(VK_CONTROL, 0x1d, 0) ==
        Game_Key_Left_Control);
    TEST_CHECK(
        standalone_translate_win32_key(VK_CONTROL, 0x1d, 1) ==
        Game_Key_Right_Control);
    TEST_CHECK(
        standalone_translate_win32_key(VK_MENU, 0x38, 0) ==
        Game_Key_Left_Alt);
    TEST_CHECK(
        standalone_translate_win32_key(VK_MENU, 0x38, 1) ==
        Game_Key_Right_Alt);
    TEST_CHECK(
        standalone_translate_win32_key(VK_SHIFT, 0x2a, 0) ==
        Game_Key_Left_Shift);
    TEST_CHECK(
        standalone_translate_win32_key(VK_SHIFT, 0x36, 0) ==
        Game_Key_Right_Shift);
    TEST_CHECK(
        standalone_translate_win32_key(VK_RETURN, 0x1c, 0) ==
        Game_Key_Enter);
    TEST_CHECK(
        standalone_translate_win32_key(VK_RETURN, 0x1c, 1) ==
        Game_Key_Keypad_Enter);
}

static void
test_navigation_keypad_and_punctuation_mapping(void)
{
    TEST_CHECK(
        standalone_translate_win32_key(VK_INSERT, 0x52, 1) ==
        Game_Key_Insert);
    TEST_CHECK(
        standalone_translate_win32_key(VK_INSERT, 0x52, 0) ==
        Game_Key_Keypad_0);
    TEST_CHECK(
        standalone_translate_win32_key(VK_HOME, 0x47, 1) ==
        Game_Key_Home);
    TEST_CHECK(
        standalone_translate_win32_key(VK_HOME, 0x47, 0) ==
        Game_Key_Keypad_7);
    TEST_CHECK(
        standalone_translate_win32_key(VK_OEM_1, 0, 0) ==
        Game_Key_Semicolon);
    TEST_CHECK(
        standalone_translate_win32_key(VK_OEM_5, 0, 0) ==
        Game_Key_Backslash);
    TEST_CHECK(
        standalone_translate_win32_key(VK_PROCESSKEY, 0, 0) ==
        Game_Key_Unknown);
}

int
main(void)
{
    test_explicit_alphanumeric_and_function_keys();
    test_modifier_and_extended_key_disambiguation();
    test_navigation_keypad_and_punctuation_mapping();

    if (test_failure_count != 0)
    {
        printf(
            "%u standalone-host check(s) failed\n",
            (unsigned int)test_failure_count);
        return 1;
    }

    printf("all standalone-host key translation checks passed\n");
    return 0;
}
