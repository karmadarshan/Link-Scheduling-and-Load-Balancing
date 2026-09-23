#include <iostream>
#include <fstream>
#include <string>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <algorithm>
#include <sys/socket.h>
#include <stdexcept>
#include <vector>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std;

// ============================================================
// REQUEST
// ============================================================

struct Request {
    int client_fd;

    string op;
    string filename;

    size_t bytes;
};

// ============================================================
// GLOBAL REQUEST QUEUE
// ============================================================

deque<Request> request_queue;

mutex queue_mutex;
condition_variable queue_cv;
string scheduler = "fcfs";

// ============================================================
// SEND ALL
// ============================================================

bool send_all(int fd, const char* data, size_t length) {

    size_t sent = 0;

    while (sent < length) {

        ssize_t n = send(
            fd,
            data + sent,
            length - sent,
            0
        );

        if (n <= 0) {
            return false;
        }

        sent += n;
    }

    return true;
}

// ============================================================
// RECEIVE EXACTLY N BYTES
// ============================================================

bool recv_exact(
    int fd,
    char* buffer,
    size_t length
) {

    size_t received = 0;

    while (received < length) {

        ssize_t n = recv(
            fd,
            buffer + received,
            length - received,
            0
        );

        if (n <= 0) {
            return false;
        }

        received += n;
    }

    return true;
}

// ============================================================
// RECEIVE ONE LINE
// ============================================================

bool recv_line(
    int fd,
    string& line
) {

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

// ============================================================
// SEND ERROR
// ============================================================

void send_error(
    int client_fd,
    const string& reason
) {

    string response =
        "ERR " + reason + "\n";

    send_all(
        client_fd,
        response.c_str(),
        response.size()
    );
}

// ============================================================
// GET
// ============================================================

void handle_get(
    int client_fd,
    const string& filename
) {

    string path =
        "files/" + filename;

    ifstream file(
        path,
        ios::binary
    );

    if (!file) {

        send_error(
            client_fd,
            "file not found"
        );

        return;
    }

    file.seekg(0, ios::end);

    streamoff file_size =
        file.tellg();

    file.seekg(0, ios::beg);

    // Send file size first.
    string header =
        "OK " +
        to_string(file_size) +
        "\n";

    if (!send_all(
            client_fd,
            header.c_str(),
            header.size())) {

        return;
    }

    // Send file contents.
    char buffer[4096];

    while (file) {

        file.read(
            buffer,
            sizeof(buffer)
        );

        streamsize bytes_read =
            file.gcount();

        if (bytes_read > 0) {

            if (!send_all(
                    client_fd,
                    buffer,
                    static_cast<size_t>(
                        bytes_read))) {

                return;
            }
        }
    }
}

// ============================================================
// PUT
// ============================================================

void handle_put(
    int client_fd,
    const string& filename,
    size_t file_size
) {

    string path =
        "files/" + filename;

    ofstream file(
        path,
        ios::binary | ios::trunc
    );

    if (!file) {

        send_error(
            client_fd,
            "cannot open file"
        );

        return;
    }

    // Tell client that server is ready.
    string ready = "OK 0\n";

    if (!send_all(
            client_fd,
            ready.c_str(),
            ready.size())) {

        return;
    }

    char buffer[4096];

    size_t remaining =
        file_size;

    while (remaining > 0) {

        size_t chunk =
            min(
                remaining,
                sizeof(buffer)
            );

        ssize_t n = recv(
            client_fd,
            buffer,
            chunk,
            0
        );

        if (n <= 0) {
            cerr << "PUT receive failed\n";
            return;
        }

        file.write(
            buffer,
            n
        );

        if (!file) {
            cerr << "File write failed\n";
            return;
        }

        remaining -=
            static_cast<size_t>(n);
    }

    file.close();

    // Confirm PUT finished.
    string done = "OK 0\n";

    send_all(
        client_fd,
        done.c_str(),
        done.size()
    );
}

// ============================================================
// SERVE REQUEST
// ============================================================

void serve_request(
    const Request& req
) {

    cout << "Worker serving: "
         << req.op
         << " "
         << req.filename
         << " ("
         << req.bytes
         << " bytes)\n";

    if (req.op == "GET") {

        handle_get(
            req.client_fd,
            req.filename
        );

    } else if (req.op == "PUT") {

        handle_put(
            req.client_fd,
            req.filename,
            req.bytes
        );
    }

    close(req.client_fd);
}

// ============================================================
// WORKER THREAD
// ============================================================

void worker_thread() {
    while (true) {
        Request req;

        {
            unique_lock<mutex> lock(queue_mutex);

            queue_cv.wait(lock, [] {
                return !request_queue.empty();
            });

            if (scheduler == "fcfs") {

                // First Come First Serve
                req = request_queue.front();
                request_queue.pop_front();

            }
            else if (scheduler == "sjf") {

                // Shortest Job First
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

            cout << "Queue size after pop: "
                 << request_queue.size() << "\n";
        }

        serve_request(req);
    }
}

// ============================================================
// MAIN
// ============================================================

int main(int argc, char* argv[]){

  string scheduler;

if (argc != 3 || string(argv[1]) != "--sched") {
    cerr << "Usage: ./server --sched <fcfs|sjf|rr|drr>\n";
    return 1;
}

scheduler = argv[2];

if (scheduler != "fcfs" &&
    scheduler != "sjf" &&
    scheduler != "rr" &&
    scheduler != "drr") {

    cerr << "Error: unknown scheduling policy: "
         << scheduler << "\n";

    return 1;
}

cout << "Scheduling policy: "
     << scheduler << "\n";

    // --------------------------------------------------------
    // Create server socket
    // --------------------------------------------------------

    int server_fd =
        socket(
            AF_INET,
            SOCK_STREAM,
            0
        );

    if (server_fd < 0) {

        cerr << "Error creating socket\n";

        return 1;
    }

    // --------------------------------------------------------
    // Allow address reuse
    // --------------------------------------------------------

    int opt = 1;

    setsockopt(
        server_fd,
        SOL_SOCKET,
        SO_REUSEADDR,
        &opt,
        sizeof(opt)
    );

    // --------------------------------------------------------
    // Server address
    // --------------------------------------------------------

    sockaddr_in server_addr{};

    server_addr.sin_family =
        AF_INET;

    server_addr.sin_addr.s_addr =
        inet_addr("127.0.0.1");

    server_addr.sin_port =
        htons(9000);

    // --------------------------------------------------------
    // Bind
    // --------------------------------------------------------

    if (bind(
            server_fd,
            (struct sockaddr*)&server_addr,
            sizeof(server_addr)) < 0) {

        cerr << "Error binding socket\n";

        close(server_fd);

        return 1;
    }

    // --------------------------------------------------------
    // Listen
    // --------------------------------------------------------

    if (listen(
            server_fd,
            10) < 0) {

        cerr << "Error listening\n";

        close(server_fd);

        return 1;
    }

    cout << "Server listening on "
         << "127.0.0.1:9000\n";

    // --------------------------------------------------------
    // Start one worker
    // --------------------------------------------------------

    const int NUM_WORKERS = 4;

vector<thread> workers;

for (int i = 0; i < NUM_WORKERS; i++) {
    workers.emplace_back(worker_thread);
}

for (auto& worker : workers) {
    worker.detach();
}

    // --------------------------------------------------------
    // Accept clients forever
    // --------------------------------------------------------

    while (true) {

        sockaddr_in client_addr{};

        socklen_t client_len =
            sizeof(client_addr);

        int client_fd =
            accept(
                server_fd,
                (struct sockaddr*)&client_addr,
                &client_len
            );

        if (client_fd < 0) {

            cerr << "Error accepting client\n";

            continue;
        }

        cout << "Client connected\n";

        // ----------------------------------------------------
        // IMPORTANT:
        // Read header BEFORE putting request into queue.
        // ----------------------------------------------------

        string request;

        if (!recv_line(
                client_fd,
                request)) {

            cerr << "Could not read request\n";

            close(client_fd);

            continue;
        }

        cout << "Received: "
             << request;

        Request req;

        req.client_fd = client_fd;

        // ----------------------------------------------------
        // GET
        // ----------------------------------------------------

        if (request.rfind("GET ", 0) == 0) {

            req.op = "GET";

            req.filename =
                request.substr(4);

            if (!req.filename.empty() &&
                req.filename.back() == '\n') {

                req.filename.pop_back();
            }

            // We need to know the GET size
            // before queueing.
            string path =
                "files/" + req.filename;

            ifstream file(
                path,
                ios::binary
            );

            if (!file) {

                send_error(
                    client_fd,
                    "file not found"
                );

                close(client_fd);

                continue;
            }

            file.seekg(
                0,
                ios::end
            );

            req.bytes =
                static_cast<size_t>(
                    file.tellg()
                );

            file.close();
        }

        // ----------------------------------------------------
        // PUT
        // ----------------------------------------------------

        else if (
            request.rfind("PUT ", 0) == 0
        ) {

            req.op = "PUT";

            string rest =
                request.substr(4);

            size_t space =
                rest.find(' ');

            if (space == string::npos) {

                send_error(
                    client_fd,
                    "invalid PUT"
                );

                close(client_fd);

                continue;
            }

            req.filename =
                rest.substr(
                    0,
                    space
                );

            string size_string =
                rest.substr(
                    space + 1
                );

            if (!size_string.empty() &&
                size_string.back() == '\n') {

                size_string.pop_back();
            }

            try {

                req.bytes =
                    stoull(
                        size_string
                    );

            } catch (...) {

                send_error(
                    client_fd,
                    "invalid byte count"
                );

                close(client_fd);

                continue;
            }
        }

        // ----------------------------------------------------
        // HEALTH
        // ----------------------------------------------------

        else if (
            request == "HEALTH\n"
        ) {

            // Queue depth right now.
            size_t depth;

            {
                lock_guard<mutex> lock(
                    queue_mutex
                );

                depth =
                    request_queue.size();
            }

            string response =
                "OK " +
                to_string(depth) +
                "\n";

            send_all(
                client_fd,
                response.c_str(),
                response.size()
            );

            close(client_fd);

            continue;
        }

        // ----------------------------------------------------
        // Unknown request
        // ----------------------------------------------------

        else {

            send_error(
                client_fd,
                "unknown request"
            );

            close(client_fd);

            continue;
        }

        // ----------------------------------------------------
        // PUT/GET REQUEST IS NOW ADMITTED.
        //
        // Add it to queue.
        // ----------------------------------------------------

        {
            lock_guard<mutex> lock(
                queue_mutex
            );

            request_queue.push_back(req);

            cout << "Request admitted. "
                 << "Queue size = "
                 << request_queue.size()
                 << "\n";
        }

        // Wake worker.
        queue_cv.notify_one();
    }

    close(server_fd);

    return 0;
}