#include "Socket.h"
#include "WriteAll.h"
#include <iostream>

Socket::Socket(const std::string& ip, uint16_t port)
    : endpoint(asio::ip::address::from_string(ip), port), socket(io_context) {
    try {
        socket.connect(endpoint);
    } catch (const std::exception& e) {
        std::cerr << "Error connecting to endpoint: " << e.what() << std::endl;
        throw;
    }
}

Socket::~Socket()
{
}

void Socket::sendData(const std::string& data) {
    vectorwarp::writeAll(socket, data);
}
