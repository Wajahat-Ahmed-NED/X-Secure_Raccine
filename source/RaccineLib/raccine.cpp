// Raccine 
// A Simple Ransomware Vaccine
// https://github.com/Neo23x0/Raccine
//
// Florian Roth, Ollie Whitehouse, Branislav Djalic, John Lambert
// with help of Hilko Bengen

#include <cwchar>
#include <Windows.h>
#include <cstdio>
#include <string>
#include <array>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <strsafe.h>
#include <Shlwapi.h>
#include <vector>
#include <thread>

#include "Raccine.h"

#include "HandleWrapper.h"
#include "Utils.h"
#include "YaraRuleRunner.h"
#include "EventLogHelper.h"
#pragma comment(lib,"advapi32.lib")
#pragma comment(lib,"shlwapi.lib")
#pragma comment(lib,"Wbemuuid.lib")

bool EvaluateYaraRules(const RaccineConfig& raccine_config,
    const std::wstring& lpCommandLine,
    std::wstring& outYaraOutput,
    DWORD dwChildPid,
    DWORD dwParentPid,
    DWORD dwGrandParentPid)
{

    std::wstring recent_event_details(L"\r\n\r\nRecentEvents:");

    if (raccine_config.is_debug_mode()) {
        wprintf(L"Running YARA on: %s\n", lpCommandLine.c_str());
    }

    if (raccine_config.use_eventlog_data_in_rules())
    {
        recent_event_details += eventloghelper::GetEvents();
    }

    WCHAR wTestFilename[MAX_PATH] = { 0 };

    ExpandEnvironmentStringsW(RACCINE_USER_CONTEXT_DIRECTORY, wTestFilename, ARRAYSIZE(wTestFilename) - 1);

    if (std::filesystem::create_directories(wTestFilename) == false) {
        if (raccine_config.is_debug_mode()) {
            wprintf(L"Unable to create temporary user folder: %s\n", wTestFilename);
        }
    }

    const int c = GetTempFileNameW(wTestFilename, L"Raccine", 0, wTestFilename);
    if (c == 0) {
        return false;
    }
    utils::write_string_to_file(wTestFilename, lpCommandLine + recent_event_details);

    BOOL fSuccess = TRUE;

    std::wstring childContext = CreateContextForProgram(dwChildPid, L"");

    std::wstring parentContext = CreateContextForProgram(dwParentPid, L"Parent");

    std::wstring grandparentContext;
    if (dwGrandParentPid != 0) {
        grandparentContext = CreateContextForProgram(dwGrandParentPid, L"GrandParent");
    }

    std::wstring combinedContext = childContext + L" " + parentContext +L" " + grandparentContext;

    if (raccine_config.is_debug_mode()) {
        wprintf(L"Composed test-string is: %s\n", combinedContext.c_str());
        wprintf(L"Everything OK? %d\n", fSuccess);
    }

    bool fRetVal = false;

    if (fSuccess) {
        //wprintf(L"Checking rule dir: %s", raccine_config.yara_rules_directory().c_str());
        YaraRuleRunner rule_runner(raccine_config.yara_rules_directory(),
            utils::expand_environment_strings(RACCINE_PROGRAM_DIRECTORY));
        fRetVal = rule_runner.run_yara_rules_on_file(wTestFilename, lpCommandLine, outYaraOutput, combinedContext);

        if (raccine_config.scan_memory())
        {
            YaraRuleRunner rule_runner_process(raccine_config.yara_in_memory_rules_directory(),
                utils::expand_environment_strings(RACCINE_PROGRAM_DIRECTORY));
            BOOL fProcessRetVal = rule_runner_process.run_yara_rules_on_process(dwParentPid, lpCommandLine, outYaraOutput, combinedContext);
            if (!fRetVal)
                fRetVal = fProcessRetVal;
        }
    }
    if (!raccine_config.is_debug_mode()) {
        DeleteFileW(wTestFilename);
    }

    // szYaraOutput comes formated with \0s to the end of the buffer
    outYaraOutput = outYaraOutput.substr(0, outYaraOutput.find(L'\0'));

    return fRetVal;
}

std::wstring CreateContextForProgram(DWORD pid, const std::wstring& szDefinePrefix)
{
    const utils::ProcessDetail details(pid);

    std::wstring strDetails = details.ToString(szDefinePrefix);

    return strDetails;
}

void WriteEventLogEntryWithId(const std::wstring& pszMessage, DWORD dwEventId)
{
    constexpr LPCWSTR LOCAL_COMPUTER = nullptr;
    EventSourceHandleWrapper hEventSource = RegisterEventSourceW(LOCAL_COMPUTER,
        L"Raccine");
    if (!hEventSource) {
        return;
    }

    LPCWSTR lpszStrings[2] = { pszMessage.c_str() , nullptr };

    // Select an eventlog message type
    WORD eventType = EVENTLOG_INFORMATION_TYPE;
    if (dwEventId == RACCINE_EVENTID_MALICIOUS_ACTIVITY) {
        eventType = EVENTLOG_WARNING_TYPE;
    }

    constexpr PSID NO_USER_SID = nullptr;
    constexpr LPVOID NO_BINARY_DATA = nullptr;
    ReportEventW(hEventSource,      // Event log handle
        eventType,                  // Event type
        0,                          // Event category
        dwEventId,                  // Event identifier
        NO_USER_SID,                // No security identifier
        1,                          // Size of lpszStrings array
        0,                          // No binary data
        lpszStrings,                // Array of strings
        NO_BINARY_DATA              // No binary data
    );
}

void WriteEventLogEntry(const std::wstring& pszMessage)
{
    WriteEventLogEntryWithId(pszMessage, RACCINE_DEFAULT_EVENTID);
}

bool needs_powershell_workaround(const std::wstring& command_line)
{
    if (command_line.find(L"-File ") != std::wstring::npos &&
        command_line.find(L".ps") != std::wstring::npos &&
        command_line.find(L"powershell") == std::wstring::npos) {
        return true;
    }

    return false;
}

void trigger_gui_event()
{
    constexpr BOOL DO_NOT_INHERIT = FALSE;
    EventHandleWrapper hEvent = OpenEventW(EVENT_MODIFY_STATE,
        DO_NOT_INHERIT,
        L"RaccineAlertEvent");
    if (hEvent) {
        SetEvent(hEvent);
    }
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

std::string getTimeStamp()
{
    struct tm buf {};
    auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now() - std::chrono::hours(24));
    localtime_s(&buf, &time);
    std::stringstream ss;
    ss << std::put_time(&buf, "%F %T");
    auto timestamp = ss.str();
    return timestamp;
}

std::wstring logFormat(const std::wstring& cmdLine, const std::wstring& comment)
{
    const std::string timeString = getTimeStamp();
    const std::wstring timeStringW(timeString.cbegin(), timeString.cend());
    std::wstring logLine = timeStringW + L" DETECTED_CMD: '" + cmdLine + L"' COMMENT: " + comment + L"\n";
    return logLine;
}

std::wstring logFormatLine(const std::wstring& line)
{
    const std::string timeString = getTimeStamp();
    const std::wstring timeStringW(timeString.cbegin(), timeString.cend());
    std::wstring logLine = timeStringW + L" " + line + L"\n";
    return logLine;
}

std::wstring logFormatAction(DWORD pid, const std::wstring& imageName, const std::wstring& cmdLine, const std::wstring& comment)
{
    const std::string timeString = getTimeStamp();
    const std::wstring timeStringW(timeString.cbegin(), timeString.cend());
    std::wstring logLine = timeStringW + L" DETECTED_CMD: '" + cmdLine + L"' IMAGE: '" + imageName + L"' PID: " + std::to_wstring(pid) + L" ACTION: " + comment + L"\n";
    return logLine;
}

void logSend(const std::wstring& logStr)
{
    static FILE* logFile = nullptr;
    if (logFile == nullptr) {
        const std::filesystem::path raccine_data_directory = utils::expand_environment_strings(RACCINE_DATA_DIRECTORY);
        const std::filesystem::path raccine_log_file_path = raccine_data_directory / L"Raccine_log.txt";
        errno_t err = _wfopen_s(&logFile, raccine_log_file_path.c_str(), L"at");

        if (err != 0) {
            err = _wfopen_s(&logFile, raccine_log_file_path.c_str(), L"wt");
        }

        if (err != 0) {
            wprintf(L"\nCan not open %s for writing.\n", raccine_log_file_path.c_str());
            return;   // bail out if we can't log
        }
    }
    // Replace new line characters
    std::wstring logString = logStr;
    utils::removeNewLines(logString);

    if (logFile != nullptr) {
        fwprintf(logFile, L"%s\n", logString.c_str());
        fflush(logFile);
        fclose(logFile);
        logFile = nullptr;
    }
}

std::tuple<DWORD, ProcessHandleWrapper, ThreadHandleWrapper> createChildProcessWithDebugger(LPWSTR lpzchildCommandLine,
    DWORD dwAdditionalCreateParams)
{

    STARTUPINFO info = { sizeof(info) };
    PROCESS_INFORMATION processInfo{};

    constexpr LPCWSTR NO_APPLICATION_NAME = nullptr;
    constexpr LPSECURITY_ATTRIBUTES DEFAULT_SECURITY_ATTRIBUTES = nullptr;
    constexpr BOOL INHERIT_HANDLES = TRUE;
    constexpr LPVOID USE_CALLER_ENVIRONMENT = nullptr;
    constexpr LPCWSTR USE_CALLER_WORKING_DIRECTORY = nullptr;


    const BOOL res = CreateProcessW(NO_APPLICATION_NAME,
        lpzchildCommandLine,
        DEFAULT_SECURITY_ATTRIBUTES,
        DEFAULT_SECURITY_ATTRIBUTES,
        INHERIT_HANDLES,
        DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS | dwAdditionalCreateParams,
        USE_CALLER_ENVIRONMENT,
        USE_CALLER_WORKING_DIRECTORY,
        &info,
        &processInfo);
    if (res == 0) {
        return { 0, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE };
    }

    DebugActiveProcessStop(processInfo.dwProcessId);

    return { processInfo.dwProcessId, processInfo.hProcess, processInfo.hThread };
}

std::set<DWORD> find_processes_to_kill(const std::wstring& sCommandLine, std::wstring& sListLogs)
{
    std::set<DWORD> pids;
    DWORD pid = GetCurrentProcessId();

    while (true) {
        pid = utils::getParentPid(pid);
        if (pid == 0) {
            break;
        }

        const std::wstring imageName = utils::getImageName(pid);

        if (!isAllowListed(pid)) {
            wprintf(L"\nCollecting IMAGE %s with PID %d for a kill\n", imageName.c_str(), pid);
            pids.insert(pid);
        }
        else {
            wprintf(L"\nProcess IMAGE %s with PID %d is on allowlist\n", imageName.c_str(), pid);
            sListLogs.append(logFormatAction(pid, imageName, sCommandLine, L"Whitelisted"));
        }
    }

    return pids;
}

void find_and_kill_processes(bool log_only, const std::wstring& sCommandLine, std::wstring& sListLogs)
{
    const std::set<DWORD> pids = find_processes_to_kill(sCommandLine, sListLogs);

    for (DWORD process_id : pids) {
        const std::wstring imageName = utils::getImageName(process_id);
        // If no simulation flag is set
        if (!log_only) {
            // Kill
            wprintf(L"Kill process IMAGE %s with PID %d\n", imageName.c_str(), process_id);
            utils::killProcess(process_id, 1);
            sListLogs.append(logFormatAction(process_id, imageName, sCommandLine, L"Terminated"));
        }
        else {
            // Simulated kill
            wprintf(L"Simulated Kill IMAGE %s with PID %d\n", imageName.c_str(), process_id);
            sListLogs.append(logFormatAction(process_id, imageName, sCommandLine, L"Terminated (Simulated)"));
        }
    }

    printf("\nRaccine v%s finished\n", VER_FILEVERSION_STR);
    //std::this_thread::sleep_for(std::chrono::seconds(5));
}
