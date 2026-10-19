#include "test_support.h"

#include <arpa/inet.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <netinet/in.h>
#include <random>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace test_support {

namespace {

std::string trim(std::string text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    return text;
}

bool tcpPortAccepts(int port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    const bool connected = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    ::close(fd);
    return connected;
}

int publishedPort(const std::string& containerId, int containerPort)
{
    // "docker port" prints e.g. "127.0.0.1:49153".
    const CommandResult result = runCommand("docker port " + containerId + " " + std::to_string(containerPort) + "/tcp");
    const auto colon = result.output.rfind(':');
    if (result.exit_code != 0 || colon == std::string::npos)
        throw std::runtime_error("Cannot read published port of container " + containerId + ": " + result.output);
    return std::stoi(result.output.substr(colon + 1));
}

} // namespace

TempDir::TempDir(const std::string& prefix)
{
    std::random_device device;
    const auto base = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto candidate = base / (prefix + "-" + std::to_string(::getpid()) + "-" + std::to_string(device()));
        if (std::filesystem::create_directory(candidate)) {
            path_ = candidate;
            return;
        }
    }
    throw std::runtime_error("Cannot create a temporary directory");
}

TempDir::~TempDir()
{
    const char* keep = std::getenv("ALGOTRADING_TEST_KEEP_TMP");
    if (keep && std::string(keep) == "1")
        return;
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
}

void writeFile(const std::filesystem::path& path, const std::string& content)
{
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("Cannot write " + path.string());
    output << content;
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot read " + path.string());
    std::stringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::filesystem::path sourcePath(const std::string& relativePath)
{
    const char* root = std::getenv("ALGOTRADING_SOURCE_ROOT");
    if (!root)
        throw std::runtime_error("ALGOTRADING_SOURCE_ROOT is not set (run through meson test)");
    return std::filesystem::path(root) / relativePath;
}

std::filesystem::path builtExecutable(const std::string& environmentName)
{
    const char* value = std::getenv(environmentName.c_str());
    if (!value)
        throw std::runtime_error(environmentName + " is not set (run through meson test)");
    return value;
}

bool waitUntil(const std::function<bool()>& condition,
               std::chrono::milliseconds timeout,
               std::chrono::milliseconds interval)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        if (condition())
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(interval);
    }
}

CommandResult runCommand(const std::string& command)
{
    CommandResult result;
    FILE* pipe = ::popen((command + " 2>&1").c_str(), "r");
    if (!pipe)
        return result;
    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), pipe))
        result.output += buffer;
    const int status = ::pclose(pipe);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

ChildProcess::ChildProcess(const std::filesystem::path& executable,
                           const std::vector<std::string>& arguments,
                           const std::filesystem::path& logFile,
                           const std::map<std::string, std::string>& extraEnvironment)
    : logFile_(logFile)
{
    std::vector<std::string> argumentStorage{executable.string()};
    argumentStorage.insert(argumentStorage.end(), arguments.begin(), arguments.end());

    // The child inherits this process's environment plus the extra variables.
    std::map<std::string, std::string> environment;
    for (char** entry = environ; *entry; ++entry) {
        const std::string text = *entry;
        const auto equals = text.find('=');
        if (equals != std::string::npos)
            environment[text.substr(0, equals)] = text.substr(equals + 1);
    }
    for (const auto& [name, value] : extraEnvironment)
        environment[name] = value;
    std::vector<std::string> environmentStorage;
    for (const auto& [name, value] : environment)
        environmentStorage.push_back(name + "=" + value);

    if (logFile.has_parent_path())
        std::filesystem::create_directories(logFile.parent_path());

    pid_ = ::fork();
    if (pid_ < 0)
        throw std::runtime_error("fork failed");
    if (pid_ == 0) {
        const int fd = ::open(logFile.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            ::dup2(fd, STDOUT_FILENO);
            ::dup2(fd, STDERR_FILENO);
            ::close(fd);
        }
        ::setpgid(0, 0);
        std::vector<char*> argv;
        for (auto& argument : argumentStorage)
            argv.push_back(argument.data());
        argv.push_back(nullptr);
        std::vector<char*> envp;
        for (auto& entry : environmentStorage)
            envp.push_back(entry.data());
        envp.push_back(nullptr);
        ::execve(argv[0], argv.data(), envp.data());
        ::_exit(127);
    }
}

ChildProcess::~ChildProcess()
{
    if (running())
        stop(std::chrono::seconds(5));
}

bool ChildProcess::running()
{
    if (exitCode_ || pid_ <= 0)
        return false;
    int status = 0;
    const int result = ::waitpid(pid_, &status, WNOHANG);
    if (result == 0)
        return true;
    exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return false;
}

std::optional<int> ChildProcess::waitForExit(std::chrono::milliseconds timeout)
{
    waitUntil([this] { return !running(); }, timeout, std::chrono::milliseconds(20));
    return exitCode_;
}

int ChildProcess::stop(std::chrono::milliseconds grace)
{
    if (!running())
        return exitCode_.value_or(-1);
    ::kill(pid_, SIGTERM);
    if (!waitForExit(grace)) {
        ::kill(pid_, SIGKILL);
        waitForExit(std::chrono::seconds(5));
    }
    return exitCode_.value_or(-1);
}

void ChildProcess::kill()
{
    if (!running())
        return;
    ::kill(pid_, SIGKILL);
    waitForExit(std::chrono::seconds(5));
}

std::string ChildProcess::log() const
{
    std::error_code ignored;
    if (!std::filesystem::exists(logFile_, ignored))
        return {};
    return readFile(logFile_);
}

bool dockerAvailable()
{
    static const bool available = runCommand("docker info --format '{{.ServerVersion}}'").exit_code == 0;
    return available;
}

std::unique_ptr<DockerContainer> DockerContainer::startNats()
{
    std::unique_ptr<DockerContainer> container(new DockerContainer());
    const CommandResult started = runCommand(
        "docker run -d --rm -p 127.0.0.1::4222 nats:2.10-alpine -js");
    if (started.exit_code != 0)
        throw std::runtime_error("Cannot start NATS container: " + started.output);
    container->id_ = trim(started.output);
    container->containerPort_ = 4222;
    container->hostPort_ = publishedPort(container->id_, 4222);
    container->url_ = "nats://127.0.0.1:" + std::to_string(container->hostPort_);
    container->ready_ = [port = container->hostPort_] { return tcpPortAccepts(port); };
    container->waitReady();
    return container;
}

std::unique_ptr<DockerContainer> DockerContainer::startPostgres()
{
    std::unique_ptr<DockerContainer> container(new DockerContainer());
    const CommandResult started = runCommand(
        "docker run -d --rm -p 127.0.0.1::5432 -e POSTGRES_USER=test -e POSTGRES_PASSWORD=test "
        "-e POSTGRES_DB=algotrading_test postgres:16-alpine");
    if (started.exit_code != 0)
        throw std::runtime_error("Cannot start PostgreSQL container: " + started.output);
    container->id_ = trim(started.output);
    container->containerPort_ = 5432;
    container->hostPort_ = publishedPort(container->id_, 5432);
    container->url_ = "host=127.0.0.1 port=" + std::to_string(container->hostPort_) +
                      " dbname=algotrading_test user=test password=test";
    // The image restarts the server once after initialization; require a real query
    // over TCP, not only pg_isready on the socket.
    container->ready_ = [id = container->id_] {
        return runCommand("docker exec " + id +
                          " psql -h 127.0.0.1 -U test -d algotrading_test -c 'select 1'").exit_code == 0;
    };
    container->waitReady();
    return container;
}

void DockerContainer::waitReady()
{
    if (!waitUntil(ready_, std::chrono::seconds(60), std::chrono::milliseconds(200)))
        throw std::runtime_error("Container " + id_ + " did not become ready");
}

void DockerContainer::restart()
{
    runCommand("docker restart " + id_);
    // Docker may publish a different host port after a restart.
    const int newPort = publishedPort(id_, containerPort_);
    const std::string oldPort = std::to_string(hostPort_);
    const auto position = url_.find(oldPort);
    if (position != std::string::npos)
        url_.replace(position, oldPort.size(), std::to_string(newPort));
    hostPort_ = newPort;
    if (containerPort_ == 4222)
        ready_ = [port = hostPort_] { return tcpPortAccepts(port); };
    waitReady();
}

DockerContainer::~DockerContainer()
{
    if (!id_.empty())
        runCommand("docker rm -f " + id_);
}

} // namespace test_support
