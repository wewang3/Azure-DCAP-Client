// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include <algorithm>
#include <cstring>
#include <mutex>
#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <fstream>
#include <stdio.h>
#include <string>
#include <sstream>
#include <time.h>
#include <iomanip> 
#include <iostream>
#include <cstdio>
#include <filesystem>

#define NT_SUCCESS(Status)          (((NTSTATUS)(Status)) >= 0)

#define STATUS_UNSUCCESSFUL         ((NTSTATUS)0xC0000001L)

#define MAX_RETRY					10000
#define SLEEP_RETRY_MS				15

constexpr uint16_t CACHE_V1 = 1;

static std::wstring g_cache_dirname;

static void throw_if(bool should_throw, const std::string& error)
{
    if (should_throw)
    {
        throw std::runtime_error(error);
    }
}

static void throw_if(bool should_throw, const std::string& error, HANDLE file)
{
	if (should_throw)
	{
		CloseHandle(file);
		throw std::runtime_error(error);
	}
}

static void fthrow_if(bool should_throw, const std::string& error, HANDLE file)
{
	if (should_throw)
	{
		FindClose(file);
		throw std::runtime_error(error);
	}
}


struct CacheEntryHeaderV1 {
    uint16_t version;   // The version of the cache header
    time_t expiry;      // expiration time of this cache item
};

static void make_dir(const std::wstring& dirname)
{
	CreateDirectory(dirname.c_str(), NULL);
	throw_if(GetLastError() == ERROR_PATH_NOT_FOUND && GetLastError() != ERROR_ALREADY_EXISTS, "Path not found");
}

static void init_callback()
{
	const DWORD buffSize = 65535;
	
	LPTSTR env_home = new TCHAR[buffSize];
	GetEnvironmentVariable(L"LOCALAPPDATA", env_home, buffSize);
	std::wstring wenv_home(env_home);

	LPTSTR env_azdcap_cache = new TCHAR[buffSize];
	GetEnvironmentVariable(L"AZDCAP_CACHE", env_azdcap_cache, buffSize);

    const std::wstring application_name(L"\\.az-dcap-client");

    std::wstring dirname;

    if (env_home != 0 && env_home[0] != 0 )
    {
		dirname = env_home;
    } else if (env_azdcap_cache != 0 && env_azdcap_cache[0] != 0)
	{
		dirname = env_azdcap_cache;
	}
	else
    {
        // Throwing exception if the expected HOME
        // environment variable is not defined.
        throw std::runtime_error("LOCALAPPDATA and AZDCAPCACHE environment variables not defined");
    }

    dirname += application_name;
    make_dir(dirname);
    g_cache_dirname = dirname;
}


static void init()
{
    static std::once_flag init_flag;
    std::call_once(init_flag, init_callback);
}

static std::wstring sha256(size_t data_size, const void* data)
{
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    std::string errorString;
    DWORD cbData = 0;
    DWORD cbHash = 0;
    DWORD cbHashObject = 0;
    PBYTE pbHashObject = NULL;
    PBYTE pbHash = NULL;
    std::string retval;

    //open an algorithm handle
    if (!NT_SUCCESS(status = BCryptOpenAlgorithmProvider(
        &hAlg,
        BCRYPT_SHA256_ALGORITHM,
        NULL,
        0)))
    {
        errorString = "Error 0x" + std::to_string(status) + "returned by BCryptOpenAlgorithmProvider\n";
        goto Cleanup;
    }

    if (!NT_SUCCESS(status == BCryptGetProperty(
        hAlg,
        BCRYPT_OBJECT_LENGTH,
        (PBYTE)&cbHashObject,
        sizeof(DWORD),
        &cbData,
        0)))
    {
        errorString = "Error 0x" + std::to_string(status) + "returned by BCryptGetProperty\n";
        goto Cleanup;
    }

    pbHashObject = (PBYTE)HeapAlloc(GetProcessHeap(), 0, cbHashObject);
    if (pbHashObject == NULL)
    {
        errorString = "Memory allocation failed\n";
        status = STATUS_NO_MEMORY;
        goto Cleanup;
    }

    //calculate the length of the hash
    if (!NT_SUCCESS(status = BCryptGetProperty(
        hAlg,
        BCRYPT_HASH_LENGTH,
        (PBYTE)&cbHash,
        sizeof(DWORD),
        &cbData,
        0)))
    {
        errorString = "Error 0x" + std::to_string(status) + "returned by BCryptGetProperty\n";
        goto Cleanup;
    }

    //allocate the hash buffer on the heap
    pbHash = (PBYTE)HeapAlloc(GetProcessHeap(), 0, cbHash);
    if (NULL == pbHash)
    {
        errorString = "Memory allocation failed\n";
        status = STATUS_NO_MEMORY;
        goto Cleanup;
    }

    //create a hash
    if (!NT_SUCCESS(status = BCryptCreateHash(
        hAlg,
        &hHash,
        pbHashObject,
        cbHashObject,
        NULL,
        0,
        0)))
    {
        errorString = "Error 0x" + std::to_string(status) + "returned by BCryptCreateHash\n";
        goto Cleanup;
    }


    //hash some data
    if (!NT_SUCCESS(status = BCryptHashData(
        hHash,
        (PBYTE)data,
        (ULONG)data_size,
        0)))
    {
        errorString = "Error 0x" + std::to_string(status) + "returned by BCryptHashData\n";
        goto Cleanup;
    }

    //close the hash
    if (!NT_SUCCESS(status = BCryptFinishHash(
        hHash,
        pbHash,
        cbHash,
        0)))
    {
        errorString = "Error 0x" + std::to_string(status) + "returned by BCryptFinishHash\n";
        goto Cleanup;
    }

    retval.reserve(2 * cbHash + 1);
    for (size_t i = 0; i < cbHash; i++)
    {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", pbHash[i]);
        retval += buf;
    }

Cleanup:

    if (hAlg)
    {
        BCryptCloseAlgorithmProvider(hAlg, 0);
    }

    if (hHash)
    {
        BCryptDestroyHash(hHash);
    }

    if (pbHashObject)
    {
        HeapFree(GetProcessHeap(), 0, pbHashObject);
    }

    if (pbHash)
    {
        HeapFree(GetProcessHeap(), 0, pbHash);
    }

    throw_if(!NT_SUCCESS(status), errorString);

	std::wstring wretval(retval.begin(), retval.end());
    return wretval;
}

static std::wstring sha256(const std::string& input)
{
    return sha256(input.length(), input.data());
}

static std::wstring get_file_name(const std::string& id)
{
    return g_cache_dirname + L"\\" + sha256(id);
}

void local_cache_clear()
{
    init();

    WIN32_FIND_DATA data;
    std::wstring baseDir(g_cache_dirname.begin(), g_cache_dirname.end());
    std::wstring searchPattern = baseDir + L"\\*";

    HANDLE hFind = FindFirstFile(searchPattern.c_str(), &data);

    if (hFind != INVALID_HANDLE_VALUE)
    {
        do {
            std::wstring fileName(data.cFileName);
            if (fileName.compare(L".") && fileName.compare(L".."))
            {
                std::wstring fullFileName = baseDir + L"\\" + fileName;

                fthrow_if(!DeleteFileW(fullFileName.c_str()),
                    "Deleting file failed, error code " + GetLastError(), hFind);
            }
        } while (FindNextFile(hFind, &data));
        FindClose(hFind);
    }

    return;
};

HANDLE OpenHandle(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
    
    HANDLE file;
    int i = 0;
    do {
        file = CreateFile(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
            dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
            
        Sleep(SLEEP_RETRY_MS);
        i++;
    } while ((file == INVALID_HANDLE_VALUE) && (GetLastError() == ERROR_SHARING_VIOLATION) && (i < MAX_RETRY));

    return file;
}


void local_cache_add(const std::string& id, time_t expiry, size_t data_size, const void* data)
{
    throw_if(id.empty(), "The 'id' parameter must not be empty.");
    throw_if(data_size == 0, "Data cannot be empty.");
    throw_if(data == nullptr, "Data pointer must not be NULL.");

    init();
    CacheEntryHeaderV1 header{};
    header.version = CACHE_V1;
    header.expiry = expiry;

    std::wstring filename = get_file_name(id);
    //std::wstring wfilename(filename.begin(), filename.end());

    HANDLE file = OpenHandle(filename.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    throw_if(file == INVALID_HANDLE_VALUE, "Create file failed", file);

    DWORD headerwritten;
    DWORD datawritten;

    throw_if(!WriteFile(file, &header, sizeof(header), &headerwritten, nullptr),
        "Header write to local cache failed", file);

    throw_if(!WriteFile(file, data, (DWORD)data_size, &datawritten, nullptr),
        "Data write to local cache failed", file);

    CloseHandle(file);
}

std::unique_ptr<std::vector<uint8_t>> local_cache_get(
    const std::string& id)
{
    throw_if(id.empty(), "The 'id' parameter must not be empty.");
    init();

    std::wstring filename = get_file_name(id);
    //std::wstring wfilename(filename.begin(), filename.end());
    
    HANDLE file = OpenHandle(filename.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return nullptr;
    }

    CacheEntryHeaderV1 *header;
    char buf[sizeof(CacheEntryHeaderV1)] = { 0 };
    DWORD headerread = 0;

	ReadFile(file, &buf, sizeof(CacheEntryHeaderV1), &headerread, nullptr);
	throw_if(GetLastError(), "Header read from local cache failed", file);

    header = (CacheEntryHeaderV1*)buf;

    if (header->expiry <= time(nullptr))
    {
        CloseHandle(file);
        DeleteFileW(filename.c_str());
        // Even if unlink fails, we can just return null. Thus, the return
        // value is intentionally ignored here.
        return nullptr;
    }

    DWORD size = GetFileSize(file, nullptr);
    int datasize = size - sizeof(CacheEntryHeaderV1);
    auto cache_entry = std::make_unique<std::vector<uint8_t>>(datasize);

    DWORD dataread = 0;
	ReadFile(file, cache_entry->data(), size, &dataread, nullptr);
	throw_if(GetLastError(), "Data read from local cache failed", file);
	throw_if(dataread != datasize, "Something wrong with data", file);

    CloseHandle(file);

    return cache_entry;
}
