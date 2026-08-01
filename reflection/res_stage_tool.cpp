/*
 * res_stage: stages resource files (docs/resource_manifest.md section 5).
 *
 * Two modes, both content-hash gated and copy-only:
 *   --vault <dir>    sync vault-relative files into the res/ tree
 *   --stage <dir>    mirror the res/ tree beside an executable
 * --check reports drift without copying and exits nonzero when work is
 * pending, for CI.
 *
 * The tool never transforms content; converters are a separate decision.
 */

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    Res_Stage_Max_Path = 1024,
    Res_Stage_Chunk = 64 * 1024
};

typedef struct Res_Stage_Counters
{
    uint32_t copied;
    uint32_t skipped;
    uint32_t missing;
    uint32_t failed;
} Res_Stage_Counters;

static uint64_t
res_stage_hash_file(const char* path, uint32_t* out_ok)
{
    /* FNV-1a over the file contents; equal size and hash means equal. */
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    static uint8_t chunk[Res_Stage_Chunk];
    HANDLE handle;
    DWORD read_bytes = 0;

    *out_ok = 0;
    handle = CreateFileA(
        path,
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
    for (;;)
    {
        if (!ReadFile(handle, chunk, (DWORD)sizeof(chunk), &read_bytes, 0))
        {
            CloseHandle(handle);
            return 0;
        }
        if (read_bytes == 0)
        {
            break;
        }
        for (DWORD index = 0; index < read_bytes; ++index)
        {
            hash ^= (uint64_t)chunk[index];
            hash *= UINT64_C(0x100000001b3);
        }
    }
    CloseHandle(handle);
    *out_ok = 1;
    return hash;
}

static uint32_t
res_stage_files_match(const char* source, const char* destination)
{
    WIN32_FILE_ATTRIBUTE_DATA source_data;
    WIN32_FILE_ATTRIBUTE_DATA destination_data;
    uint32_t source_ok = 0;
    uint32_t destination_ok = 0;

    if (!GetFileAttributesExA(source, GetFileExInfoStandard, &source_data) ||
        !GetFileAttributesExA(
            destination,
            GetFileExInfoStandard,
            &destination_data))
    {
        return 0;
    }
    if (source_data.nFileSizeLow != destination_data.nFileSizeLow ||
        source_data.nFileSizeHigh != destination_data.nFileSizeHigh)
    {
        return 0;
    }
    const uint64_t source_hash = res_stage_hash_file(source, &source_ok);
    const uint64_t destination_hash =
        res_stage_hash_file(destination, &destination_ok);
    return source_ok && destination_ok && source_hash == destination_hash;
}

static uint32_t
res_stage_ensure_directory(const char* path)
{
    char partial[Res_Stage_Max_Path];
    const size_t length = strlen(path);
    if (length >= sizeof(partial))
    {
        return 0;
    }
    memcpy(partial, path, length + 1);
    for (size_t index = 1; index < length; ++index)
    {
        if (partial[index] == '/' || partial[index] == '\\')
        {
            const char saved = partial[index];
            partial[index] = 0;
            CreateDirectoryA(partial, 0);
            partial[index] = saved;
        }
    }
    CreateDirectoryA(partial, 0);
    return 1;
}

static uint32_t
res_stage_copy_file(
    const char* source,
    const char* destination,
    uint32_t check_only,
    Res_Stage_Counters* counters)
{
    if (GetFileAttributesA(source) == INVALID_FILE_ATTRIBUTES)
    {
        fprintf(stderr, "res_stage: missing source %s\n", source);
        counters->missing += 1;
        return 0;
    }
    if (res_stage_files_match(source, destination))
    {
        counters->skipped += 1;
        return 1;
    }
    if (check_only)
    {
        printf("res_stage: would copy %s\n", destination);
        counters->copied += 1;
        return 1;
    }

    char parent[Res_Stage_Max_Path];
    const size_t length = strlen(destination);
    if (length >= sizeof(parent))
    {
        counters->failed += 1;
        return 0;
    }
    memcpy(parent, destination, length + 1);
    for (size_t index = length; index > 0; --index)
    {
        if (parent[index - 1] == '/' || parent[index - 1] == '\\')
        {
            parent[index - 1] = 0;
            break;
        }
    }
    res_stage_ensure_directory(parent);

    if (!CopyFileA(source, destination, FALSE))
    {
        fprintf(
            stderr,
            "res_stage: could not copy %s to %s (error %lu)\n",
            source,
            destination,
            (unsigned long)GetLastError());
        counters->failed += 1;
        return 0;
    }
    printf("res_stage: copied %s\n", destination);
    counters->copied += 1;
    return 1;
}

static void
res_stage_walk(
    const char* source_directory,
    const char* destination_directory,
    const char* relative_prefix,
    uint32_t check_only,
    Res_Stage_Counters* counters)
{
    char pattern[Res_Stage_Max_Path];
    WIN32_FIND_DATAA find_data;
    HANDLE find_handle;

    if (snprintf(pattern, sizeof(pattern), "%s/*", source_directory) >=
        (int)sizeof(pattern))
    {
        counters->failed += 1;
        return;
    }
    find_handle = FindFirstFileA(pattern, &find_data);
    if (find_handle == INVALID_HANDLE_VALUE)
    {
        return;
    }

    do
    {
        if (strcmp(find_data.cFileName, ".") == 0 ||
            strcmp(find_data.cFileName, "..") == 0)
        {
            continue;
        }

        char source_path[Res_Stage_Max_Path];
        char destination_path[Res_Stage_Max_Path];
        char relative_path[Res_Stage_Max_Path];
        if (snprintf(
                source_path,
                sizeof(source_path),
                "%s/%s",
                source_directory,
                find_data.cFileName) >= (int)sizeof(source_path) ||
            snprintf(
                destination_path,
                sizeof(destination_path),
                "%s/%s",
                destination_directory,
                find_data.cFileName) >= (int)sizeof(destination_path) ||
            snprintf(
                relative_path,
                sizeof(relative_path),
                "%s%s%s",
                relative_prefix,
                relative_prefix[0] == 0 ? "" : "/",
                find_data.cFileName) >= (int)sizeof(relative_path))
        {
            counters->failed += 1;
            continue;
        }

        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            res_stage_walk(
                source_path,
                destination_path,
                relative_path,
                check_only,
                counters);
            continue;
        }
        res_stage_copy_file(
            source_path,
            destination_path,
            check_only,
            counters);
    }
    while (FindNextFileA(find_handle, &find_data) != 0);

    FindClose(find_handle);
}

int main(int argument_count, char** arguments)
{
    const char* res_directory = 0;
    const char* vault_directory = 0;
    const char* stage_directory = 0;
    uint32_t check_only = 0;
    Res_Stage_Counters counters;

    memset(&counters, 0, sizeof(counters));

    for (int index = 1; index < argument_count; ++index)
    {
        if (strcmp(arguments[index], "--res") == 0 &&
            index + 1 < argument_count)
        {
            res_directory = arguments[++index];
        }
        else if (strcmp(arguments[index], "--vault") == 0 &&
                 index + 1 < argument_count)
        {
            vault_directory = arguments[++index];
        }
        else if (strcmp(arguments[index], "--stage") == 0 &&
                 index + 1 < argument_count)
        {
            stage_directory = arguments[++index];
        }
        else if (strcmp(arguments[index], "--check") == 0)
        {
            check_only = 1;
        }
        else
        {
            fprintf(
                stderr,
                "usage: res_stage --res <dir> [--vault <dir>] "
                "[--stage <dir>] [--check]\n");
            return 2;
        }
    }

    if (res_directory == 0 ||
        (vault_directory == 0 && stage_directory == 0))
    {
        fprintf(
            stderr,
            "usage: res_stage --res <dir> [--vault <dir>] "
            "[--stage <dir>] [--check]\n");
        return 2;
    }

    if (vault_directory != 0)
    {
        res_stage_walk(
            vault_directory,
            res_directory,
            "",
            check_only,
            &counters);
    }
    if (stage_directory != 0)
    {
        char destination[Res_Stage_Max_Path];
        if (snprintf(
                destination,
                sizeof(destination),
                "%s/res",
                stage_directory) >= (int)sizeof(destination))
        {
            fprintf(stderr, "res_stage: staging path is too long\n");
            return 1;
        }
        res_stage_walk(
            res_directory,
            destination,
            "",
            check_only,
            &counters);
    }

    if (counters.missing != 0 || counters.failed != 0)
    {
        fprintf(
            stderr,
            "res_stage: %u missing, %u failed\n",
            counters.missing,
            counters.failed);
        return 1;
    }
    if (check_only && counters.copied != 0)
    {
        fprintf(
            stderr,
            "res_stage: %u file(s) are out of date\n",
            counters.copied);
        return 1;
    }
    printf(
        "res_stage: %u copied, %u up to date\n",
        counters.copied,
        counters.skipped);
    return 0;
}
