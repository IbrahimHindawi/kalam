#ifndef KALAM_ENGINE_RENDER_D3D11_DDS_PARSE_H
#define KALAM_ENGINE_RENDER_D3D11_DDS_PARSE_H

#include <stdint.h>

/*
 * First-party DDS container parsing (docs/resource_manifest.md section 6).
 * Pure memory-in, description-out; no GPU, no allocation, no platform
 * headers, so every rule is unit-testable with byte fixtures.
 *
 * Revision 1 formats: 32-bit BGRA8 / RGBA8 masks and the BC1/BC3 fourCC
 * blocks (which pass through to the GPU compressed, no decode).
 */

typedef uint32_t Dds_Parse_Result;

enum
{
    Dds_Parse_Result_Ok = 0,
    Dds_Parse_Result_Invalid_Argument,
    Dds_Parse_Result_Bad_Magic,
    Dds_Parse_Result_Bad_Header,
    Dds_Parse_Result_Unsupported_Format,
    Dds_Parse_Result_Truncated
};

typedef uint32_t Dds_Format;

enum
{
    Dds_Format_B8G8R8A8 = 0,
    Dds_Format_R8G8B8A8,
    Dds_Format_BC1,
    Dds_Format_BC3
};

typedef struct Dds_Image
{
    uint32_t width;
    uint32_t height;
    Dds_Format format;
    uint32_t row_pitch;
    const uint8_t* data;
    uint64_t data_size;
} Dds_Image;

#if defined(__cplusplus)
extern "C" {
#endif

Dds_Parse_Result dds_parse(
    const void* bytes,
    uint64_t size,
    Dds_Image* out_image);

const char* dds_parse_result_string(Dds_Parse_Result result);

#if defined(__cplusplus)
}
#endif

#endif
