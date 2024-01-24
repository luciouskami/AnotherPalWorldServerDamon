#include <WinSock2.h>
#include <Windows.h>
#include "RConClient.hpp"
#include <toml.hpp>
#include <TlHelp32.h>
#include <filesystem>
#include <format>

using toml_t = decltype(toml::parse_file(""));

namespace fs = std::filesystem;

struct rcon_config {
    std::string_view server_ip;
    std::string_view server_port;
    std::string_view password;
    ;
};

struct damon_config {
    fs::path server_path;
    std::string_view server_process_name;
    uint64_t memory_thresholds;
};

struct config {
    rcon_config rcon;
    damon_config damon;
};

namespace {
    toml_t g_toml_config;
    config g_config;
}

namespace util {
    toml_t load_config(const fs::path& path) {
        return toml::parse_file(path.generic_string());
    }

    std::string send_command(const std::string& command) {
        RconClient rcon_client(g_config.rcon.server_ip.data(), std::stoi(g_config.rcon.server_port.data()),
            g_config.rcon.password.data());
        if (rcon_client.connect())
        {
            std::string response = rcon_client.send_command(command);

            return response;
        }

        return { };
    }

    uint64_t get_available_physical_memory() {
        MEMORYSTATUSEX stat_ex;
        stat_ex.dwLength = sizeof(stat_ex);

        if (GlobalMemoryStatusEx(&stat_ex))
        {
            return stat_ex.ullAvailPhys / 1024 / 1024;
        }
        return {};
    }

    inline namespace process {
        inline bool is_process_running(const std::string& a_process_name)
        {
            const HANDLE h_snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (INVALID_HANDLE_VALUE == h_snapshot) {
                return false;
            }

            PROCESSENTRY32 pe32;
            pe32.dwSize = sizeof(PROCESSENTRY32W);

            if (!Process32First(h_snapshot, &pe32)) {
                CloseHandle(h_snapshot);
                return false;
            }

            std::string process_name_lower = a_process_name;
            std::ranges::transform(process_name_lower, process_name_lower.begin(), ::tolower);

            do {
                std::string current_process_name = pe32.szExeFile;

                std::ranges::transform(current_process_name, current_process_name.begin(), ::tolower);

                if (process_name_lower == current_process_name) {
                    CloseHandle(h_snapshot);
                    return true;
                }
            } while (Process32Next(h_snapshot, &pe32));

            CloseHandle(h_snapshot);
            return false;
        }

        bool launch_process(const fs::path& process_path, const std::string& arguments) {
            if (!fs::exists(process_path) || fs::is_empty(process_path) || fs::is_directory(process_path)) {
                throw std::invalid_argument("The specified process path is invalid or does not exist.");
            }

            STARTUPINFOA si = { 0 };
            PROCESS_INFORMATION pi = { nullptr };
            si.cb = sizeof(si);

            std::string command_line = process_path.string() + " " + arguments;

            const BOOL result = CreateProcessA(
                nullptr,
                command_line.data(),
                nullptr,
                nullptr,
                FALSE,
                0,
                nullptr,
                nullptr,
                &si,
                &pi
            );

            if (!result) {
                const auto error_code = GetLastError();
                throw std::runtime_error("Failed to start process. Error code: " + std::to_string(error_code));
            }

            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);

            return true;
        }
    }
}

namespace PalWorld {
    toml_t load_config(const fs::path& path) {
        if (g_toml_config.empty()) {
            g_toml_config = util::load_config(path);
        }
        return g_toml_config;
    }

    config from_toml_config(const toml_t& toml_config) {
        config config;
        config.damon.server_path = toml_config["damon"]["server_path"].value_or(
            R"("E:/Program Files (x86)/Steam/steamapps/common/PalServer/PalServer.exe")");
        config.damon.server_process_name = toml_config["damon"]["server_process_name"].value_or(R"("PalServer-Win64-Test-Cmd.exe")");
        config.damon.memory_thresholds = toml_config["damon"]["memory_thresholds"].value_or(500);
        config.rcon.server_ip = toml_config["rcon"]["server_ip"].value_or("127.0.0.1");
        config.rcon.server_port = toml_config["rcon"]["server_port"].value_or("25575");
        config.rcon.password = toml_config["rcon"]["password"].value_or("your_admin_password");
        return config;
    }

    inline namespace RCon {
        std::string stop_server(int32_t time, const std::string_view& message) {
            return util::send_command(std::format("Shutdown {} {}", time, message));
        }

        std::string force_stop_server() {
            return util::send_command("DoExit");
        }

        std::string save() {
            return util::send_command("Save");
        }

        std::string broad_cast(const std::string_view& message) {
            return util::send_command(std::format("Broadcast {}", message));
        }

        std::string kick_player(const std::string_view& player_name) {
            return util::send_command(std::format("KickPlayer {}", player_name));
        }

        std::string ban_player(const std::string_view& player_name) {
            return util::send_command(std::format("BanPlayer {}", player_name));
        }

        std::string info() {
            return util::send_command("Info");
        }

        std::string show_players() {
            return util::send_command("ShowPlayers");
        }
    }

    namespace damon {
        bool is_insufficient_memory(uint64_t memory_thresholds) {
            return util::get_available_physical_memory() < memory_thresholds;
        }

        bool is_server_running(const std::string_view& server_process_name) {
            return util::is_process_running(server_process_name.data());
        }

        void start_server(const fs::path& server_path) {
            util::launch_process(server_path, "-useperfthreads -NoAsyncLoadingThread -UseMultithreadForDS");
        }

        void stop_server() {
            save();
            RCon::stop_server(60, "server_will_shutdown_soon!");
            for (int i = 0; i < 5; ++i)
            {
                RCon::broad_cast("server_will_shutdown_in_60_seconds!");
            }
        }
    }
}

std::atomic g_running(true);

BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT) {
        g_running.store(false);
        return TRUE;
    }
    return FALSE;
}

class ServerManager {
public:
    explicit ServerManager(const config& cfg) : cfg_(cfg), running_(true), stop_requested_(false) {}

    void start() {
        if (!SetConsoleCtrlHandler(console_handler, TRUE)) {
            throw std::runtime_error("Error setting up console handler");
        }

        future_ = std::async(std::launch::async, &ServerManager::management_task, this);
    }

    void stop() {
        running_.store(false);

        if (future_.valid()) {
            future_.get();
        }
    }

private:
    void management_task() {
        while (running_.load() && g_running.load()) {
            if (PalWorld::damon::is_server_running(cfg_.damon.server_process_name)) {
                if (PalWorld::damon::is_insufficient_memory(cfg_.damon.memory_thresholds) && !stop_requested_) {
                    PalWorld::damon::stop_server();
                    stop_requested_ = true;
                }
            }
            else {
                stop_requested_ = false;
                PalWorld::damon::start_server(cfg_.damon.server_path);
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    const config& cfg_;
    std::atomic<bool> running_;
    std::atomic<bool> stop_requested_;
    std::future<void> future_;
};

int main(int argc, char** argv) {
    fs::path config_path{ argv[1] };

    if (!fs::exists(config_path) || config_path.empty()) {
        config_path = fs::current_path() / "config.toml";
    }

    try {
        PalWorld::load_config(config_path);
    }
    catch (const toml::parse_error& err) {
        std::cerr << "config parsing failed:\n" << err << "\n";
        return 1;
    }

    if (g_toml_config.empty()) {
        std::cerr << "config file is empty\n";
        return 1;
    }

    g_config = PalWorld::from_toml_config(g_toml_config);
    ServerManager server_manager(g_config);
    try {
        server_manager.start();

        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }

        server_manager.stop();
    }
    catch (const std::exception& e) {
        std::cerr << "Exception occurred: " << e.what() << "\n";
        return 1;
    }

    return 0;
}