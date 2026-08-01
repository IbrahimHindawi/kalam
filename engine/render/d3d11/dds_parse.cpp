#include "dds_parse.h"

namespace
{

const uint32_t Dds_Magic = 0x20534444u; /* "DDS " */
const uint32_t Dds_Header_Size = 124;
const uint32_t Dds_Pixel_Format_Size = 32;
const uint32_t Dds_Flag_Caps = 0x1;
const uint32_t Dds_Flag_Height = 0x2;
const uint32_t Dds_Flag_Width = 0x4;
const uint32_t Dds_Flag_Pixel_Format = 0x1000;
const uint32_t Dds_Pixel_Flag_Fourcc = 0x4;
const uint32_t Dds_Pixel_Flag_Rgb = 0x40;

uint32_t read_u32(const uint8_t* bytes)
{
    return (uint32_t)bytes[0] |
        ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) |
        ((uint32_t)bytes[3] << 24);
}

uint32_t fourcc(char a, char b, char c, char d)
{
    return (uint32_t)(uint8_t)a |
        ((uint32_t)(uint8_t)b << 8) |
        ((uint32_t)(uint8_t)c << 16) |
        ((uint32_t)(uint8_t)d << 24);
}

} // namespace

extern "C" Dds_Parse_Result dds_parse(
    const void* bytes,
    uint64_t size,
    Dds_Image* out_image)
{
    if (bytes == 0 || out_image == 0)
    {
        return Dds_Parse_Result_Invalid_Argument;
    }
    const uint8_t* cursor = (const uint8_t*)bytes;

    if (size < 4 + Dds_Header_Size)
    {
        return size < 4 ? Dds_Parse_Result_Bad_Magic
                        : Dds_Parse_Result_Bad_Header;
    }
    if (read_u32(cursor) != Dds_Magic)
    {
        return Dds_Parse_Result_Bad_Magic;
    }

    const uint8_t* header = cursor + 4;
    const uint32_t header_size = read_u32(header + 0);
    const uint32_t flags = read_u32(header + 4);
    const uint32_t height = read_u32(header + 8);
    const uint32_t width = read_u32(header + 12);
    const uint8_t* pixel_format = header + 72;
    const uint32_t pixel_format_size = read_u32(pixel_format + 0);
    const uint32_t pixel_flags = read_u32(pixel_format + 4);
    const uint32_t pixel_fourcc = read_u32(pixel_format + 8);
    const uint32_t bit_count = read_u32(pixel_format + 12);
    const uint32_t red_mask = read_u32(pixel_format + 16);
    const uint32_t green_mask = read_u32(pixel_format + 20);
    const uint32_t blue_mask = read_u32(pixel_format + 24);

    const uint32_t required_flags =
        Dds_Flag_Caps | Dds_Flag_Height | Dds_Flag_Width |
        Dds_Flag_Pixel_Format;
    if (header_size != Dds_Header_Size ||
        pixel_format_size != Dds_Pixel_Format_Size ||
        (flags & required_flags) != required_flags ||
        width == 0 || height == 0 ||
        width > 16384 || height > 16384)
    {
        return Dds_Parse_Result_Bad_Header;
    }

    Dds_Image image;
    image.width = width;
    image.height = height;
    image.data = cursor + 4 + Dds_Header_Size;

    if (pixel_flags & Dds_Pixel_Flag_Fourcc)
    {
        uint32_t block_size;
        if (pixel_fourcc == fourcc('D', 'X', 'T', '1'))
        {
            image.format = Dds_Format_BC1;
            block_size = 8;
        }
        else if (pixel_fourcc == fourcc('D', 'X', 'T', '5'))
        {
            image.format = Dds_Format_BC3;
            block_size = 16;
        }
        else
        {
            /* DX10 extension headers and other codecs are deferred. */
            return Dds_Parse_Result_Unsupported_Format;
        }
        const uint32_t blocks_wide = (width + 3) / 4;
        const uint32_t blocks_high = (height + 3) / 4;
        image.row_pitch = blocks_wide * block_size;
        image.data_size = (uint64_t)image.row_pitch * blocks_high;
    }
    else if ((pixel_flags & Dds_Pixel_Flag_Rgb) && bit_count == 32)
    {
        if (red_mask == 0x00ff0000u && green_mask == 0x0000ff00u &&
            blue_mask == 0x000000ffu)
        {
            image.format = Dds_Format_B8G8R8A8;
        }
        else if (red_mask == 0x000000ffu && green_mask == 0x0000ff00u &&
                 blue_mask == 0x00ff0000u)
        {
            image.format = Dds_Format_R8G8B8A8;
        }
        else
        {
            return Dds_Parse_Result_Unsupported_Format;
        }
        image.row_pitch = width * 4;
        image.data_size = (uint64_t)image.row_pitch * height;
    }
    else
    {
        return Dds_Parse_Result_Unsupported_Format;
    }

    if (size < 4 + (uint64_t)Dds_Header_Size + image.data_size)
    {
        return Dds_Parse_Result_Truncated;
    }

    *out_image = image;
    return Dds_Parse_Result_Ok;
}

extern "C" const char* dds_parse_result_string(Dds_Parse_Result result)
{
    switch (result)
    {
    case Dds_Parse_Result_Ok: return "ok";
    case Dds_Parse_Result_Invalid_Argument: return "invalid argument";
    case Dds_Parse_Result_Bad_Magic: return "bad magic";
    case Dds_Parse_Result_Bad_Header: return "bad header";
    case Dds_Parse_Result_Unsupported_Format: return "unsupported format";
    case Dds_Parse_Result_Truncated: return "truncated";
    }
    return "unknown";
}
