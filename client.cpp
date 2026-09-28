#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <vector>
#include <thread>
#include <atomic>
#include <random>

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "json.hpp"

using namespace std;
namespace fs = std::filesystem;
using json = nlohmann::json;

// ============================================================
// CONFIG
// ============================================================

string g_server_ip = "127.0.0.1";
int    g_server_port = 9000;
int    g_client_threads = 8;   // set by load_config(); used by 'load'

template <typename T>
T require_field(const json& obj, const string& key, const string& path) {
    if (!obj.contains(key)) {
        cerr << "error: missing required field '" << path << "'\n";
        exit(1);
    }
    try {
        return obj.at(key).get<T>();
    } catch (...) {
        cerr << "error: field '" << path << "' has the wrong type\n";
        exit(1);
    }
}

void load_config(const string& path) {
    ifstream in(path);
    if (!in) {
        cerr << "error: cannot open config file '" << path << "'\n";
        exit(1);
    }

    json root;
    try {
        in >> root;
    } catch (const std::exception& e) {
        cerr << "error: malformed JSON in '" << path << "': " << e.what() << "\n";
        exit(1);
    }

    if (!root.contains("server")) {
        cerr << "error: missing required field 'server'\n";
        exit(1);
    }

    const json& server_cfg = root.at("server");
    g_server_ip      = require_field<string>(server_cfg, "ip", "server.ip");
    g_server_port    = require_field<int>(server_cfg, "port", "server.port");

    // Not used by this binary, but validated anyway: the config schema
    // requires every field regardless of which side reads it, so a
    // config missing server_threads should fail the same way it would
    // against the server binary.
    require_field<int>(server_cfg, "server_threads", "server.server_threads");

    g_client_threads = require_field<int>(server_cfg, "client_threads", "server.client_threads");
}

// ============================================================
// NETWORK HELPERS
// ============================================================

bool send_all(int fd, const char* data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = send(fd, data + sent, length - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recv_exact(int fd, char* buffer, size_t length) {
    size_t received = 0;
    while (received < length) {
        ssize_t n = recv(fd, buffer + received, length - received, 0);
        if (n <= 0) return false;
        received += static_cast<size_t>(n);
    }
    return true;
}

bool recv_line(int fd, string& line) {
    line.clear();
    while (true) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return false;
        line.push_back(c);
        if (c == '\n') return true;
    }
}

int connect_to_server() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    // Disable Nagle: avoids ~40ms delayed-ACK stalls on the last partial PUT chunk.
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(g_server_port);
    server_addr.sin_addr.s_addr = inet_addr(g_server_ip.c_str());

    if (connect(fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

// ============================================================
// PUT
// ============================================================

// Returns true on success. `quiet` suppresses chatty stdout
// (used by the load driver, which per A4 reports nothing).
bool put_file(const string& local_path, const string& override_name, bool quiet) {

    ifstream file(local_path, ios::binary);
    if (!file) {
        if (!quiet) cerr << "Could not open file: " << local_path << '\n';
        return false;
    }

    file.seekg(0, ios::end);
    streamoff file_size = file.tellg();
    file.seekg(0, ios::beg);

    string filename = override_name.empty()
        ? fs::path(local_path).filename().string()
        : override_name;

    if (filename.empty()) {
        if (!quiet) cerr << "Invalid filename\n";
        return false;
    }

    int fd = connect_to_server();
    if (fd < 0) {
        if (!quiet) cerr << "Error connecting to server\n";
        return false;
    }

    string request = "PUT " + filename + " " + to_string(file_size) + "\n";

    if (!send_all(fd, request.c_str(), request.size())) {
        close(fd);
        return false;
    }

    string response;
    if (!recv_line(fd, response) || response != "OK 0\n") {
        if (!quiet) cerr << "Server rejected PUT: " << response;
        close(fd);
        return false;
    }

    char buffer[4096];
    while (file) {
        file.read(buffer, sizeof(buffer));
        streamsize bytes_read = file.gcount();
        if (bytes_read > 0) {
            if (!send_all(fd, buffer, static_cast<size_t>(bytes_read))) {
                close(fd);
                return false;
            }
        }
    }

    response.clear();
    bool ok = recv_line(fd, response) && response == "OK 0\n";

    if (!quiet) {
        if (ok) cout << "PUT successful\n";
        else    cerr << "PUT failed: " << response;
    }

    close(fd);
    return ok;
}

// ============================================================
// GET
// ============================================================

// Writes to the current directory under the wire name (A3).
bool get_file(const string& name, bool quiet) {

    int fd = connect_to_server();
    if (fd < 0) {
        if (!quiet) cerr << "Error connecting to server\n";
        return false;
    }

    string request = "GET " + name + "\n";
    if (!send_all(fd, request.c_str(), request.size())) {
        close(fd);
        return false;
    }

    string header;
    if (!recv_line(fd, header)) {
        if (!quiet) cerr << "No response from server\n";
        close(fd);
        return false;
    }

    if (header.rfind("ERR", 0) == 0) {
        if (!quiet) cerr << "Server error: " << header;
        close(fd);
        return false;
    }

    // header is "OK <n>\n"
    size_t n;
    try {
        n = stoull(header.substr(3));
    } catch (...) {
        if (!quiet) cerr << "Malformed header: " << header;
        close(fd);
        return false;
    }

    ofstream out(name, ios::binary | ios::trunc);
    if (!out) {
        if (!quiet) cerr << "Could not open " << name << " for writing\n";
        close(fd);
        return false;
    }

    char buffer[4096];
    size_t remaining = n;
    bool ok = true;

    while (remaining > 0) {
        size_t chunk = min(remaining, sizeof(buffer));
        ssize_t r = recv(fd, buffer, chunk, 0);
        if (r <= 0) { ok = false; break; }
        out.write(buffer, r);
        remaining -= static_cast<size_t>(r);
    }

    out.close();
    close(fd);

    if (!quiet) {
        if (ok) cout << "GET successful (" << n << " bytes)\n";
        else    cerr << "GET failed (connection dropped mid-transfer)\n";
    }

    return ok;
}

// ============================================================
// LOAD
//
// A4: seed every workload file once (uncounted), then run N
// closed-loop requests across `client_threads` threads, each
// picking a workload file uniformly at random and, with equal
// probability, GET-ing or PUT-ing it. The client reports
// nothing; all metrics come from the server's CSV.
// ============================================================

void run_load(const string& workload_dir, long long requests, int client_threads) {

    vector<string> files;
    for (auto& entry : fs::directory_iterator(workload_dir)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path().string());
        }
    }

    if (files.empty()) {
        cerr << "error: no files found in workload directory '"
             << workload_dir << "'\n";
        exit(1);
    }

    // Seed: PUT every workload file once, uncounted.
    for (const auto& f : files) {
        if (!put_file(f, "", /*quiet*/true)) {
            cerr << "warning: seed PUT failed for " << f << "\n";
        }
    }

    atomic<long long> remaining{requests};

    auto worker = [&]() {
        random_device rd;
        mt19937 rng(rd() ^ static_cast<unsigned>(
            hash<thread::id>{}(this_thread::get_id())));
        uniform_int_distribution<size_t> file_pick(0, files.size() - 1);
        uniform_int_distribution<int> op_pick(0, 1);

        while (true) {
            long long r = remaining.fetch_sub(1);
            if (r <= 0) break;

            const string& path = files[file_pick(rng)];
            string base = fs::path(path).filename().string();

            if (op_pick(rng) == 0) {
                get_file(base, /*quiet*/true);
            } else {
                put_file(path, base, /*quiet*/true);
            }
        }
    };

    vector<thread> pool;
    for (int i = 0; i < client_threads; i++) {
        pool.emplace_back(worker);
    }
    for (auto& t : pool) {
        t.join();
    }
}

// ============================================================
// MAIN
// ============================================================

void print_usage_and_exit() {
    cerr <<
        "Usage:\n"
        "  ./client put <local-path> [--config <path>]\n"
        "  ./client get <name>       [--config <path>]\n"
        "  ./client load <dir>       [--config <path>] --requests <N>\n";
    exit(1);
}

int main(int argc, char* argv[]) {

    if (argc < 3) {
        print_usage_and_exit();
    }

    string operation = argv[1];
    string target = argv[2];

    string config_path = "config.json";
    long long requests = -1;
    bool requests_set = false;

    for (int i = 3; i < argc; i++) {
        string arg = argv[i];

        if (arg == "--config") {
            if (i + 1 >= argc) { cerr << "error: --config requires a value\n"; return 1; }
            config_path = argv[++i];
        }
        else if (arg == "--requests") {
            if (i + 1 >= argc) { cerr << "error: --requests requires a value\n"; return 1; }
            try {
                requests = stoll(argv[++i]);
            } catch (...) {
                cerr << "error: --requests must be an integer\n";
                return 1;
            }
            requests_set = true;
        }
        else {
            cerr << "error: unknown flag '" << arg << "'\n";
            print_usage_and_exit();
        }
    }

    if (operation != "put" && operation != "get" && operation != "load") {
        cerr << "error: unknown operation '" << operation << "'\n";
        print_usage_and_exit();
    }

    if (operation == "load" && !requests_set) {
        cerr << "error: --requests is required for 'load'\n";
        return 1;
    }

    if (operation != "load" && requests_set) {
        cerr << "error: --requests is only valid with 'load'\n";
        return 1;
    }

    load_config(config_path);

    if (operation == "put") {
        return put_file(target, "", /*quiet*/false) ? 0 : 1;
    }
    else if (operation == "get") {
        return get_file(target, /*quiet*/false) ? 0 : 1;
    }
    else { // load
        run_load(target, requests, g_client_threads);
        return 0;
    }
}
