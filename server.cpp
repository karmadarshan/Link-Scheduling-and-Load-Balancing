#include <iostream>
#include <fstream>
#include <string>
#include <deque>
#include <map>
#include <set>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <vector>
#include <limits>
#include <csignal>
#include <ctime>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/time.h>

#include "json.hpp"

using namespace std;
using json = nlohmann::json;

// ============================================================
// CONFIG / CLI STATE
// ============================================================

string    g_server_ip      = "127.0.0.1";
int       g_server_port    = 9000;
int       g_server_threads = 4;
int       g_client_threads = 8;   // read from config; used by the client, not this binary

long long g_quantum      = -1;    // bytes; -1 means "not set" (fcfs/sjf don't use it)
string    g_file_dir     = "files";
int       g_p            = 1;     // lines batched per write on GET (A8)
string    g_config_path  = "config.json";
string    g_metrics_out  = "metrics.csv";

string    g_scheduler    = "fcfs";

// ============================================================
// REQUEST
//
// Carries everything needed to resume a partially-served
// request across multiple dispatches under rr/drr (A17):
// byte offset (bytes_done), and deficit for drr. The file is
// reopened and seeked to bytes_done each round rather than
// keeping a handle open across dispatches, since a request may
// be picked up by a different worker thread each round.
// ============================================================

struct Request {
    long long id = 0;

    int client_fd = -1;

    string op;        // "GET" or "PUT"
    string filename;

    size_t bytes      = 0; // total size of the transfer
    size_t bytes_done = 0; // bytes transferred so far

    long long deficit = 0; // drr only
    int rounds        = 0;
    size_t forfeited_bytes = 0;

    long long arrival_ns = 0;
    long long start_ns   = 0;
    bool started         = false;

    bool holds_read_lock  = false; // GET: released when the transfer finishes
    bool holds_write_lock = false; // PUT: released when the transfer finishes
};

// ============================================================
// GLOBAL REQUEST QUEUE
// ============================================================

deque<Request> request_queue;

mutex queue_mutex;
condition_variable queue_cv;

// ============================================================
// SHUTDOWN / METRICS STATE
// ============================================================

atomic<bool>      g_shutting_down{false};
atomic<long long> g_active_connections{0};
atomic<long long> g_next_request_id{1};
atomic<long long> g_a14_fired_count{0};
atomic<long long> g_total_get_served{0};
atomic<long long> g_total_put_served{0};

struct CsvRow {
    long long id;
    string op;
    string filename;
    size_t bytes;
    int rounds;
    size_t forfeited_bytes;
    long long arrival_ns;
    long long start_ns;
    long long finish_ns;
};

vector<CsvRow> g_csv_rows;
mutex g_csv_mutex;

int g_server_fd = -1;

// ============================================================
// MONOTONIC CLOCK (A18)
// ============================================================

long long now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000000000LL
         + static_cast<long long>(ts.tv_nsec);
}

// ============================================================
// SEND / RECEIVE HELPERS
// ============================================================

bool send_all(int fd, const char* data, size_t length) {
    size_t sent = 0;

    while (sent < length) {
        ssize_t n = send(fd, data + sent, length - sent, 0);

        if (n <= 0) {
            return false;
        }

        sent += static_cast<size_t>(n);
    }

    return true;
}

bool recv_line(int fd, string& line) {
    line.clear();

    while (true) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);

        if (n <= 0) {
            return false;
        }

        line.push_back(c);

        if (c == '\n') {
            return true;
        }
    }
}

void send_error(int client_fd, const string& reason) {
    string response = "ERR " + reason + "\n";
    send_all(client_fd, response.c_str(), response.size());
}

void set_recv_timeout(int fd, int seconds) {
    timeval tv{};
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// ============================================================
// FILENAME VALIDATION (A25)
// ============================================================

bool is_valid_filename(const string& name) {
    if (name.empty())               return false;
    if (name == ".")                return false;
    if (name == "..")               return false;
    if (name.find('/') != string::npos) return false;
    return true;
}

// ============================================================
// PER-FILENAME READ/WRITE COORDINATION
//
// Multiple concurrent GETs on the same filename are safe, but a
// GET racing a concurrent PUT to the same filename is not: the
// PUT's first round truncates the file before rewriting it, and
// a GET admitted in that window can observe (and declare) a
// stale or zero size. Standard single-writer/multiple-readers
// coordination, held by the *filename* for the request's whole
// lifetime (across every rr/drr round, however many worker
// threads process it) - not by std::mutex ownership, since a
// request can be picked up by a different thread each round.
// ============================================================

mutex file_rw_mutex;
condition_variable file_rw_cv;
map<string, int> active_readers;
set<string> active_writers;

void acquire_read_lock(const string& name) {
    unique_lock<mutex> lock(file_rw_mutex);
    file_rw_cv.wait(lock, [&] {
        return active_writers.find(name) == active_writers.end();
    });
    active_readers[name]++;
}

void release_read_lock(const string& name) {
    {
        lock_guard<mutex> lock(file_rw_mutex);
        if (--active_readers[name] == 0) {
            active_readers.erase(name);
        }
    }
    file_rw_cv.notify_all();
}

void acquire_write_lock(const string& name) {
    unique_lock<mutex> lock(file_rw_mutex);
    file_rw_cv.wait(lock, [&] {
        return active_writers.find(name) == active_writers.end() &&
               active_readers.find(name) == active_readers.end();
    });
    active_writers.insert(name);
}

void release_write_lock(const string& name) {
    {
        lock_guard<mutex> lock(file_rw_mutex);
        active_writers.erase(name);
    }
    file_rw_cv.notify_all();
}

// ============================================================
// ONE GET ROUND
//
// Sends whole lines (A6) starting at req.bytes_done, until the
// next line would exceed `budget`, then stops - never emitting
// a partial line. `allow_overrun` enables the A14 escape hatch
// (rr only): if the very first line of the round alone exceeds
// the whole budget, send it anyway rather than starve forever.
// `count_forfeiture` (rr only) charges the unused remainder of
// the round to req.forfeited_bytes when the round ends via
// preemption (not via completion/EOF).
// ============================================================

struct RoundResult {
    bool finished = false; // true => request is done (success) or aborted (error)
    bool ok       = false; // true iff finished because the transfer completed successfully
    size_t consumed = 0;   // bytes actually transferred this round
};

RoundResult serve_get_round(
    Request& req,
    size_t budget,
    bool allow_overrun,
    bool count_forfeiture
) {
    RoundResult result;

    string path = g_file_dir + "/" + req.filename;

    ifstream file(path, ios::binary);

    if (!file) {
        send_error(req.client_fd, "file not found");
        result.finished = true;
        result.ok = false;
        return result;
    }

    file.seekg(static_cast<streamoff>(req.bytes_done));

    size_t consumed_this_round = 0;
    bool   first_line_of_round = true;

    // --------------------------------------------------------
    // --p batching (A8): gather up to g_p whole lines before
    // issuing a write. Batching changes call granularity only,
    // never which bytes go out or the quantum accounting.
    // --------------------------------------------------------
    string batch;
    int lines_in_batch = 0;

    auto flush_batch = [&]() -> bool {
        if (batch.empty()) return true;
        bool ok = send_all(req.client_fd, batch.data(), batch.size());
        batch.clear();
        lines_in_batch = 0;
        return ok;
    };

    while (req.bytes_done < req.bytes) {

        streampos before = file.tellg();

        string line;
        char c;
        bool got_any = false;

        while (file.get(c)) {
            got_any = true;
            line.push_back(c);
            if (c == '\n') break;
        }

        if (!got_any) {
            // Shouldn't happen if bytes_done tracking is correct,
            // but don't hang if the file changed underneath us.
            break;
        }

        size_t line_len = line.size();
        bool fits = (consumed_this_round + line_len <= budget);

        if (!fits) {
            if (allow_overrun && first_line_of_round) {
                // A14: the line alone is longer than the quantum.
                // Send it in full; the round overruns.
                g_a14_fired_count++;
            } else {
                // Round ends here; rewind so this line is served
                // (in full) at the start of a future round.
                file.seekg(before);
                break;
            }
        }

        batch += line;
        lines_in_batch++;

        consumed_this_round += line_len;
        req.bytes_done += line_len;
        first_line_of_round = false;

        if (lines_in_batch >= g_p) {
            if (!flush_batch()) {
                result.finished = true;
                result.ok = false;
                result.consumed = consumed_this_round;
                return result;
            }
        }
    }

    if (!flush_batch()) {
        result.finished = true;
        result.ok = false;
        result.consumed = consumed_this_round;
        return result;
    }

    result.consumed = consumed_this_round;

    bool done = (req.bytes_done >= req.bytes);

    if (!done && count_forfeiture && budget > consumed_this_round) {
        req.forfeited_bytes += (budget - consumed_this_round);
    }

    result.finished = done;
    result.ok = done;

    return result;
}

// ============================================================
// ONE PUT ROUND
//
// The body is opaque (A9): read exactly min(budget, remaining)
// bytes from the socket and write them to disk. No line
// scanning, no forfeiture (A13).
// ============================================================

RoundResult serve_put_round(Request& req, size_t budget) {

    RoundResult result;

    string path = g_file_dir + "/" + req.filename;

    size_t remaining_total = req.bytes - req.bytes_done;
    size_t to_read = min(budget, remaining_total);

    if (req.bytes_done == 0) {
        // First round: create/truncate the destination file.
        ofstream creator(path, ios::binary | ios::trunc);
        if (!creator) {
            send_error(req.client_fd, "cannot open file");
            result.finished = true;
            result.ok = false;
            return result;
        }
    }

    fstream file(path, ios::binary | ios::in | ios::out);

    if (!file) {
        send_error(req.client_fd, "cannot open file");
        result.finished = true;
        result.ok = false;
        return result;
    }

    file.seekp(static_cast<streamoff>(req.bytes_done));

    char buffer[4096];
    size_t got_total = 0;

    while (got_total < to_read) {

        size_t chunk = min(to_read - got_total, sizeof(buffer));

        ssize_t n = recv(req.client_fd, buffer, chunk, 0);

        if (n <= 0) {
            result.finished = true;
            result.ok = false;
            result.consumed = got_total;
            return result;
        }

        file.write(buffer, n);

        if (!file) {
            result.finished = true;
            result.ok = false;
            result.consumed = got_total;
            return result;
        }

        got_total += static_cast<size_t>(n);
    }

    file.close();

    req.bytes_done += got_total;
    result.consumed = got_total;

    bool done = (req.bytes_done >= req.bytes);
    result.finished = done;
    result.ok = done;

    return result;
}

// ============================================================
// PROCESS ONE DISPATCH OF A REQUEST
//
// fcfs/sjf: budget = SIZE_MAX, so the whole transfer completes
// in a single round (rounds == 1, forfeited_bytes == 0), which
// is exactly their required CSV semantics (A23).
//
// rr: budget = quantum, A14 overrun allowed, forfeiture counted.
// drr: budget = deficit + quantum, no overrun, deficit updated
//      to whatever of the round's allowance went unused (A15).
// ============================================================

void log_completed(const Request& req) {
    lock_guard<mutex> lock(g_csv_mutex);
    g_csv_rows.push_back({
        req.id, req.op, req.filename, req.bytes,
        req.rounds, req.forfeited_bytes,
        req.arrival_ns, req.start_ns, now_ns()
    });
}

void process_request(Request req) {

    if (!req.started) {
        req.started = true;
        req.start_ns = now_ns();
    }

    req.rounds++;

    RoundResult r;

    if (g_scheduler == "fcfs" || g_scheduler == "sjf") {

        size_t budget = numeric_limits<size_t>::max();

        if (req.op == "GET") {
            r = serve_get_round(req, budget, /*overrun*/false, /*forfeit*/false);
        } else {
            r = serve_put_round(req, budget);
        }

    } else if (g_scheduler == "rr") {

        size_t budget = static_cast<size_t>(g_quantum);

        if (req.op == "GET") {
            r = serve_get_round(req, budget, /*overrun*/true, /*forfeit*/true);
        } else {
            r = serve_put_round(req, budget);
        }

    } else { // drr

        size_t budget = static_cast<size_t>(req.deficit) + static_cast<size_t>(g_quantum);

        if (req.op == "GET") {
            r = serve_get_round(req, budget, /*overrun*/false, /*forfeit*/false);
        } else {
            r = serve_put_round(req, budget);
        }

        // Deficit rule (A15): whatever of this round's allowance
        // went unused is retained, unless the request is now done
        // (deficit discarded on exit).
        req.deficit = r.finished ? 0
                                  : static_cast<long long>(budget - r.consumed);
    }

    if (r.finished) {

        if (req.holds_read_lock)  release_read_lock(req.filename);
        if (req.holds_write_lock) release_write_lock(req.filename);

        if (r.ok) {
            if (req.op == "PUT") {
                string done = "OK 0\n";
                send_all(req.client_fd, done.c_str(), done.size());
            }

            log_completed(req);

            if (req.op == "GET") g_total_get_served++;
            else                 g_total_put_served++;
        }
        // r.ok == false: an error/broken-connection path already
        // sent ERR (or the peer vanished); no CSV row, per A23
        // ("one row per completed request").

        close(req.client_fd);
        return;
    }

    // Not finished: requeue at the tail (rr/drr only reach here).
    // Notify: without this, a worker thread already asleep on an
    // empty queue would never learn about this newly-requeued
    // item except via some unrelated future admission, which
    // would silently degrade rr/drr to single-worker throughput
    // under sustained load.
    {
        lock_guard<mutex> lock(queue_mutex);
        request_queue.push_back(req);
    }
    queue_cv.notify_one();
}

// ============================================================
// WORKER THREAD
// ============================================================

void worker_thread() {
    while (true) {

        Request req;
        bool have_request = false;

        {
            unique_lock<mutex> lock(queue_mutex);

            queue_cv.wait(lock, [] {
                return !request_queue.empty() ||
                       (g_shutting_down.load() && g_active_connections.load() == 0);
            });

            if (request_queue.empty()) {
                // Nothing left and we've been told to shut down
                // (A26: drain admitted requests, then stop).
                return;
            }

            if (g_scheduler == "fcfs" || g_scheduler == "rr" || g_scheduler == "drr") {

                // All three drain the queue head; rr/drr just
                // requeue at the tail themselves when unfinished.
                req = request_queue.front();
                request_queue.pop_front();

            } else { // sjf

                auto it = min_element(
                    request_queue.begin(),
                    request_queue.end(),
                    [](const Request& a, const Request& b) {
                        return a.bytes < b.bytes;
                    }
                );

                req = *it;
                request_queue.erase(it);
            }

            have_request = true;
        }

        if (have_request) {
            process_request(req);
        }
    }
}

// ============================================================
// PER-CONNECTION HANDLER
//
// Runs in its own thread, one per accepted connection. Parses
// the request header (with a receive timeout) and, for GET/PUT,
// admits the request into the scheduler queue. This is what
// keeps a stalled/malicious client from blocking admission of
// everyone else (A5) - the accept loop never blocks on header
// parsing.
// ============================================================

void handle_connection(int client_fd) {

    g_active_connections++;

    set_recv_timeout(client_fd, 10);

    string request;

    if (!recv_line(client_fd, request)) {
        close(client_fd);
        g_active_connections--;
        queue_cv.notify_all();
        return;
    }

    Request req;
    req.client_fd = client_fd;

    // ----------------------------------------------------
    // GET
    // ----------------------------------------------------
    if (request.rfind("GET ", 0) == 0) {

        req.op = "GET";
        req.filename = request.substr(4);

        if (!req.filename.empty() && req.filename.back() == '\n') {
            req.filename.pop_back();
        }

        if (!is_valid_filename(req.filename)) {
            send_error(client_fd, "invalid filename");
            close(client_fd);
            g_active_connections--;
            queue_cv.notify_all();
            return;
        }

        string path = g_file_dir + "/" + req.filename;

        // Held from here until the whole transfer completes
        // (process_request releases it), so a concurrent PUT to
        // this filename can't truncate/rewrite it mid-GET.
        acquire_read_lock(req.filename);

        ifstream file(path, ios::binary);

        if (!file) {
            release_read_lock(req.filename);
            send_error(client_fd, "file not found");
            close(client_fd);
            g_active_connections--;
            queue_cv.notify_all();
            return;
        }

        file.seekg(0, ios::end);
        req.bytes = static_cast<size_t>(file.tellg());
        file.close();

        req.holds_read_lock = true;

        // OK <n>\n header is sent once, here at admission time,
        // ahead of the (possibly multi-round) body.
        string header = "OK " + to_string(req.bytes) + "\n";
        if (!send_all(client_fd, header.c_str(), header.size())) {
            release_read_lock(req.filename);
            close(client_fd);
            g_active_connections--;
            queue_cv.notify_all();
            return;
        }
    }

    // ----------------------------------------------------
    // PUT
    // ----------------------------------------------------
    else if (request.rfind("PUT ", 0) == 0) {

        req.op = "PUT";

        string rest = request.substr(4);
        size_t space = rest.find(' ');

        if (space == string::npos) {
            send_error(client_fd, "invalid PUT");
            close(client_fd);
            g_active_connections--;
            queue_cv.notify_all();
            return;
        }

        req.filename = rest.substr(0, space);

        string size_string = rest.substr(space + 1);
        if (!size_string.empty() && size_string.back() == '\n') {
            size_string.pop_back();
        }

        if (!is_valid_filename(req.filename)) {
            send_error(client_fd, "invalid filename");
            close(client_fd);
            g_active_connections--;
            queue_cv.notify_all();
            return;
        }

        try {
            req.bytes = stoull(size_string);
        } catch (...) {
            send_error(client_fd, "invalid byte count");
            close(client_fd);
            g_active_connections--;
            queue_cv.notify_all();
            return;
        }

        // Held from here until the whole transfer completes
        // (process_request releases it): blocks any concurrent
        // GET/PUT on this filename until this write is done.
        acquire_write_lock(req.filename);
        req.holds_write_lock = true;

        // First OK (readiness to receive body) is sent at
        // admission, matching the documented PUT exchange.
        string ready = "OK 0\n";
        if (!send_all(client_fd, ready.c_str(), ready.size())) {
            release_write_lock(req.filename);
            close(client_fd);
            g_active_connections--;
            queue_cv.notify_all();
            return;
        }
    }

    // ----------------------------------------------------
    // HEALTH - answered immediately, out of band (A5, B8)
    // ----------------------------------------------------
    else if (request == "HEALTH\n") {

        size_t depth;
        {
            lock_guard<mutex> lock(queue_mutex);
            depth = request_queue.size();
        }

        string response = "OK " + to_string(depth) + "\n";
        send_all(client_fd, response.c_str(), response.size());
        close(client_fd);
        g_active_connections--;
        queue_cv.notify_all();
        return;
    }

    // ----------------------------------------------------
    // Unknown request
    // ----------------------------------------------------
    else {
        send_error(client_fd, "unknown request");
        close(client_fd);
        g_active_connections--;
        queue_cv.notify_all();
        return;
    }

    // ----------------------------------------------------
    // GET/PUT request is now admitted (A5: header parsed
    // before enqueueing, so its size is known up front).
    // ----------------------------------------------------

    req.id = g_next_request_id++;
    req.arrival_ns = now_ns();

    {
        lock_guard<mutex> lock(queue_mutex);
        request_queue.push_back(req);
    }

    queue_cv.notify_one();

    g_active_connections--;
    queue_cv.notify_all(); // in case shutdown is waiting on active_connections
}

// ============================================================
// CONFIG LOADING
// ============================================================

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

void load_config(const string& config_path) {

    ifstream in(config_path);

    if (!in) {
        cerr << "error: cannot open config file '" << config_path << "'\n";
        exit(1);
    }

    json root;

    try {
        in >> root;
    } catch (const std::exception& e) {
        cerr << "error: malformed JSON in '" << config_path << "': " << e.what() << "\n";
        exit(1);
    }

    if (!root.contains("server")) {
        cerr << "error: missing required field 'server'\n";
        exit(1);
    }

    const json& server_cfg = root.at("server");

    g_server_ip      = require_field<string>(server_cfg, "ip", "server.ip");
    g_server_port    = require_field<int>(server_cfg, "port", "server.port");
    g_server_threads = require_field<int>(server_cfg, "server_threads", "server.server_threads");
    g_client_threads = require_field<int>(server_cfg, "client_threads", "server.client_threads");
}

// ============================================================
// CLI ARGUMENT PARSING
// ============================================================

void print_usage_and_exit() {
    cerr <<
        "Usage: ./server --sched <fcfs|sjf|rr|drr> --file <dir>\n"
        "                 [--quantum <bytes>] [--p <N>]\n"
        "                 [--config <path>] [--metrics-out <path>]\n";
    exit(1);
}

void parse_args(int argc, char* argv[]) {

    bool sched_set = false;
    bool file_set  = false;
    bool quantum_set = false;

    for (int i = 1; i < argc; i++) {

        string arg = argv[i];

        auto next_value = [&](const string& flag) -> string {
            if (i + 1 >= argc) {
                cerr << "error: " << flag << " requires a value\n";
                exit(1);
            }
            return argv[++i];
        };

        if (arg == "--sched") {
            g_scheduler = next_value(arg);
            sched_set = true;
        }
        else if (arg == "--quantum") {
            try {
                g_quantum = stoll(next_value(arg));
            } catch (...) {
                cerr << "error: --quantum must be an integer\n";
                exit(1);
            }
            quantum_set = true;
        }
        else if (arg == "--file") {
            g_file_dir = next_value(arg);
            file_set = true;
        }
        else if (arg == "--p") {
            try {
                g_p = stoi(next_value(arg));
            } catch (...) {
                cerr << "error: --p must be an integer\n";
                exit(1);
            }
        }
        else if (arg == "--config") {
            g_config_path = next_value(arg);
        }
        else if (arg == "--metrics-out") {
            g_metrics_out = next_value(arg);
        }
        else {
            cerr << "error: unknown flag '" << arg << "'\n";
            print_usage_and_exit();
        }
    }

    if (!sched_set) {
        cerr << "error: --sched is required\n";
        print_usage_and_exit();
    }

    if (g_scheduler != "fcfs" && g_scheduler != "sjf" &&
        g_scheduler != "rr"   && g_scheduler != "drr") {
        cerr << "error: unknown scheduling policy: " << g_scheduler << "\n";
        exit(1);
    }

    if (!file_set) {
        cerr << "error: --file is required\n";
        print_usage_and_exit();
    }

    bool needs_quantum = (g_scheduler == "rr" || g_scheduler == "drr");

    if (needs_quantum && !quantum_set) {
        cerr << "error: --quantum is required for --sched " << g_scheduler << "\n";
        exit(1);
    }

    if (!needs_quantum && quantum_set) {
        cerr << "error: --quantum is not valid with --sched " << g_scheduler << "\n";
        exit(1);
    }

    if (needs_quantum && g_quantum <= 0) {
        cerr << "error: --quantum must be a positive integer\n";
        exit(1);
    }

    if (g_p < 1) {
        cerr << "error: --p must be >= 1\n";
        exit(1);
    }
}

// ============================================================
// METRICS CSV (A23)
// ============================================================

void write_metrics_csv() {
    ofstream out(g_metrics_out);

    if (!out) {
        cerr << "warning: could not open " << g_metrics_out << " for writing metrics\n";
        return;
    }

    out << "request_id,op,filename,bytes,rounds,forfeited_bytes,"
           "arrival_ns,start_ns,finish_ns\n";

    lock_guard<mutex> lock(g_csv_mutex);

    for (const auto& row : g_csv_rows) {
        out << row.id << ","
            << row.op << ","
            << row.filename << ","
            << row.bytes << ","
            << row.rounds << ","
            << row.forfeited_bytes << ","
            << row.arrival_ns << ","
            << row.start_ns << ","
            << row.finish_ns << "\n";
    }
}

void print_aggregate_summary() {
    cout << "\n=== Shutdown summary ===\n";
    cout << "Scheduler:        " << g_scheduler << "\n";
    cout << "GET requests:     " << g_total_get_served.load() << "\n";
    cout << "PUT requests:     " << g_total_put_served.load() << "\n";
    cout << "A14 overruns:     " << g_a14_fired_count.load() << "\n";
    cout << "Metrics written:  " << g_metrics_out << "\n";
    cout << "========================\n";
}

// ============================================================
// MAIN
// ============================================================

int main(int argc, char* argv[]) {

    parse_args(argc, argv);
    load_config(g_config_path);

    cout << "Scheduling policy: " << g_scheduler << "\n";
    if (g_quantum > 0) {
        cout << "Quantum: " << g_quantum << " bytes\n";
    }
    cout << "Serving directory: " << g_file_dir << "\n";

    // --------------------------------------------------------
    // Block SIGINT/SIGTERM in this thread (and therefore all
    // threads spawned after this point); a dedicated thread
    // below catches them synchronously via sigwait, which is
    // the safe way to do graceful shutdown from multiple
    // worker threads (A26).
    // --------------------------------------------------------

    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigset, nullptr);

    // --------------------------------------------------------
    // Create server socket
    // --------------------------------------------------------

    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (g_server_fd < 0) {
        cerr << "Error creating socket\n";
        return 1;
    }

    int opt = 1;
    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(g_server_ip.c_str());
    server_addr.sin_port = htons(g_server_port);

    if (bind(g_server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        cerr << "Error binding socket\n";
        close(g_server_fd);
        return 1;
    }

    if (listen(g_server_fd, 128) < 0) {
        cerr << "Error listening\n";
        close(g_server_fd);
        return 1;
    }

    cout << "Server listening on " << g_server_ip << ":" << g_server_port << "\n";

    // --------------------------------------------------------
    // Signal-handling thread (A26)
    // --------------------------------------------------------

    thread signal_thread([&]() {
        int sig = 0;
        sigwait(&sigset, &sig);

        cout << "\nReceived shutdown signal, draining...\n";
        g_shutting_down = true;

        // Force accept() in main to unblock.
        shutdown(g_server_fd, SHUT_RDWR);

        queue_cv.notify_all();
    });

    // --------------------------------------------------------
    // Start worker pool
    // --------------------------------------------------------

    vector<thread> workers;
    for (int i = 0; i < g_server_threads; i++) {
        workers.emplace_back(worker_thread);
    }

    // --------------------------------------------------------
    // Accept clients until shutdown
    // --------------------------------------------------------

    while (!g_shutting_down.load()) {

        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(g_server_fd, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            if (g_shutting_down.load()) break;
            cerr << "Error accepting client\n";
            continue;
        }

        // Connections accepted moments before the signal must
        // still be answered - handle_connection runs regardless
        // of g_shutting_down and will still admit into the
        // queue; workers drain the queue before exiting.
        thread(handle_connection, client_fd).detach();
    }

    // --------------------------------------------------------
    // Drain: wait for any header-parsing threads already in
    // flight to finish admitting (or rejecting) their request,
    // so the queue can't grow after workers decide it's empty.
    // --------------------------------------------------------

    while (g_active_connections.load() > 0) {
        this_thread::sleep_for(chrono::milliseconds(20));
    }

    queue_cv.notify_all();

    for (auto& w : workers) {
        w.join();
    }

    close(g_server_fd);

    print_aggregate_summary();
    write_metrics_csv();

    signal_thread.join();

    return 0;
}