#include "Utils.h"

#include <algorithm>
#include <Shlwapi.h>
#include <vector>

#include <Psapi.h>
#include "HandleWrapper.h"
#include <array>
#include <strsafe.h>
#include <WbemIdl.h>
#include <comdef.h>

namespace utils
{
    std::wstring getUserSid()
    {

        HANDLE hToken = NULL;
        DWORD dwBufferSize = 0;
        PTOKEN_USER pTokenUser = NULL;
        LPWSTR pszSid = NULL;
        std::wstring sid;

        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) 
        {
            goto ErrorReturn;
        }

        (void)GetTokenInformation(hToken, TokenUser, NULL, 0, &dwBufferSize);

        if (dwBufferSize == 0)
        {
            goto ErrorReturn;
        }

        pTokenUser = (PTOKEN_USER)LocalAlloc(LPTR, dwBufferSize);
        if (!pTokenUser)
        {
            goto ErrorReturn;
        }

        if (!GetTokenInformation(hToken, TokenUser, pTokenUser, dwBufferSize,
            &dwBufferSize)) 
        {
            goto ErrorReturn;
        }

        if (!IsValidSid(pTokenUser->User.Sid)) 
        {
            goto ErrorReturn;
        }

        if (!ConvertSidToStringSid(pTokenUser->User.Sid, &pszSid))
        {
            goto ErrorReturn;
        }

        sid = std::wstring(pszSid);
        LocalFree(pszSid);
        LocalFree(pTokenUser);
        CloseHandle(hToken);
        return sid;

    ErrorReturn:
        if (hToken != NULL)
            CloseHandle(hToken);

        if (pTokenUser)
            LocalFree(pTokenUser);

        return std::wstring(L"(unavailable)");
    }

    DWORD getCurrentSessionId()
    {
        DWORD sess_id = 999;
        ProcessIdToSessionId(
            GetCurrentProcessId(),
            &sess_id
        );
        return sess_id;
    }

std::wstring to_lower(const std::wstring& input)
{
    std::wstring output = input;
    std::transform(output.begin(), output.end(), output.begin(),
                   [](wchar_t c)->wchar_t { return static_cast<wchar_t>(std::tolower(c)); });
    return output;
}

DWORD getParentPid(DWORD pid)
{
    SnapshotHandleWrapper hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (!hSnapshot) {
        return 0;
    }

    PROCESSENTRY32W pe32{};
    pe32.dwSize = sizeof pe32;

    if (!Process32FirstW(hSnapshot, &pe32)) {
        return 0;
    }

    do {
        if (pe32.th32ProcessID == pid) {
            return pe32.th32ParentProcessID;
        }
    } while (Process32NextW(hSnapshot, &pe32));

    return 0;
}

Integrity getIntegrityLevel(HANDLE hProcess)
{
    TokenHandleWrapper hToken = INVALID_HANDLE_VALUE;

    if (!OpenProcessToken(hProcess, TOKEN_QUERY, &hToken)) {
        return Integrity::Error;
    }

    DWORD dwLengthNeeded = 0;
    GetTokenInformation(hToken,
                        TokenIntegrityLevel,
                        NULL,
                        0,
                        &dwLengthNeeded);
    std::vector<unsigned char> token_data(static_cast<size_t>(dwLengthNeeded), 0);

    const BOOL ret = GetTokenInformation(hToken,
                                         TokenIntegrityLevel,
                                         token_data.data(),
                                         dwLengthNeeded,
                                         &dwLengthNeeded);
    if (!ret) {
        return Integrity::Error;
    }

    auto* const pTIL = reinterpret_cast<PTOKEN_MANDATORY_LABEL>(token_data.data());
    const DWORD dwIntegrityLevel = *GetSidSubAuthority(pTIL->Label.Sid,
                                                       static_cast<DWORD>(static_cast<UCHAR>(*GetSidSubAuthorityCount(pTIL->Label.Sid) - 1)));

    if (dwIntegrityLevel == SECURITY_MANDATORY_LOW_RID) {
        return Integrity::Low;
    }

    if (dwIntegrityLevel >= SECURITY_MANDATORY_MEDIUM_RID &&
        dwIntegrityLevel < SECURITY_MANDATORY_HIGH_RID) {
        return Integrity::Medium;
    }

    if (dwIntegrityLevel >= SECURITY_MANDATORY_HIGH_RID &&
        dwIntegrityLevel < SECURITY_MANDATORY_SYSTEM_RID) {
        return Integrity::High;
    }

    if (dwIntegrityLevel >= SECURITY_MANDATORY_SYSTEM_RID) {
        return Integrity::System;
    }

    return Integrity::Error;
}

DWORD GetPriorityClassByPid(DWORD pid)
{
    ProcessHandleWrapper hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                                FALSE,
                                                pid);
    if (hProcess != NULL) {
        return GetPriorityClass(hProcess);
    }

    return NORMAL_PRIORITY_CLASS;
}

std::wstring getImageEXEPath(DWORD pid)
{
    WCHAR wEXEPath[MAX_PATH] = { 0 };

    ProcessHandleWrapper hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                                FALSE,
                                                pid);

    DWORD size = MAX_PATH - 1;
    constexpr DWORD NO_FLAGS = 0;
    const BOOL res = QueryFullProcessImageNameW(hProcess,
                                                NO_FLAGS,
                                                wEXEPath,
                                                &size);
    if (res == NULL) {
        return L"";
    }

    return std::wstring(wEXEPath);
}

std::wstring getImageName(DWORD pid)
{
    SnapshotHandleWrapper hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (!hSnapshot) {
        return L"(unavailable)";
    }

    PROCESSENTRY32W pe32{};
    pe32.dwSize = sizeof pe32;

    if (!Process32FirstW(hSnapshot, &pe32)) {
        return L"(unavailable)";
    }

    do {
        if (pe32.th32ProcessID == pid) {
            return std::wstring(static_cast<wchar_t*>(pe32.szExeFile));
        }
    } while (Process32NextW(hSnapshot, &pe32));

    return L"(unavailable)";
}

bool isProcessAllowed(const PROCESSENTRY32W& pe32)
{
    ProcessHandleWrapper hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pe32.th32ProcessID);
    if (!hProcess) {
        return false;
    }

    const std::wstring exe_name(pe32.szExeFile);

    const std::array<std::wstring, 3> allow_list{ L"wininit.exe", L"winlogon.exe", L"explorer.exe" };
    for (const std::wstring& allowed_name : allow_list) {
        if (exe_name != allowed_name) {
            continue;
        }

        wchar_t filePath[MAX_PATH] = { 0 };
        DWORD cchFilePath = ARRAYSIZE(filePath) - 1;
       
        // This API is needed when explorer is running at a higher integrity level than raccine.exe. 
        // It can get the imagepath with PROCESS_QUERY_LIMITED_INFORMATION
        if (QueryFullProcessImageName(hProcess, 0, filePath, &cchFilePath)) {
            const std::wstring file_path(utils::to_lower(filePath));

            // Are they in the Windows directory?
            if (file_path.starts_with(L"c:\\windows\\system32\\")) {
                // Is the process running as SYSTEM
                return getIntegrityLevel(hProcess) == Integrity::System;
            }

            // Are you explorer running in the Windows dir
            const std::wstring explorer_path = L"c:\\windows\\explorer.exe";
            if (file_path == explorer_path) {
                // Is the process running as MEDIUM (which Explorer does)
                return (getIntegrityLevel(hProcess) == Integrity::Medium || getIntegrityLevel(hProcess) == Integrity::High);
            }
        }
    }

    return false;
}

std::wstring GetProcessCommandLine(DWORD pid)
{
    std::wstring CommandLine;
    HRESULT hr = 0;
    IWbemLocator* WbemLocator = nullptr;
    IWbemServices* WbemServices = nullptr;
    IEnumWbemClassObject* EnumWbem = nullptr;
    std::wstring Query = L"SELECT CommandLine FROM Win32_Process WHERE ProcessID = " + std::to_wstring(pid);

    // initialize the Windows security
    hr = CoInitializeEx(0, COINIT_MULTITHREADED);
    hr = CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);


    hr = CoCreateInstance(CLSID_WbemLocator,
                          0,
                          CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator,
                          reinterpret_cast<LPVOID*>(&WbemLocator));
    //connect to the WMI
    hr = WbemLocator->ConnectServer(
        _bstr_t(L"ROOT\\CIMV2"), // WMI namespace
        NULL,                      // User name
        NULL,                      // User password
        0,                         // Locale
        NULL,                      // Security flags                 
        0,                         // Authority       
        0,                         // Context object
        &WbemServices              // IWbemServices proxy
    );
    //Run the WQL Query
    hr = WbemServices->ExecQuery(_bstr_t(L"WQL"), _bstr_t(Query.c_str()), WBEM_FLAG_FORWARD_ONLY, NULL, &EnumWbem);

    // Iterate over the enumerator
    if (EnumWbem != nullptr) {
        IWbemClassObject* result = nullptr;
        ULONG returnedCount = 0;

        while ((hr = EnumWbem->Next(WBEM_INFINITE, 1, &result, &returnedCount)) == S_OK) {
            VARIANT procCommandLine{};

            // access the properties
            hr = result->Get(L"CommandLine", 0, &procCommandLine, 0, 0);
            if (hr == S_OK) {
                if (!(procCommandLine.vt == VT_NULL))
                    CommandLine = std::wstring(procCommandLine.bstrVal);
            }

            result->Release();
        }
    }

    // Release the resources
    EnumWbem->Release();
    WbemServices->Release();
    WbemLocator->Release();

    CoUninitialize();
    return CommandLine;
}

BOOL killProcess(DWORD dwProcessId, UINT uExitCode)
{
    constexpr DWORD dwDesiredAccess = PROCESS_TERMINATE;
    constexpr BOOL  bInheritHandle = FALSE;
    ProcessHandleWrapper hProcess = OpenProcess(dwDesiredAccess, bInheritHandle, dwProcessId);
    if (!hProcess) {
        return FALSE;
    }

    return TerminateProcess(hProcess, uExitCode);
}

bool isAllowListed(DWORD pid)
{
    SnapshotHandleWrapper hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (!hSnapshot) {
        return false;
    }

    PROCESSENTRY32W pe32{};
    pe32.dwSize = sizeof pe32;

    if (!Process32FirstW(hSnapshot, &pe32)) {
        return false;
    }

    do {
        if (pe32.th32ProcessID != pid) {
            continue;
        }

        return utils::isProcessAllowed(pe32);
    } while (Process32NextW(hSnapshot, &pe32));

    return false;
}

ProcessDetail::ProcessDetail(DWORD dwPid) :
    ProcessDetailStruct{}
{
    ProcessDetailStruct.dwPid = dwPid;
    //ProcessDetailStruct.Priority = GetPriorityClassByPid(dwPid);

    ProcessDetailStruct.ExeName = getImageName(dwPid);
    std::replace(ProcessDetailStruct.ExeName.begin(), ProcessDetailStruct.ExeName.end(), '"', '\'');

    ProcessDetailStruct.ExePath = getImageEXEPath(dwPid);
    std::replace(ProcessDetailStruct.ExePath.begin(), ProcessDetailStruct.ExePath.end(), '"', '\'');

    ProcessDetailStruct.CommandLine = GetProcessCommandLine(dwPid);
    std::replace(ProcessDetailStruct.CommandLine.begin(), ProcessDetailStruct.CommandLine.end(), '"', '\'');
    ProcessDetailStruct.CommandLine.erase(std::remove(ProcessDetailStruct.CommandLine.begin(), ProcessDetailStruct.CommandLine.end(), '\r'));
    ProcessDetailStruct.CommandLine.erase(std::remove(ProcessDetailStruct.CommandLine.begin(), ProcessDetailStruct.CommandLine.end(), '\n'));

    ProcessDetailStruct.TimeSinceExeCreation = getLastWriteTime(ProcessDetailStruct.ExePath);
}

std::wstring ProcessDetail::ToString(const std::wstring& szPrefix) const
{
    const std::wstring YaraDef = L" -d ";

    std::wstring full_string = YaraDef + L" " + szPrefix + L"Name=\"" + ProcessDetailStruct.ExeName + L"\""
        + YaraDef + L" " + szPrefix + L"ExecutablePath=\"" + ProcessDetailStruct.ExePath + L"\""
        + YaraDef + L" " + szPrefix + L"CommandLine=\"" + ProcessDetailStruct.CommandLine + L"\""
        + YaraDef + L" " + szPrefix + L"TimeSinceExeCreation=" + std::to_wstring(ProcessDetailStruct.TimeSinceExeCreation);

    return full_string;
}

std::wstring ProcessDetail::ToPrintedString(const std::wstring& szPrefix) const
{
    std::wstring full_string = szPrefix + L"Name=\"" + ProcessDetailStruct.ExeName + L"\"\r\n" +
        szPrefix + L"ExecutablePath=\"" + ProcessDetailStruct.ExePath + L"\"\r\n" +
        szPrefix + L"CommandLine=\"" + ProcessDetailStruct.CommandLine + L"\"\r\n" +
        szPrefix + L"TimeSinceExeCreation=" + std::to_wstring(ProcessDetailStruct.TimeSinceExeCreation) + L"\r\n" +
        szPrefix + L"Pid=" + std::to_wstring(ProcessDetailStruct.dwPid) + L"\r\n";

    return full_string;
}

std::wstring expand_environment_strings(const std::wstring& input)
{
    constexpr size_t MAX_STRING_SIZE = 2 << 15;
    std::vector<wchar_t> output(MAX_STRING_SIZE, 0);
    const DWORD ret = ExpandEnvironmentStringsW(input.c_str(),
                                                output.data(),
                                                static_cast<DWORD>(output.size()));
    if (ret == 0) {
        return input;
    }

    return std::wstring(output.data());
}

ULONG getLastWriteTime(const std::wstring& szFilePath)
{
    FileHandleWrapper hFile = CreateFileW(szFilePath.c_str(),
                                          GENERIC_READ,
                                          FILE_SHARE_READ, 
                                          NULL, 
                                          OPEN_EXISTING, 
                                          0,
                                          NULL);
    if (hFile == INVALID_HANDLE_VALUE){
        return 0;
    }

    FILETIME timeFile, timeNow = { 0 };
    SYSTEMTIME stNow = { 0 };
    GetSystemTime(&stNow);
    SystemTimeToFileTime(&stNow, &timeNow);

    if (!GetFileTime(hFile, NULL, NULL, &timeFile))
    {
        return 0;
    }

    ULARGE_INTEGER ulNow = { 0 }, ulFile = { 0 }, ulDiff = { 0 };

    ulNow.HighPart = timeNow.dwHighDateTime;
    ulNow.LowPart = timeNow.dwLowDateTime;
    ulFile.HighPart = timeFile.dwHighDateTime;
    ulFile.LowPart = timeFile.dwLowDateTime;

    memcpy(&ulNow, &timeNow, sizeof(ulNow));
    memcpy(&ulFile, &timeFile, sizeof(timeFile));


    if (ulNow.QuadPart > ulFile.QuadPart)
    {
        ulDiff.QuadPart = ulNow.QuadPart - ulFile.QuadPart;
        const ULONG diff = (static_cast<ULONG>(ulDiff.QuadPart / (10000 * 1000)) / (60 * 60 * 24)); 

        return diff;
    }

    const ULONG  timeDiff = 999999999;
    return timeDiff;
}

bool write_string_to_file(const std::filesystem::path& file_path, const std::wstring& string_to_write)
{
    //  Creates the new file to write to for the upper-case version.
    FileHandleWrapper hTempFile = CreateFileW(file_path.c_str(),    // file name 
                                              GENERIC_WRITE,        // open for write 
                                              0,                    // do not share 
                                              NULL,                 // default security 
                                              CREATE_ALWAYS,        // overwrite existing
                                              FILE_ATTRIBUTE_NORMAL,// normal file 
                                              NULL);     // no template 
    if (!hTempFile) {
        return false;
    }

    DWORD dwWritten = 0;

    std::optional<std::string> ansi_command_line = utils::convert_wstring_to_string(string_to_write);

    if (!ansi_command_line.has_value()) {
        return false;
    }

    // TODO: check for error and handle
    WriteFile(hTempFile,
              ansi_command_line->c_str(),
              static_cast<DWORD>(ansi_command_line->length()),
              &dwWritten, 
              NULL);

    return true;
}

std::optional<std::string> convert_wstring_to_string(const std::wstring& input)
{
    std::vector<char> ansi_command_line(input.length() + 1, 0);

    const int ret = WideCharToMultiByte(
        CP_ACP,
        0,
        input.c_str(),
        static_cast<int>(input.length()),
        ansi_command_line.data(),
        static_cast<int>(input.size()),
        NULL,
        NULL
    );
    if (ret == 0) {
        return std::nullopt;
    }

    return std::string(ansi_command_line.data());
}

std::wstring getFileName(const std::wstring& s)
{
    wchar_t sep = '/';

#ifdef _WIN32
    sep = '\\';
#endif

    size_t i = s.rfind(sep, s.length());
    if (i != std::wstring::npos)
    {
        return(s.substr(i + 1, s.length() - i));
    }

    return(s);
}

int removeNewLines(std::wstring& str)
{
    std::replace(str.begin(), str.end(), L'\r', L' ');
    std::replace(str.begin(), str.end(), L'\n', L' ');
    return 0;
}

}
