#include <loglib/user_paths.hpp>

#include <catch2/catch_all.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdlib>
#endif

using namespace loglib;

namespace
{

#ifdef _WIN32
class EnvVarGuard
{
public:
    explicit EnvVarGuard(std::wstring name)
        : mName(std::move(name)), mHad(Read(mName, mPrevious))
    {
    }

    ~EnvVarGuard()
    {
        if (mHad)
        {
            Set(mName, mPrevious);
        }
        else
        {
            Unset(mName);
        }
    }

    EnvVarGuard(const EnvVarGuard &) = delete;
    EnvVarGuard &operator=(const EnvVarGuard &) = delete;

    static void Set(const std::wstring &name, const std::wstring &value)
    {
        SetEnvironmentVariableW(name.c_str(), value.c_str());
    }

    static void Unset(const std::wstring &name)
    {
        SetEnvironmentVariableW(name.c_str(), nullptr);
    }

private:
    static bool Read(const std::wstring &name, std::wstring &value)
    {
        const DWORD needed = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
        if (needed == 0)
        {
            return false;
        }
        value.assign(static_cast<size_t>(needed), L'\0');
        const DWORD written = GetEnvironmentVariableW(name.c_str(), value.data(), needed);
        if (written == 0)
        {
            return false;
        }
        value.resize(written);
        return true;
    }

    std::wstring mName;
    std::wstring mPrevious;
    bool mHad = false;
};
#else
class EnvVarGuard
{
public:
    explicit EnvVarGuard(std::string name)
        : mName(std::move(name)), mHad(Capture(mName, mPrevious))
    {
    }

    ~EnvVarGuard()
    {
        if (mHad)
        {
            Set(mName, mPrevious);
        }
        else
        {
            Unset(mName);
        }
    }

    EnvVarGuard(const EnvVarGuard &) = delete;
    EnvVarGuard &operator=(const EnvVarGuard &) = delete;

    static void Set(const std::string &name, const std::string &value)
    {
        setenv(name.c_str(), value.c_str(), 1);
    }

    static void Unset(const std::string &name)
    {
        unsetenv(name.c_str());
    }

private:
    static bool Capture(const std::string &name, std::string &previous)
    {
        const char *value = std::getenv(name.c_str());
        if (value == nullptr)
        {
            return false;
        }
        previous = value;
        return true;
    }

    std::string mName;
    std::string mPrevious;
    bool mHad = false;
};
#endif

class TempDir
{
public:
    TempDir()
    {
        static int counter = 0;
        mPath = std::filesystem::temp_directory_path() /
                ("slv-user-paths-" + std::to_string(++counter) + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(mPath);
    }

    // NOLINTNEXTLINE(bugprone-exception-escape): `remove_all` takes an error_code; MSVC still models a throw.
    ~TempDir() noexcept
    {
        std::error_code error;
        std::filesystem::remove_all(mPath, error);
    }

    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    [[nodiscard]] const std::filesystem::path &Path() const
    {
        return mPath;
    }

private:
    std::filesystem::path mPath;
};

void WriteFile(const std::filesystem::path &path, std::string_view bytes)
{
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out);
}

} // namespace

#ifdef _WIN32

TEST_CASE("UserDataDirectory appends the application identity to APPDATA", "[user_paths]")
{
    const EnvVarGuard appData(L"APPDATA");
    EnvVarGuard::Set(L"APPDATA", L"C:\\slv-appdata");

    CHECK(
        UserDataDirectory() ==
        std::filesystem::path(L"C:\\slv-appdata") / USER_DATA_ORGANIZATION_NAME / USER_DATA_APPLICATION_NAME
    );
    CHECK(UserRegexTemplateDirectory() == UserDataDirectory() / "regex_templates");
}

TEST_CASE("UserDataDirectory is empty when APPDATA is missing", "[user_paths]")
{
    const EnvVarGuard appData(L"APPDATA");
    EnvVarGuard::Unset(L"APPDATA");

    CHECK(UserDataDirectory().empty());
    CHECK(UserRegexTemplateDirectory().empty());
}

#elifdef __APPLE__

TEST_CASE("UserDataDirectory uses Application Support under HOME", "[user_paths]")
{
    const EnvVarGuard home("HOME");
    EnvVarGuard::Set("HOME", "/tmp/slv-home");

    CHECK(
        UserDataDirectory() == std::filesystem::path("/tmp/slv-home/Library/Application Support") /
                                   USER_DATA_ORGANIZATION_NAME / USER_DATA_APPLICATION_NAME
    );
    CHECK(UserRegexTemplateDirectory() == UserDataDirectory() / "regex_templates");
}

#else

TEST_CASE("UserDataDirectory prefers an absolute XDG_DATA_HOME", "[user_paths]")
{
    const EnvVarGuard home("HOME");
    const EnvVarGuard xdg("XDG_DATA_HOME");
    EnvVarGuard::Set("HOME", "/tmp/slv-home");
    EnvVarGuard::Set("XDG_DATA_HOME", "/tmp/slv-xdg");

    CHECK(
        UserDataDirectory() ==
        std::filesystem::path("/tmp/slv-xdg") / USER_DATA_ORGANIZATION_NAME / USER_DATA_APPLICATION_NAME
    );
}

TEST_CASE("UserDataDirectory ignores a relative XDG_DATA_HOME", "[user_paths]")
{
    const EnvVarGuard home("HOME");
    const EnvVarGuard xdg("XDG_DATA_HOME");
    EnvVarGuard::Set("HOME", "/tmp/slv-home");
    EnvVarGuard::Set("XDG_DATA_HOME", "relative/share");

    CHECK(
        UserDataDirectory() ==
        std::filesystem::path("/tmp/slv-home/.local/share") / USER_DATA_ORGANIZATION_NAME / USER_DATA_APPLICATION_NAME
    );
}

TEST_CASE("UserDataDirectory falls back when XDG_DATA_HOME is missing", "[user_paths]")
{
    const EnvVarGuard home("HOME");
    const EnvVarGuard xdg("XDG_DATA_HOME");
    EnvVarGuard::Set("HOME", "/tmp/slv-home");
    EnvVarGuard::Unset("XDG_DATA_HOME");

    CHECK(
        UserDataDirectory() ==
        std::filesystem::path("/tmp/slv-home/.local/share") / USER_DATA_ORGANIZATION_NAME / USER_DATA_APPLICATION_NAME
    );
    CHECK(UserRegexTemplateDirectory() == UserDataDirectory() / "regex_templates");
}

#endif

TEST_CASE("LoadRegexTemplatesFromDirectory defaults priority and skips malformed files", "[user_paths]")
{
    const TempDir dir;
    WriteFile(dir.Path() / "b.json", R"({"name":"Beta","pattern":"^(?<message>.*)$","sampleLines":["hello"]})");
    WriteFile(
        dir.Path() / "a.json", R"({"name":"Alpha","pattern":"^(?<message>.*)$","sampleLines":["hello"],"priority":7})"
    );
    WriteFile(dir.Path() / "broken.json", "{");
    WriteFile(dir.Path() / "notes.txt", "not a template");

    const RegexTemplateLoadResult loaded = LoadRegexTemplatesFromDirectory(dir.Path());

    REQUIRE(loaded.templates.size() == 2);
    CHECK(loaded.templates[0].name == "Alpha");
    CHECK(loaded.templates[0].priority == 7);
    CHECK(loaded.templates[1].name == "Beta");
    CHECK(loaded.templates[1].priority == USER_TEMPLATE_DEFAULT_PRIORITY);
    REQUIRE(loaded.errors.size() == 1);
    CHECK(loaded.errors[0].contains("broken.json"));
}

TEST_CASE("LoadRegexTemplatesFromDirectory tolerates a missing directory", "[user_paths]")
{
    const std::filesystem::path missing = std::filesystem::temp_directory_path() / "slv-user-paths-missing-8f3c1a";
    std::filesystem::remove_all(missing);

    const RegexTemplateLoadResult loaded = LoadRegexTemplatesFromDirectory(missing);

    CHECK(loaded.templates.empty());
    CHECK(loaded.errors.empty());
}
