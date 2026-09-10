/// @file Socket.h
/// @class Socket
/// @brief A class to implement network socket functionality.
/// @details Used to pass radar data from app to the API.
/// @author 30hours

#ifndef SOCKET_H
#define SOCKET_H

#include <string>
#include <cstdint>
#include <asio.hpp>

class Socket 
{
private:
    /// @brief Owned context must outlive this socket, including at program exit.
    /// Members are destroyed in reverse declaration order.
    asio::io_context io_context;

    /// @brief The ASIO endpoint.
    asio::ip::tcp::endpoint endpoint;

    /// @brief The ASIO socket.
    asio::ip::tcp::socket socket;

public:
    /// @brief Constructor for Socket.
    /// @param ip IP address of data destination.
    /// @param port Port of data destination.
    /// @return The object.
    Socket(const std::string& ip, uint16_t port);

    /// @brief Destructor.
    /// @return Void.
    ~Socket();

    /// @brief Helper function to send data in chunks.
    /// @param data String of complete data to send.
    /// @return Void.
    void sendData(const std::string& data);

};

#endif
