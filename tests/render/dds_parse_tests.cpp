/*
 * Byte-fixture tests for the first-party DDS container parser
 * (docs/resource_manifest.md section 8). No GPU, no files.
 */

#include "dds_parse.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t dds_test_failures = 0;

static void
dds_check(uint32_t condition, const char* message)
{
    if (!condition)
    {
        fprintf(stderr, "check failed: %s\n", message);
        dds_test_failures += 1;
    }
}

static void
dds_write_u32(uint8_t* bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xffu);
    bytes[1] = (uint8_t)((value >> 8) & 0xffu);
    bytes[2] = (uint8_t)((value >> 16) & 0xffu);
    bytes[3] = (uint8_t)((value >> 24) & 0xffu);
}

/*
 * Builds a DDS header into buffer. Returns the header size; the caller
 * appends pixel data.
 */
static uint32_t
dds_build_header(
    uint8_t* buffer,
    uint32_t width,
    uint32_t height,
    uint32_t pixel_flags,
    uint32_t pixel_fourcc,
    uint32_t bit_count,
    uint32_t red_mask,
    uint32_t green_mask,
    uint32_t blue_mask)
{
    memset(buffer, 0, 128);
    memcpy(buffer, "DDS ", 4);
    dds_write_u32(buffer + 4, 124);
    dds_write_u32(buffer + 8, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000);
    dds_write_u32(buffer + 12, height);
    dds_write_u32(buffer + 16, width);
    dds_write_u32(buffer + 76, 32);
    dds_write_u32(buffer + 80, pixel_flags);
    dds_write_u32(buffer + 84, pixel_fourcc);
    dds_write_u32(buffer + 88, bit_count);
    dds_write_u32(buffer + 92, red_mask);
    dds_write_u32(buffer + 96, green_mask);
    dds_write_u32(buffer + 100, blue_mask);
    dds_write_u32(buffer + 108, 0x1000);
    return 128;
}

static uint32_t
dds_fourcc(char a, char b, char c, char d)
{
    return (uint32_t)(uint8_t)a |
        ((uint32_t)(uint8_t)b << 8) |
        ((uint32_t)(uint8_t)c << 16) |
        ((uint32_t)(uint8_t)d << 24);
}

static void
test_bgra8(void)
{
    uint8_t buffer[128 + 4 * 4 * 4];
    Dds_Image image;
    Dds_Parse_Result result;

    const uint32_t header_size = dds_build_header(
        buffer, 4, 4, 0x40, 0, 32,
        0x00ff0000u, 0x0000ff00u, 0x000000ffu);
    memset(buffer + header_size, 0xab, sizeof(buffer) - header_size);

    result = dds_parse(buffer, sizeof(buffer), &image);
    dds_check(result == Dds_Parse_Result_Ok, "BGRA8 parses");
    dds_check(image.width == 4 && image.height == 4, "BGRA8 dimensions");
    dds_check(image.format == Dds_Format_B8G8R8A8, "BGRA8 format");
    dds_check(image.row_pitch == 16, "BGRA8 row pitch");
    dds_check(image.data_size == 64, "BGRA8 data size");
    dds_check(
        image.data == buffer + header_size,
        "BGRA8 data points past the header");
    dds_check(image.data[0] == 0xab, "BGRA8 pixels are reachable");
}

static void
test_rgba8(void)
{
    uint8_t buffer[128 + 2 * 2 * 4];
    Dds_Image image;

    dds_build_header(
        buffer, 2, 2, 0x40, 0, 32,
        0x000000ffu, 0x0000ff00u, 0x00ff0000u);
    memset(buffer + 128, 0, sizeof(buffer) - 128);

    dds_check(
        dds_parse(buffer, sizeof(buffer), &image) == Dds_Parse_Result_Ok &&
            image.format == Dds_Format_R8G8B8A8,
        "RGBA8 mask order is recognized");
}

static void
test_block_compressed(void)
{
    /* 8x8 BC1: 2x2 blocks of 8 bytes. */
    uint8_t bc1[128 + 4 * 8];
    /* 8x8 BC3: 2x2 blocks of 16 bytes. */
    uint8_t bc3[128 + 4 * 16];
    Dds_Image image;

    dds_build_header(bc1, 8, 8, 0x4, dds_fourcc('D', 'X', 'T', '1'), 0, 0, 0, 0);
    memset(bc1 + 128, 0, sizeof(bc1) - 128);
    dds_check(
        dds_parse(bc1, sizeof(bc1), &image) == Dds_Parse_Result_Ok &&
            image.format == Dds_Format_BC1 &&
            image.row_pitch == 16 &&
            image.data_size == 32,
        "BC1 block geometry");

    dds_build_header(bc3, 8, 8, 0x4, dds_fourcc('D', 'X', 'T', '5'), 0, 0, 0, 0);
    memset(bc3 + 128, 0, sizeof(bc3) - 128);
    dds_check(
        dds_parse(bc3, sizeof(bc3), &image) == Dds_Parse_Result_Ok &&
            image.format == Dds_Format_BC3 &&
            image.row_pitch == 32 &&
            image.data_size == 64,
        "BC3 block geometry");

    /* Non-multiple-of-four dimensions round up to whole blocks. */
    uint8_t odd[128 + 4 * 8];
    dds_build_header(odd, 5, 5, 0x4, dds_fourcc('D', 'X', 'T', '1'), 0, 0, 0, 0);
    memset(odd + 128, 0, sizeof(odd) - 128);
    dds_check(
        dds_parse(odd, sizeof(odd), &image) == Dds_Parse_Result_Ok &&
            image.row_pitch == 16 &&
            image.data_size == 32,
        "BC1 rounds partial blocks up");
}

static void
test_rejections(void)
{
    uint8_t buffer[128 + 64];
    Dds_Image image;

    dds_check(
        dds_parse(0, 16, &image) == Dds_Parse_Result_Invalid_Argument,
        "a null buffer is rejected");
    dds_check(
        dds_parse(buffer, 128, 0) == Dds_Parse_Result_Invalid_Argument,
        "a null output is rejected");

    dds_build_header(
        buffer, 4, 4, 0x40, 0, 32,
        0x00ff0000u, 0x0000ff00u, 0x000000ffu);
    memcpy(buffer, "XXXX", 4);
    dds_check(
        dds_parse(buffer, sizeof(buffer), &image) ==
            Dds_Parse_Result_Bad_Magic,
        "wrong magic is rejected");

    dds_build_header(
        buffer, 4, 4, 0x40, 0, 32,
        0x00ff0000u, 0x0000ff00u, 0x000000ffu);
    dds_check(
        dds_parse(buffer, 20, &image) == Dds_Parse_Result_Bad_Header,
        "a truncated header is rejected");
    dds_check(
        dds_parse(buffer, 2, &image) == Dds_Parse_Result_Bad_Magic,
        "a buffer too short for magic is rejected");

    /* A valid header whose pixel payload is short. */
    dds_build_header(
        buffer, 4, 4, 0x40, 0, 32,
        0x00ff0000u, 0x0000ff00u, 0x000000ffu);
    dds_check(
        dds_parse(buffer, 128 + 8, &image) == Dds_Parse_Result_Truncated,
        "truncated pixel data is rejected");

    /* Header size field must be exactly 124. */
    dds_build_header(
        buffer, 4, 4, 0x40, 0, 32,
        0x00ff0000u, 0x0000ff00u, 0x000000ffu);
    dds_write_u32(buffer + 4, 100);
    dds_check(
        dds_parse(buffer, sizeof(buffer), &image) ==
            Dds_Parse_Result_Bad_Header,
        "a wrong header size is rejected");

    /* Zero dimensions. */
    dds_build_header(
        buffer, 0, 4, 0x40, 0, 32,
        0x00ff0000u, 0x0000ff00u, 0x000000ffu);
    dds_check(
        dds_parse(buffer, sizeof(buffer), &image) ==
            Dds_Parse_Result_Bad_Header,
        "a zero width is rejected");

    /* Unsupported: 24-bit RGB. */
    dds_build_header(
        buffer, 4, 4, 0x40, 0, 24,
        0x00ff0000u, 0x0000ff00u, 0x000000ffu);
    dds_check(
        dds_parse(buffer, sizeof(buffer), &image) ==
            Dds_Parse_Result_Unsupported_Format,
        "24-bit RGB is unsupported");

    /* Unsupported fourCC (DX10 extension header). */
    dds_build_header(
        buffer, 4, 4, 0x4, dds_fourcc('D', 'X', '1', '0'), 0, 0, 0, 0);
    dds_check(
        dds_parse(buffer, sizeof(buffer), &image) ==
            Dds_Parse_Result_Unsupported_Format,
        "the DX10 extension header is unsupported");

    /* Unsupported channel mask order. */
    dds_build_header(
        buffer, 4, 4, 0x40, 0, 32,
        0x0000ff00u, 0x00ff0000u, 0x000000ffu);
    dds_check(
        dds_parse(buffer, sizeof(buffer), &image) ==
            Dds_Parse_Result_Unsupported_Format,
        "an unknown mask order is unsupported");
}

static void
test_result_strings(void)
{
    dds_check(
        strcmp(dds_parse_result_string(Dds_Parse_Result_Ok), "ok") == 0,
        "ok result string");
    dds_check(
        strcmp(
            dds_parse_result_string(Dds_Parse_Result_Truncated),
            "truncated") == 0,
        "truncated result string");
}

int
main(void)
{
    test_bgra8();
    test_rgba8();
    test_block_compressed();
    test_rejections();
    test_result_strings();

    if (dds_test_failures == 0)
    {
        printf("all DDS parser tests passed\n");
        return 0;
    }
    fprintf(stderr, "%u DDS parser checks failed\n", dds_test_failures);
    return 1;
}
