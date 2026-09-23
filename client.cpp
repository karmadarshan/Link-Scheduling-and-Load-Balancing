#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

using namespace std;
namespace fs = std::filesystem;

// ------------------------------------------------------------
// Send all bytes
// ------------------------------------------------------------
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

        sent += static_cast<size_t>(n);
    }

    return true;
}

// ------------------------------------------------------------
// Receive one complete line
// ------------------------------------------------------------
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

// ------------------------------------------------------------
// PUT operation
// ------------------------------------------------------------
int put_file(const string& local_path) {

    // --------------------------------------------------------
    // Open local file
    // --------------------------------------------------------
    ifstream file(
        local_path,
        ios::binary
    );

    if (!file) {
        cerr << "Could not open file: "
             << local_path << '\n';
        return 1;
    }

    // --------------------------------------------------------
    // Get file size
    // --------------------------------------------------------
    file.seekg(0, ios::end);

    streamoff file_size =
        file.tellg();

    file.seekg(0, ios::beg);

    // --------------------------------------------------------
    // Get ONLY the base filename
    //
    // Example:
    // text/upload.txt
    //
    // sent over network:
    // upload.txt
    // --------------------------------------------------------
    string filename =
        fs::path(local_path).filename().string();

    if (filename.empty()) {
        cerr << "Invalid filename\n";
        return 1;
    }

    // --------------------------------------------------------
    // Create socket
    // --------------------------------------------------------
    int client_fd = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (client_fd < 0) {
        cerr << "Error creating socket\n";
        return 1;
    }

    // --------------------------------------------------------
    // Server address
    // --------------------------------------------------------
    sockaddr_in server_addr{};

    server_addr.sin_family =
        AF_INET;

    server_addr.sin_port =
        htons(9000);

    server_addr.sin_addr.s_addr =
        inet_addr("127.0.0.1");

    // --------------------------------------------------------
    // Connect
    // --------------------------------------------------------
    if (connect(
            client_fd,
            (struct sockaddr*)&server_addr,
            sizeof(server_addr)) < 0) {

        cerr << "Error connecting to server\n";

        close(client_fd);
        return 1;
    }

    cout << "Connected to server\n";

    // --------------------------------------------------------
    // Send PUT header
    // --------------------------------------------------------
    string request =
        "PUT " +
        filename +
        " " +
        to_string(file_size) +
        "\n";

    cout << "Sending: " << request;

    if (!send_all(
            client_fd,
            request.c_str(),
            request.size())) {

        cerr << "Error sending request\n";

        close(client_fd);
        return 1;
    }

    // --------------------------------------------------------
    // Wait for first OK
    // --------------------------------------------------------
    string response;

    if (!recv_line(
            client_fd,
            response)) {

        cerr << "No response from server\n";

        close(client_fd);
        return 1;
    }

    cout << "Server: " << response;

    if (response != "OK 0\n") {

        cerr << "Server rejected PUT\n";

        close(client_fd);
        return 1;
    }

    // --------------------------------------------------------
    // Send file body
    // --------------------------------------------------------
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

                cerr << "Error sending file\n";

                close(client_fd);
                return 1;
            }
        }
    }

    // --------------------------------------------------------
    // Wait for final OK
    // --------------------------------------------------------
    response.clear();

    if (!recv_line(
            client_fd,
            response)) {

        cerr << "No final response\n";

        close(client_fd);
        return 1;
    }

    cout << "Server: " << response;

    if (response == "OK 0\n") {
        cout << "PUT successful\n";
    }

    close(client_fd);

    return 0;
}

// ------------------------------------------------------------
// Main
// ------------------------------------------------------------
int main(int argc, char* argv[]) {

    // Expected:
    //
    // ./client put <local-path>
    //
    if (argc != 3) {
        cerr << "Usage: ./client put <local-path>\n";
        return 1;
    }

    string operation = argv[1];
    string path = argv[2];

    if (operation != "put") {
        cerr << "Only 'put' is supported right now.\n";
        return 1;
    }

    return put_file(path);
}
