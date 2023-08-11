#include "pch.h"



#include "../../source/RaccineLib/HandleWrapper.h"
#include "../../source/RaccineLib/Raccine.h"
#include "../../source/RaccineLib/Utils.h"

TEST(TestUtils, ToLower)
{
    const std::wstring input = L"HellO WoRld";
    std::wstring excepted_output = L"hello world";
    EXPECT_EQ(excepted_output, utils::to_lower(input));
}

TEST(TestGetImageName, System)
{
    std::wstring image_name = utils::getImageName(4);
    ASSERT_EQ(image_name, L"System");
}

TEST(TestGetImageName, NonExistant)
{
    std::wstring image_name = utils::getImageName(3);
    ASSERT_EQ(image_name, L"(unavailable)");
}

TEST(TestGetImageName, CurrentProcess)
{
    std::wstring image_name = utils::getImageName(GetCurrentProcessId());
    ASSERT_EQ(image_name, L"Raccine-Test.exe");
}

TEST(TestGetParentPid, System)
{
    DWORD parent_pid = utils::getParentPid(4);
    ASSERT_EQ(parent_pid, 0);
}

TEST(TestGetParentPid, NonExistant)
{
    DWORD parent_pid = utils::getParentPid(3);
    ASSERT_EQ(parent_pid, 0);
}

TEST(TestGetIntegrityLevel, CurrentProcess)
{
    ProcessHandleWrapper hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 
                                                FALSE, 
                                                GetCurrentProcessId());
    if (!hProcess) {
        FAIL() << "Failed to open process";
    }

    const utils::Integrity integrity = utils::getIntegrityLevel(hProcess);
    EXPECT_TRUE(integrity == utils::Integrity::Medium || integrity == utils::Integrity::High);
}

TEST(TestGetIntegrityLevel, InvalidHAndle)
{
    const utils::Integrity integrity = utils::getIntegrityLevel(reinterpret_cast<HANDLE>(1));
    EXPECT_TRUE(integrity == utils::Integrity::Error);
}

TEST(TestExpandEnvironmentStrings, RaccineDataDirectory)
{
    std::wstring result = utils::expand_environment_strings(RACCINE_DATA_DIRECTORY);
    EXPECT_EQ(result, L"C:\\ProgramData\\Raccine");
}

TEST(TestFindProcessesToKill, Parent)
{
    const std::wstring command_line = L"TEST_COMMAND_LINE";
    std::wstring logs;
    const std::set<DWORD> pids = find_processes_to_kill(command_line, logs);
    EXPECT_FALSE(pids.empty());

    const DWORD parent_pid = utils::getParentPid(GetCurrentProcessId());
    EXPECT_TRUE(pids.contains(parent_pid));

    // TODO: test logs output
}

TEST(TestFindProcessesToKill, System)
{
    const std::wstring command_line = L"TEST_COMMAND_LINE";
    std::wstring logs;
    const std::set<DWORD> pids = find_processes_to_kill(command_line, logs);
    EXPECT_FALSE(pids.empty());

    EXPECT_FALSE(pids.contains(4));
}

TEST(TestFindProcessesToKill, NonExistant)
{
    const std::wstring command_line = L"TEST_COMMAND_LINE";
    std::wstring logs;
    const std::set<DWORD> pids = find_processes_to_kill(command_line, logs);
    EXPECT_FALSE(pids.empty());

    EXPECT_FALSE(pids.contains(3));
}
