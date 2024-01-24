#pragma once
#include <iostream>
#include <asio.hpp>
#include <asio/ts/buffer.hpp>
#include <asio/ts/internet.hpp>

constexpr int RCON_PACKET_SIZE = 4096;

class RconClient {
public:
    RconClient(std::string server_ip, uint16_t server_port, std::string password)
        : server_ip_(std::move(server_ip)), server_port_(server_port), password_(std::move(password)), socket_(io_context_) {}

    bool connect() {
        try {
            asio::ip::tcp::resolver resolver(io_context_);
            const asio::ip::tcp::resolver::results_type endpoints = resolver.resolve(server_ip_, std::to_string(server_port_));

            asio::connect(socket_, endpoints);

            if (!send_packet(0, 3, password_)) {
                return false;
            }

            int32_t length, id, type;
            std::string data;
            if (!receive_packet(length, id, type, data) || id == -1) {
                return false;
            }

            return true;
        }
        catch (std::exception& e) {
            std::cerr << "Exception: " << e.what() << "\n";
            return false;
        }
    }

    std::string send_command(const std::string& command) {
        try {
            if (!send_packet(1, 2, command)) {
                throw std::runtime_error("Failed to send command packet.");
            }

            int32_t length, id, type;
            std::string data;
            if (!receive_packet(length, id, type, data)) {
                throw std::runtime_error("Failed to receive command response.");
            }

            return data;
        }
        catch (std::exception& e) {
            std::cerr << "Exception: " << e.what() << "\n";
            return "";
        }
    }

private:
    std::string server_ip_;
    uint16_t server_port_;
    std::string password_;
    asio::io_context io_context_;
    asio::ip::tcp::socket socket_;

    bool send_packet(int32_t id, int32_t type, const std::string& body) {
        const int32_t body_length = static_cast<int32_t>(body.size());
        const int32_t packet_size = body_length + 14;
        std::vector<char> packet(packet_size);

        std::memcpy(packet.data(), &packet_size, sizeof(int32_t));
        std::memcpy(packet.data() + 4, &id, sizeof(int32_t));
        std::memcpy(packet.data() + 8, &type, sizeof(int32_t));
        std::memcpy(packet.data() + 12, body.data(), body_length);
        packet[packet_size - 2] = '\0';
        packet[packet_size - 1] = '\0';

        asio::write(socket_, asio::buffer(packet.data(), packet_size));

        return true;
    }

    bool receive_packet(int32_t& length, int32_t& id, int32_t& type, std::string& data) {
        char packet[RCON_PACKET_SIZE];
        asio::read(socket_, asio::buffer(packet, 4));
        std::memcpy(&length, packet, sizeof(int32_t));

        if (length > RCON_PACKET_SIZE - 4 || length <= 0) {
            return false;
        }

        asio::read(socket_, asio::buffer(packet + 4, length));
        std::memcpy(&id, packet + 4, sizeof(int32_t));
        std::memcpy(&type, packet + 8, sizeof(int32_t));
        data = std::string(packet + 12, length - 10);

        return true;
    }
};

