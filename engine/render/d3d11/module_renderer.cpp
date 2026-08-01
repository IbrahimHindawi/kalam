#include "module_renderer.h"

#include "dds_parse.h"
#include "res_manifest.generated.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>

#include <math.h>
#include <string.h>

typedef struct Module_Render_Vertex
{
    float position[3];
    float normal[3];
} Module_Render_Vertex;

typedef struct Module_Render_Constants
{
    float model_view_projection[16];
    float model[16];
    float color[4];
    float post_tint[4];
    float post_parameters[4];
} Module_Render_Constants;

typedef struct Module_Render_Matrix
{
    float value[16];
} Module_Render_Matrix;

static const Module_Render_Vertex module_renderer_cube_vertices[] =
{
    {{-1.0f, -1.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}},
    {{-1.0f,  1.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}},
    {{ 1.0f,  1.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}},
    {{ 1.0f, -1.0f, -1.0f}, { 0.0f,  0.0f, -1.0f}},

    {{ 1.0f, -1.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}},
    {{ 1.0f,  1.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}},
    {{-1.0f,  1.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}},
    {{-1.0f, -1.0f,  1.0f}, { 0.0f,  0.0f,  1.0f}},

    {{-1.0f, -1.0f,  1.0f}, {-1.0f,  0.0f,  0.0f}},
    {{-1.0f,  1.0f,  1.0f}, {-1.0f,  0.0f,  0.0f}},
    {{-1.0f,  1.0f, -1.0f}, {-1.0f,  0.0f,  0.0f}},
    {{-1.0f, -1.0f, -1.0f}, {-1.0f,  0.0f,  0.0f}},

    {{ 1.0f, -1.0f, -1.0f}, { 1.0f,  0.0f,  0.0f}},
    {{ 1.0f,  1.0f, -1.0f}, { 1.0f,  0.0f,  0.0f}},
    {{ 1.0f,  1.0f,  1.0f}, { 1.0f,  0.0f,  0.0f}},
    {{ 1.0f, -1.0f,  1.0f}, { 1.0f,  0.0f,  0.0f}},

    {{-1.0f,  1.0f, -1.0f}, { 0.0f,  1.0f,  0.0f}},
    {{-1.0f,  1.0f,  1.0f}, { 0.0f,  1.0f,  0.0f}},
    {{ 1.0f,  1.0f,  1.0f}, { 0.0f,  1.0f,  0.0f}},
    {{ 1.0f,  1.0f, -1.0f}, { 0.0f,  1.0f,  0.0f}},

    {{-1.0f, -1.0f,  1.0f}, { 0.0f, -1.0f,  0.0f}},
    {{-1.0f, -1.0f, -1.0f}, { 0.0f, -1.0f,  0.0f}},
    {{ 1.0f, -1.0f, -1.0f}, { 0.0f, -1.0f,  0.0f}},
    {{ 1.0f, -1.0f,  1.0f}, { 0.0f, -1.0f,  0.0f}},
};

static const uint16_t module_renderer_cube_indices[] =
{
     0,  1,  2,  0,  2,  3,
     4,  5,  6,  4,  6,  7,
     8,  9, 10,  8, 10, 11,
    12, 13, 14, 12, 14, 15,
    16, 17, 18, 16, 18, 19,
    20, 21, 22, 20, 22, 23,
};

static const float module_renderer_background_color[4] =
{
    0.015f,
    0.020f,
    0.030f,
    1.0f
};

static void
module_renderer_release(void** object)
{
    IUnknown* unknown = 0;

    if (object == 0 || *object == 0)
    {
        return;
    }

    unknown = (IUnknown*)*object;
    unknown->Release();
    *object = 0;
}

static Module_Renderer_Result
module_renderer_fail(
    Module_Renderer* renderer,
    Module_Renderer_Result result,
    HRESULT hresult)
{
    if (renderer != 0)
    {
        renderer->last_hresult = (int32_t)hresult;
    }
    return result;
}

static Module_Render_Matrix
module_renderer_matrix_identity(void)
{
    Module_Render_Matrix result;
    memset(&result, 0, sizeof(result));
    result.value[0] = 1.0f;
    result.value[5] = 1.0f;
    result.value[10] = 1.0f;
    result.value[15] = 1.0f;
    return result;
}

static Module_Render_Matrix
module_renderer_matrix_multiply(
    const Module_Render_Matrix* left,
    const Module_Render_Matrix* right)
{
    Module_Render_Matrix result;
    uint32_t row = 0;
    uint32_t column = 0;
    uint32_t inner = 0;

    memset(&result, 0, sizeof(result));
    for (row = 0; row < 4; row += 1)
    {
        for (column = 0; column < 4; column += 1)
        {
            for (inner = 0; inner < 4; inner += 1)
            {
                result.value[row * 4 + column] +=
                    left->value[row * 4 + inner] *
                    right->value[inner * 4 + column];
            }
        }
    }
    return result;
}

static Module_Render_Matrix
module_renderer_matrix_scale(float x, float y, float z)
{
    Module_Render_Matrix result = module_renderer_matrix_identity();
    result.value[0] = x;
    result.value[5] = y;
    result.value[10] = z;
    return result;
}

static Module_Render_Matrix
module_renderer_matrix_translation(float x, float y, float z)
{
    Module_Render_Matrix result = module_renderer_matrix_identity();
    result.value[12] = x;
    result.value[13] = y;
    result.value[14] = z;
    return result;
}

static Module_Render_Matrix
module_renderer_matrix_rotation_x(float radians)
{
    Module_Render_Matrix result = module_renderer_matrix_identity();
    float cosine = cosf(radians);
    float sine = sinf(radians);

    result.value[5] = cosine;
    result.value[6] = sine;
    result.value[9] = -sine;
    result.value[10] = cosine;
    return result;
}

static Module_Render_Matrix
module_renderer_matrix_rotation_y(float radians)
{
    Module_Render_Matrix result = module_renderer_matrix_identity();
    float cosine = cosf(radians);
    float sine = sinf(radians);

    result.value[0] = cosine;
    result.value[2] = -sine;
    result.value[8] = sine;
    result.value[10] = cosine;
    return result;
}

static Module_Render_Matrix
module_renderer_matrix_rotation_z(float radians)
{
    Module_Render_Matrix result = module_renderer_matrix_identity();
    float cosine = cosf(radians);
    float sine = sinf(radians);

    result.value[0] = cosine;
    result.value[1] = sine;
    result.value[4] = -sine;
    result.value[5] = cosine;
    return result;
}

static Module_Render_Matrix
module_renderer_matrix_perspective(
    float vertical_field_of_view_radians,
    float aspect_ratio,
    float near_plane,
    float far_plane)
{
    Module_Render_Matrix result;
    float y_scale =
        1.0f / tanf(vertical_field_of_view_radians * 0.5f);
    float x_scale = y_scale / aspect_ratio;

    memset(&result, 0, sizeof(result));
    result.value[0] = x_scale;
    result.value[5] = y_scale;
    result.value[10] = far_plane / (far_plane - near_plane);
    result.value[11] = 1.0f;
    result.value[14] =
        (-near_plane * far_plane) / (far_plane - near_plane);
    return result;
}

static uint32_t
module_renderer_cube_is_valid(const Module_Render_Cube* cube)
{
    uint32_t index = 0;

    if (cube == 0)
    {
        return 1;
    }

    for (index = 0; index < 3; index += 1)
    {
        if (!isfinite(cube->translation[index]) ||
            !isfinite(cube->rotation_degrees[index]) ||
            !isfinite(cube->scale[index]) ||
            cube->scale[index] <= 0.0f)
        {
            return 0;
        }
    }
    for (index = 0; index < 4; index += 1)
    {
        if (!isfinite(cube->color[index]))
        {
            return 0;
        }
    }
    return 1;
}

static uint32_t
module_renderer_post_process_is_valid(
    const Module_Render_Post_Process* post_process)
{
    uint32_t index = 0;

    if (post_process == 0)
    {
        return 1;
    }
    if (!isfinite(post_process->exposure) ||
        post_process->exposure < 0.0f ||
        post_process->exposure > 16.0f ||
        !isfinite(post_process->bloom_threshold) ||
        post_process->bloom_threshold < 0.0f ||
        post_process->bloom_threshold > 16.0f ||
        !isfinite(post_process->vignette) ||
        post_process->vignette < 0.0f ||
        post_process->vignette > 1.0f)
    {
        return 0;
    }
    for (index = 0; index < 4; index += 1)
    {
        if (!isfinite(post_process->tint[index]) ||
            post_process->tint[index] < 0.0f ||
            post_process->tint[index] > 1.0f)
        {
            return 0;
        }
    }
    return 1;
}

static void
module_renderer_build_constants(
    const Module_Render_Cube* cube,
    const Module_Render_Post_Process* post_process,
    uint32_t target_width,
    uint32_t target_height,
    Module_Render_Constants* out_constants)
{
    static const float degrees_to_radians =
        0.01745329251994329576923690768489f;
    Module_Render_Matrix scale;
    Module_Render_Matrix rotation_x;
    Module_Render_Matrix rotation_y;
    Module_Render_Matrix rotation_z;
    Module_Render_Matrix translation;
    Module_Render_Matrix view;
    Module_Render_Matrix projection;
    Module_Render_Matrix temporary;
    Module_Render_Matrix world;
    Module_Render_Matrix world_view;
    Module_Render_Matrix world_view_projection;
    float aspect_ratio =
        (float)target_width / (float)target_height;

    scale = module_renderer_matrix_scale(
        cube->scale[0],
        cube->scale[1],
        cube->scale[2]);
    rotation_x = module_renderer_matrix_rotation_x(
        cube->rotation_degrees[0] * degrees_to_radians);
    rotation_y = module_renderer_matrix_rotation_y(
        cube->rotation_degrees[1] * degrees_to_radians);
    rotation_z = module_renderer_matrix_rotation_z(
        cube->rotation_degrees[2] * degrees_to_radians);
    translation = module_renderer_matrix_translation(
        cube->translation[0],
        cube->translation[1],
        cube->translation[2]);
    view = module_renderer_matrix_translation(0.0f, 0.0f, 5.0f);
    projection = module_renderer_matrix_perspective(
        1.0471975511965977461542144610932f,
        aspect_ratio,
        0.1f,
        1000.0f);

    temporary = module_renderer_matrix_multiply(&scale, &rotation_x);
    world = module_renderer_matrix_multiply(&temporary, &rotation_y);
    temporary = module_renderer_matrix_multiply(&world, &rotation_z);
    world = module_renderer_matrix_multiply(&temporary, &translation);
    world_view = module_renderer_matrix_multiply(&world, &view);
    world_view_projection =
        module_renderer_matrix_multiply(&world_view, &projection);

    memcpy(
        out_constants->model_view_projection,
        world_view_projection.value,
        sizeof(out_constants->model_view_projection));
    memcpy(
        out_constants->model,
        world.value,
        sizeof(out_constants->model));
    memcpy(
        out_constants->color,
        cube->color,
        sizeof(out_constants->color));
    if (post_process != 0)
    {
        memcpy(
            out_constants->post_tint,
            post_process->tint,
            sizeof(out_constants->post_tint));
        out_constants->post_parameters[0] = post_process->exposure;
        out_constants->post_parameters[1] =
            post_process->bloom_threshold;
        out_constants->post_parameters[2] = post_process->vignette;
    }
    else
    {
        out_constants->post_tint[0] = 1.0f;
        out_constants->post_tint[1] = 1.0f;
        out_constants->post_tint[2] = 1.0f;
        out_constants->post_tint[3] = 1.0f;
        out_constants->post_parameters[0] = 1.0f;
        out_constants->post_parameters[1] = 16.0f;
        out_constants->post_parameters[2] = 0.0f;
    }
    out_constants->post_parameters[3] = 0.0f;
}

/*
 * Manifest resource loading (docs/resource_manifest.md section 6). Files
 * are read whole into a caller-owned buffer; the module owns every byte
 * it allocates and frees it before returning.
 */
typedef struct Module_Resource_File
{
    uint8_t* bytes;
    uint64_t size;
} Module_Resource_File;

static void
module_resource_file_free(Module_Resource_File* file)
{
    if (file->bytes != 0)
    {
        HeapFree(GetProcessHeap(), 0, file->bytes);
        file->bytes = 0;
    }
    file->size = 0;
}

static uint32_t
module_resource_file_read(
    const char* project_root,
    uint64_t project_root_size,
    const char* relative_path,
    Module_Resource_File* out_file)
{
    char full_path[1024];
    uint64_t root_length = project_root_size;
    size_t relative_length = strlen(relative_path);
    HANDLE handle = INVALID_HANDLE_VALUE;
    LARGE_INTEGER file_size;
    DWORD read_bytes = 0;

    out_file->bytes = 0;
    out_file->size = 0;

    while (root_length > 0 &&
           (project_root[root_length - 1] == '/' ||
            project_root[root_length - 1] == '\\'))
    {
        --root_length;
    }
    if (root_length == 0 ||
        root_length + 1 + relative_length + 1 > sizeof(full_path))
    {
        return 0;
    }
    memcpy(full_path, project_root, (size_t)root_length);
    full_path[root_length] = '/';
    memcpy(full_path + root_length + 1, relative_path, relative_length);
    full_path[root_length + 1 + relative_length] = 0;

    handle = CreateFileA(
        full_path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        0,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        0);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return 0;
    }
    if (!GetFileSizeEx(handle, &file_size) ||
        file_size.QuadPart <= 0 ||
        file_size.QuadPart > (LONGLONG)(64 * 1024 * 1024))
    {
        CloseHandle(handle);
        return 0;
    }
    out_file->bytes = (uint8_t*)HeapAlloc(
        GetProcessHeap(),
        0,
        (SIZE_T)file_size.QuadPart);
    if (out_file->bytes == 0)
    {
        CloseHandle(handle);
        return 0;
    }
    if (!ReadFile(
            handle,
            out_file->bytes,
            (DWORD)file_size.QuadPart,
            &read_bytes,
            0) ||
        read_bytes != (DWORD)file_size.QuadPart)
    {
        CloseHandle(handle);
        module_resource_file_free(out_file);
        return 0;
    }
    CloseHandle(handle);
    out_file->size = (uint64_t)file_size.QuadPart;
    return 1;
}

Module_Renderer_Result
module_renderer_create(
    Module_Renderer* out_renderer,
    void* borrowed_d3d11_device,
    void* borrowed_d3d11_immediate_context,
    const char* project_root,
    uint64_t project_root_size)
{
    ID3D11Device* device = (ID3D11Device*)borrowed_d3d11_device;
    ID3DBlob* vertex_shader_bytecode = 0;
    ID3DBlob* pixel_shader_bytecode = 0;
    ID3DBlob* compiler_messages = 0;
    D3D11_INPUT_ELEMENT_DESC input_elements[2];
    D3D11_BUFFER_DESC buffer_desc;
    D3D11_SUBRESOURCE_DATA initial_data;
    D3D11_RASTERIZER_DESC rasterizer_desc;
    D3D11_DEPTH_STENCIL_DESC depth_desc;
    D3D11_BLEND_DESC blend_desc;
    D3D11_TEXTURE2D_DESC texture_desc;
    D3D11_SUBRESOURCE_DATA texture_data;
    D3D11_SHADER_RESOURCE_VIEW_DESC texture_view_desc;
    D3D11_SAMPLER_DESC sampler_desc;
    Module_Resource_File shader_file = {0, 0};
    Module_Resource_File texture_file = {0, 0};
    Dds_Image texture_image;
    Dds_Parse_Result dds_result = Dds_Parse_Result_Ok;
    const Res_Shader_Entry* shader_entry = &res_shader_table[
        res_shader_module_basic];
    const Res_Tex_Entry* texture_entry = &res_tex_table[res_tex_checker];
    Module_Renderer_Result failure_result =
        Module_Renderer_Result_Resource_Create_Failed;
    UINT compiler_flags = D3DCOMPILE_ENABLE_STRICTNESS;
    HRESULT hresult = E_FAIL;

    if (out_renderer == 0 ||
        borrowed_d3d11_device == 0 ||
        borrowed_d3d11_immediate_context == 0 ||
        project_root == 0 ||
        project_root_size == 0)
    {
        return Module_Renderer_Result_Invalid_Argument;
    }

    memset(out_renderer, 0, sizeof(*out_renderer));
    memset(&texture_image, 0, sizeof(texture_image));
    out_renderer->device = borrowed_d3d11_device;
    out_renderer->immediate_context = borrowed_d3d11_immediate_context;
    out_renderer->last_hresult = (int32_t)S_OK;

#if defined(_DEBUG)
    compiler_flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    compiler_flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    if (!module_resource_file_read(
            project_root,
            project_root_size,
            shader_entry->path,
            &shader_file))
    {
        out_renderer->failed_resource_path = shader_entry->path;
        failure_result = Module_Renderer_Result_Resource_Missing;
        goto fail;
    }

    hresult = D3DCompile(
        shader_file.bytes,
        (SIZE_T)shader_file.size,
        shader_entry->path,
        0,
        0,
        "vs_main",
        "vs_4_0",
        compiler_flags,
        0,
        &vertex_shader_bytecode,
        &compiler_messages);
    module_renderer_release((void**)&compiler_messages);
    if (FAILED(hresult))
    {
        out_renderer->failed_resource_path = shader_entry->path;
        failure_result = module_renderer_fail(
            out_renderer,
            Module_Renderer_Result_Shader_Compile_Failed,
            hresult);
        goto fail;
    }

    hresult = D3DCompile(
        shader_file.bytes,
        (SIZE_T)shader_file.size,
        shader_entry->path,
        0,
        0,
        "ps_main",
        "ps_4_0",
        compiler_flags,
        0,
        &pixel_shader_bytecode,
        &compiler_messages);
    module_renderer_release((void**)&compiler_messages);
    if (FAILED(hresult))
    {
        out_renderer->failed_resource_path = shader_entry->path;
        failure_result = module_renderer_fail(
            out_renderer,
            Module_Renderer_Result_Shader_Compile_Failed,
            hresult);
        goto fail;
    }
    module_resource_file_free(&shader_file);

    hresult = device->CreateVertexShader(
        vertex_shader_bytecode->GetBufferPointer(),
        vertex_shader_bytecode->GetBufferSize(),
        0,
        (ID3D11VertexShader**)&out_renderer->vertex_shader);
    if (FAILED(hresult))
    {
        failure_result = module_renderer_fail(
            out_renderer,
            Module_Renderer_Result_Resource_Create_Failed,
            hresult);
        goto fail;
    }

    hresult = device->CreatePixelShader(
        pixel_shader_bytecode->GetBufferPointer(),
        pixel_shader_bytecode->GetBufferSize(),
        0,
        (ID3D11PixelShader**)&out_renderer->pixel_shader);
    if (FAILED(hresult))
    {
        failure_result = module_renderer_fail(
            out_renderer,
            Module_Renderer_Result_Resource_Create_Failed,
            hresult);
        goto fail;
    }

    memset(input_elements, 0, sizeof(input_elements));
    input_elements[0].SemanticName = "POSITION";
    input_elements[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    input_elements[0].InputSlotClass =
        D3D11_INPUT_PER_VERTEX_DATA;
    input_elements[1].SemanticName = "NORMAL";
    input_elements[1].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    input_elements[1].AlignedByteOffset =
        D3D11_APPEND_ALIGNED_ELEMENT;
    input_elements[1].InputSlotClass =
        D3D11_INPUT_PER_VERTEX_DATA;

    hresult = device->CreateInputLayout(
        input_elements,
        2,
        vertex_shader_bytecode->GetBufferPointer(),
        vertex_shader_bytecode->GetBufferSize(),
        (ID3D11InputLayout**)&out_renderer->input_layout);
    if (FAILED(hresult))
    {
        failure_result = module_renderer_fail(
            out_renderer,
            Module_Renderer_Result_Resource_Create_Failed,
            hresult);
        goto fail;
    }

    memset(&buffer_desc, 0, sizeof(buffer_desc));
    memset(&initial_data, 0, sizeof(initial_data));
    buffer_desc.ByteWidth =
        (UINT)sizeof(module_renderer_cube_vertices);
    buffer_desc.Usage = D3D11_USAGE_IMMUTABLE;
    buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    initial_data.pSysMem = module_renderer_cube_vertices;
    hresult = device->CreateBuffer(
        &buffer_desc,
        &initial_data,
        (ID3D11Buffer**)&out_renderer->vertex_buffer);
    if (FAILED(hresult))
    {
        failure_result = module_renderer_fail(
            out_renderer,
            Module_Renderer_Result_Resource_Create_Failed,
            hresult);
        goto fail;
    }

    memset(&buffer_desc, 0, sizeof(buffer_desc));
    memset(&initial_data, 0, sizeof(initial_data));
    buffer_desc.ByteWidth =
        (UINT)sizeof(module_renderer_cube_indices);
    buffer_desc.Usage = D3D11_USAGE_IMMUTABLE;
    buffer_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    initial_data.pSysMem = module_renderer_cube_indices;
    hresult = device->CreateBuffer(
        &buffer_desc,
        &initial_data,
        (ID3D11Buffer**)&out_renderer->index_buffer);
    if (FAILED(hresult))
    {
        failure_result = module_renderer_fail(
            out_renderer,
            Module_Renderer_Result_Resource_Create_Failed,
            hresult);
        goto fail;
    }

    memset(&buffer_desc, 0, sizeof(buffer_desc));
    buffer_desc.ByteWidth = (UINT)sizeof(Module_Render_Constants);
    buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hresult = device->CreateBuffer(
        &buffer_desc,
        0,
        (ID3D11Buffer**)&out_renderer->constant_buffer);
    if (FAILED(hresult))
    {
        failure_result = module_renderer_fail(
            out_renderer,
            Module_Renderer_Result_Resource_Create_Failed,
            hresult);
        goto fail;
    }

    memset(&rasterizer_desc, 0, sizeof(rasterizer_desc));
    rasterizer_desc.FillMode = D3D11_FILL_SOLID;
    rasterizer_desc.CullMode = D3D11_CULL_NONE;
    rasterizer_desc.DepthClipEnable = TRUE;
    hresult = device->CreateRasterizerState(
        &rasterizer_desc,
        (ID3D11RasterizerState**)&out_renderer->rasterizer_state);
    if (FAILED(hresult))
    {
        failure_result = module_renderer_fail(
            out_renderer,
            Module_Renderer_Result_Resource_Create_Failed,
            hresult);
        goto fail;
    }

    memset(&depth_desc, 0, sizeof(depth_desc));
    depth_desc.DepthEnable = TRUE;
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth_desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    hresult = device->CreateDepthStencilState(
        &depth_desc,
        (ID3D11DepthStencilState**)&out_renderer->depth_enabled_state);
    if (FAILED(hresult))
    {
        failure_result = module_renderer_fail(
            out_renderer,
            Module_Renderer_Result_Resource_Create_Failed,
            hresult);
        goto fail;
    }

    memset(&depth_desc, 0, sizeof(depth_desc));
    depth_desc.DepthEnable = FALSE;
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    hresult = device->CreateDepthStencilState(
        &depth_desc,
        (ID3D11DepthStencilState**)&out_renderer->depth_disabled_state);
    if (FAILED(hresult))
    {
        failure_result = module_renderer_fail(
            out_renderer,
            Module_Renderer_Result_Resource_Create_Failed,
            hresult);
        goto fail;
    }

    memset(&blend_desc, 0, sizeof(blend_desc));
    blend_desc.RenderTarget[0].BlendEnable = FALSE;
    blend_desc.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_ALL;
    hresult = device->CreateBlendState(
        &blend_desc,
        (ID3D11BlendState**)&out_renderer->blend_state);
    if (FAILED(hresult))
    {
        failure_result = module_renderer_fail(
            out_renderer,
            Module_Renderer_Result_Resource_Create_Failed,
            hresult);
        goto fail;
    }

    /* The manifest surface texture. */
    if (!module_resource_file_read(
            project_root,
            project_root_size,
            texture_entry->path,
            &texture_file))
    {
        out_renderer->failed_resource_path = texture_entry->path;
        failure_result = Module_Renderer_Result_Resource_Missing;
        goto fail;
    }
    dds_result = dds_parse(
        texture_file.bytes,
        texture_file.size,
        &texture_image);
    if (dds_result != Dds_Parse_Result_Ok)
    {
        out_renderer->failed_resource_path = texture_entry->path;
        failure_result = Module_Renderer_Result_Resource_Invalid;
        goto fail;
    }

    memset(&texture_desc, 0, sizeof(texture_desc));
    texture_desc.Width = texture_image.width;
    texture_desc.Height = texture_image.height;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    switch (texture_image.format)
    {
    case Dds_Format_B8G8R8A8:
        texture_desc.Format = texture_entry->srgb
            ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
            : DXGI_FORMAT_B8G8R8A8_UNORM;
        break;
    case Dds_Format_R8G8B8A8:
        texture_desc.Format = texture_entry->srgb
            ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
            : DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    case Dds_Format_BC1:
        texture_desc.Format = texture_entry->srgb
            ? DXGI_FORMAT_BC1_UNORM_SRGB
            : DXGI_FORMAT_BC1_UNORM;
        break;
    default:
        texture_desc.Format = texture_entry->srgb
            ? DXGI_FORMAT_BC3_UNORM_SRGB
            : DXGI_FORMAT_BC3_UNORM;
        break;
    }
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_IMMUTABLE;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    memset(&texture_data, 0, sizeof(texture_data));
    texture_data.pSysMem = texture_image.data;
    texture_data.SysMemPitch = texture_image.row_pitch;

    hresult = device->CreateTexture2D(
        &texture_desc,
        &texture_data,
        (ID3D11Texture2D**)&out_renderer->surface_texture);
    if (FAILED(hresult))
    {
        out_renderer->failed_resource_path = texture_entry->path;
        failure_result = module_renderer_fail(
            out_renderer,
            Module_Renderer_Result_Resource_Create_Failed,
            hresult);
        goto fail;
    }
    module_resource_file_free(&texture_file);

    memset(&texture_view_desc, 0, sizeof(texture_view_desc));
    texture_view_desc.Format = texture_desc.Format;
    texture_view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    texture_view_desc.Texture2D.MipLevels = 1;
    hresult = device->CreateShaderResourceView(
        (ID3D11Resource*)out_renderer->surface_texture,
        &texture_view_desc,
        (ID3D11ShaderResourceView**)&out_renderer->surface_texture_view);
    if (FAILED(hresult))
    {
        failure_result = module_renderer_fail(
            out_renderer,
            Module_Renderer_Result_Resource_Create_Failed,
            hresult);
        goto fail;
    }

    memset(&sampler_desc, 0, sizeof(sampler_desc));
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    hresult = device->CreateSamplerState(
        &sampler_desc,
        (ID3D11SamplerState**)&out_renderer->surface_sampler);
    if (FAILED(hresult))
    {
        failure_result = module_renderer_fail(
            out_renderer,
            Module_Renderer_Result_Resource_Create_Failed,
            hresult);
        goto fail;
    }

    module_renderer_release((void**)&pixel_shader_bytecode);
    module_renderer_release((void**)&vertex_shader_bytecode);
    out_renderer->initialized = 1;
    out_renderer->last_hresult = (int32_t)S_OK;
    return Module_Renderer_Result_Ok;

fail:
    module_resource_file_free(&shader_file);
    module_resource_file_free(&texture_file);
    module_renderer_release((void**)&compiler_messages);
    module_renderer_release((void**)&pixel_shader_bytecode);
    module_renderer_release((void**)&vertex_shader_bytecode);
    hresult = (HRESULT)out_renderer->last_hresult;
    {
        const char* failed_path = out_renderer->failed_resource_path;
        module_renderer_destroy(out_renderer);
        out_renderer->last_hresult = (int32_t)hresult;
        out_renderer->failed_resource_path = failed_path;
    }
    return failure_result;
}

void
module_renderer_destroy(Module_Renderer* renderer)
{
    ID3D11DeviceContext* context = 0;
    ID3D11Buffer* null_buffer = 0;
    UINT zero = 0;
    float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    if (renderer == 0)
    {
        return;
    }

    context = (ID3D11DeviceContext*)renderer->immediate_context;
    if (context != 0)
    {
        context->OMSetRenderTargets(0, 0, 0);
        context->OMSetBlendState(0, blend_factor, 0xffffffffu);
        context->OMSetDepthStencilState(0, 0);
        context->RSSetState(0);
        context->IASetInputLayout(0);
        context->IASetVertexBuffers(0, 1, &null_buffer, &zero, &zero);
        context->IASetIndexBuffer(0, DXGI_FORMAT_UNKNOWN, 0);
        context->VSSetConstantBuffers(0, 1, &null_buffer);
        context->PSSetConstantBuffers(0, 1, &null_buffer);
        context->VSSetShader(0, 0, 0);
        context->PSSetShader(0, 0, 0);
        context->GSSetShader(0, 0, 0);
        context->HSSetShader(0, 0, 0);
        context->DSSetShader(0, 0, 0);
        context->Flush();
    }

    module_renderer_release(&renderer->surface_sampler);
    module_renderer_release(&renderer->surface_texture_view);
    module_renderer_release(&renderer->surface_texture);
    module_renderer_release(&renderer->blend_state);
    module_renderer_release(&renderer->depth_disabled_state);
    module_renderer_release(&renderer->depth_enabled_state);
    module_renderer_release(&renderer->rasterizer_state);
    module_renderer_release(&renderer->constant_buffer);
    module_renderer_release(&renderer->index_buffer);
    module_renderer_release(&renderer->vertex_buffer);
    module_renderer_release(&renderer->input_layout);
    module_renderer_release(&renderer->pixel_shader);
    module_renderer_release(&renderer->vertex_shader);

    memset(renderer, 0, sizeof(*renderer));
}

Module_Renderer_Result
module_renderer_render_cube(
    Module_Renderer* renderer,
    const Module_Render_Target* target,
    const Module_Render_Cube* cube,
    const Module_Render_Post_Process* post_process)
{
    ID3D11Device* device = 0;
    ID3D11DeviceContext* context = 0;
    ID3D11RenderTargetView* render_target_view = 0;
    ID3D11DepthStencilView* depth_stencil_view = 0;
    ID3D11Buffer* vertex_buffer = 0;
    ID3D11Buffer* constant_buffer = 0;
    D3D11_VIEWPORT viewport;
    Module_Render_Constants constants;
    UINT vertex_stride = (UINT)sizeof(Module_Render_Vertex);
    UINT vertex_offset = 0;
    float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    HRESULT hresult = S_OK;

    if (renderer == 0 ||
        renderer->initialized == 0 ||
        target == 0 ||
        target->d3d11_render_target_view == 0 ||
        target->width == 0 ||
        target->height == 0 ||
        !module_renderer_cube_is_valid(cube) ||
        !module_renderer_post_process_is_valid(post_process))
    {
        return module_renderer_fail(
            renderer,
            Module_Renderer_Result_Invalid_Argument,
            E_INVALIDARG);
    }

    device = (ID3D11Device*)renderer->device;
    context = (ID3D11DeviceContext*)renderer->immediate_context;
    render_target_view =
        (ID3D11RenderTargetView*)target->d3d11_render_target_view;
    depth_stencil_view =
        (ID3D11DepthStencilView*)target->d3d11_depth_stencil_view;
    if (device == 0 || context == 0)
    {
        return module_renderer_fail(
            renderer,
            Module_Renderer_Result_Invalid_Argument,
            E_INVALIDARG);
    }

    hresult = device->GetDeviceRemovedReason();
    if (FAILED(hresult))
    {
        return module_renderer_fail(
            renderer,
            Module_Renderer_Result_Device_Removed,
            hresult);
    }

    context->ClearRenderTargetView(
        render_target_view,
        module_renderer_background_color);
    if (depth_stencil_view != 0)
    {
        context->ClearDepthStencilView(
            depth_stencil_view,
            D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
            1.0f,
            0);
    }

    if (cube == 0)
    {
        renderer->last_hresult = (int32_t)S_OK;
        return Module_Renderer_Result_Ok;
    }

    module_renderer_build_constants(
        cube,
        post_process,
        target->width,
        target->height,
        &constants);
    constant_buffer = (ID3D11Buffer*)renderer->constant_buffer;
    context->UpdateSubresource(
        constant_buffer,
        0,
        0,
        &constants,
        0,
        0);

    memset(&viewport, 0, sizeof(viewport));
    viewport.Width = (float)target->width;
    viewport.Height = (float)target->height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    context->OMSetRenderTargets(
        1,
        &render_target_view,
        depth_stencil_view);
    context->OMSetBlendState(
        (ID3D11BlendState*)renderer->blend_state,
        blend_factor,
        0xffffffffu);
    context->OMSetDepthStencilState(
        depth_stencil_view != 0
            ? (ID3D11DepthStencilState*)renderer->depth_enabled_state
            : (ID3D11DepthStencilState*)renderer->depth_disabled_state,
        0);
    context->RSSetState(
        (ID3D11RasterizerState*)renderer->rasterizer_state);
    context->RSSetViewports(1, &viewport);

    context->IASetInputLayout(
        (ID3D11InputLayout*)renderer->input_layout);
    vertex_buffer = (ID3D11Buffer*)renderer->vertex_buffer;
    context->IASetVertexBuffers(
        0,
        1,
        &vertex_buffer,
        &vertex_stride,
        &vertex_offset);
    context->IASetIndexBuffer(
        (ID3D11Buffer*)renderer->index_buffer,
        DXGI_FORMAT_R16_UINT,
        0);
    context->IASetPrimitiveTopology(
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    context->VSSetShader(
        (ID3D11VertexShader*)renderer->vertex_shader,
        0,
        0);
    context->VSSetConstantBuffers(0, 1, &constant_buffer);
    context->HSSetShader(0, 0, 0);
    context->DSSetShader(0, 0, 0);
    context->GSSetShader(0, 0, 0);
    context->PSSetShader(
        (ID3D11PixelShader*)renderer->pixel_shader,
        0,
        0);
    context->PSSetConstantBuffers(0, 1, &constant_buffer);
    {
        ID3D11ShaderResourceView* surface_view =
            (ID3D11ShaderResourceView*)renderer->surface_texture_view;
        ID3D11SamplerState* surface_sampler =
            (ID3D11SamplerState*)renderer->surface_sampler;
        context->PSSetShaderResources(0, 1, &surface_view);
        context->PSSetSamplers(0, 1, &surface_sampler);
    }

    context->DrawIndexed(
        (UINT)(sizeof(module_renderer_cube_indices) /
               sizeof(module_renderer_cube_indices[0])),
        0,
        0);

    if (cube->draw_transform_gizmo != 0)
    {
        Module_Render_Cube axis;
        uint32_t axis_index = 0;

        context->OMSetDepthStencilState(
            (ID3D11DepthStencilState*)renderer->depth_disabled_state,
            0);
        for (axis_index = 0; axis_index < 3; axis_index += 1)
        {
            memset(&axis, 0, sizeof(axis));
            memcpy(
                axis.translation,
                cube->translation,
                sizeof(axis.translation));
            axis.scale[0] = 0.045f;
            axis.scale[1] = 0.045f;
            axis.scale[2] = 0.045f;
            axis.scale[axis_index] = 1.1f;
            axis.translation[axis_index] += 1.1f;
            axis.color[axis_index] = 1.0f;
            axis.color[3] = 1.0f;

            module_renderer_build_constants(
                &axis,
                0,
                target->width,
                target->height,
                &constants);
            context->UpdateSubresource(
                constant_buffer,
                0,
                0,
                &constants,
                0,
                0);
            context->DrawIndexed(
                (UINT)(sizeof(module_renderer_cube_indices) /
                       sizeof(module_renderer_cube_indices[0])),
                0,
                0);
        }
    }

    hresult = device->GetDeviceRemovedReason();
    if (FAILED(hresult))
    {
        return module_renderer_fail(
            renderer,
            Module_Renderer_Result_Device_Removed,
            hresult);
    }

    renderer->last_hresult = (int32_t)S_OK;
    return Module_Renderer_Result_Ok;
}

const char*
module_renderer_result_string(Module_Renderer_Result result)
{
    switch (result)
    {
        case Module_Renderer_Result_Ok:
            return "ok";
        case Module_Renderer_Result_Invalid_Argument:
            return "invalid renderer argument";
        case Module_Renderer_Result_Shader_Compile_Failed:
            return "embedded D3D11 shader compilation failed";
        case Module_Renderer_Result_Resource_Create_Failed:
            return "game renderer D3D11 resource creation failed";
        case Module_Renderer_Result_Device_Removed:
            return "D3D11 device was removed";
        default:
            return "unknown game renderer result";
    }
}
