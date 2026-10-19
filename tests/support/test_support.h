#pragma once

// Shared helpers for tests/: temporary folders, child processes and the Docker-run
// NATS/PostgreSQL instances that service and chain tests talk to.

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace test_support {

// A fresh folder under the system temp directory, removed when the object goes away
// (kept when ALGOTRADING_TEST_KEEP_TMP=1, to inspect a failure).
class TempDir {
public:
    explicit TempDir(const std::string& prefix = "algotrading-test");
    ~TempDir();

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path operator/(const std::string& name) const { return path_ / name; }

private:
    std::filesystem::path path_;
};

void writeFile(const std::filesystem::path& path, const std::string& content);
std::string readFile(const std::filesystem::path& path);

// Absolute path of a file in the source tree (meson test sets ALGOTRADING_SOURCE_ROOT).
std::filesystem::path sourcePath(const std::string& relativePath);

// Absolute path of a built executable; meson test passes them as ALGOTRADING_BIN_<NAME>.
// Throws when the variable is not set.
std::filesystem::path builtExecutable(const std::string& environmentName);

// Polls `condition` every `interval` until it is true or `timeout` expires.
bool waitUntil(const std::function<bool()>& condition,
               std::chrono::milliseconds timeout,
               std::chrono::milliseconds interval = std::chrono::milliseconds(50));

// Runs a shell command and returns its exit code and standard output.
struct CommandResult {
    int exit_code = -1;
    std::string output;
};
CommandResult runCommand(const std::string& command);

// A child process with stdout/stderr redirected to a log file. The destructor stops it
// (SIGTERM, then SIGKILL after a grace period).
class ChildProcess {
public:
    ChildProcess(const std::filesystem::path& executable,
                 const std::vector<std::string>& arguments,
                 const std::filesystem::path& logFile,
                 const std::map<std::string, std::string>& extraEnvironment = {});
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool running();
    // SIGTERM and wait; returns the exit code (or 128 + signal).
    int stop(std::chrono::milliseconds grace = std::chrono::seconds(10));
    // SIGKILL without any shutdown work, to simulate a crash.
    void kill();
    // Waits for the process to exit by itself; nullopt on timeout.
    std::optional<int> waitForExit(std::chrono::milliseconds timeout);
    std::string log() const;
    const std::filesystem::path& logFile() const { return logFile_; }

private:
    int pid_ = -1;
    std::optional<int> exitCode_;
    std::filesystem::path logFile_;
};

// A throwaway Docker container (NATS with JetStream, or PostgreSQL) on a random
// loopback port. Tests call GTEST_SKIP() when dockerAvailable() is false.
bool dockerAvailable();

class DockerContainer {
public:
    ~DockerContainer();
    DockerContainer(const DockerContainer&) = delete;
    DockerContainer& operator=(const DockerContainer&) = delete;
    DockerContainer(DockerContainer&&) = delete;

    static std::unique_ptr<DockerContainer> startNats();
    static std::unique_ptr<DockerContainer> startPostgres();

    int hostPort() const { return hostPort_; }
    // nats://127.0.0.1:<port> or a libpq connection string.
    const std::string& url() const { return url_; }
    void restart();

private:
    DockerContainer() = default;
    std::string id_;
    int containerPort_ = 0;
    int hostPort_ = 0;
    std::string url_;
    std::function<bool()> ready_;
    void waitReady();
};

} // namespace test_support
