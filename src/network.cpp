#include "detail/network.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <algorithm>
#include <sstream>
#include <cstdlib>
#include <cstddef>
#include <vector>
#include <stdexcept>
#include <string>
#include <memory>

#include <iostream> // temp

#include "request.hpp"
#include "message.hpp"
#include "headers.hpp"
#include "detail/parse.hpp"
#include "detail/encode.hpp"

namespace tal {
namespace detail {

std::unique_ptr<ssl_socket> make_connection(const Request& r,
                                            boost::asio::io_service& ios) {
    boost::asio::ssl::context ssl_context(boost::asio::ssl::context::sslv23);
    ssl_context.set_verify_mode(boost::asio::ssl::verify_peer);
    ssl_context.set_default_verify_paths();  // change this to certs download

    auto socket_ptr = std::make_unique<ssl_socket>(ios, ssl_context);

    boost::asio::ip::tcp::resolver resolver(ios);
    boost::asio::ip::tcp::resolver::query query(r.host, r.HTTP_protocol);
    auto endpoint_iterator = resolver.resolve(query);

    // Make connection, perform tls handshake.
    boost::asio::connect(socket_ptr->lowest_layer(), endpoint_iterator);
    socket_ptr->handshake(ssl_socket::client);
    return socket_ptr;
}

// This should only write to the socket the request, it should return the
// ssl_socket, and the client can use that socket to read a status_line etc..
Message send_HTTP(const Request& request, boost::asio::io_service& ios) {
    auto socket_ptr = make_connection(request, ios);
    // Send request
    boost::asio::streambuf write_buffer;
    std::ostream stream(&write_buffer);
    stream << request;
    boost::asio::write(*socket_ptr, write_buffer);

    // Read Response - throws
    boost::asio::streambuf read_buffer;
    detail::digest(Status_line(*socket_ptr, read_buffer));

    auto header = Headers(*socket_ptr, read_buffer);
    std::string content_length = header.get("content-length");
    std::string message;
    if (!content_length.empty()) {
        auto length = std::stoi(content_length);
        message = detail::read_length(*socket_ptr, read_buffer, length);
    } else if (header.get("transfer-encoding") == "chunked") {
        while (true) {
            std::string chunk{read_chunk(*socket_ptr, read_buffer)};
            if (chunk.empty()) {
                break;
            }
            message.append(chunk);
        }
    }
    if (header.get("content-encoding") == "gzip") {
        detail::decode_gzip(message);
    }

    socket_ptr->lowest_layer().close();
    return Message(message);
}

void digest(const Status_line& status) {
    if (status.status_code != "200") {
        std::stringstream ss;
        ss << "HTTP error. Reason: ";
        ss << status.reason_phrase;
        ss << ". Status Code: ";
        ss << status.status_code;
        throw std::runtime_error(ss.str());
    }
}

}  // namespace detail
}  // namespace tal
