//
// Copyright (c) 2017 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/boostorg/beast
//

//------------------------------------------------------------------------------
//
// Example: HTTP server, small
//
//------------------------------------------------------------------------------
#include "Storage.hpp"
#include "channel_encryption.hpp"
#include "pow.hpp"
#include "utils.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <openssl/sha.h>
#include <sstream>
#include <string>

static const std::string LOKI_EPHEMKEY_HEADER = "X-Loki-EphemKey";

using tcp = boost::asio::ip::tcp;    // from <boost/asio.hpp>
namespace http = boost::beast::http; // from <boost/beast/http.hpp>
namespace pt = boost::property_tree; // from <boost/property_tree/>
using namespace service_node;

using rpc_function = std::function<void(const pt::ptree&)>;

class http_connection : public std::enable_shared_from_this<http_connection> {
  public:
    http_connection(tcp::socket socket, Storage& storage,
                    ChannelEncryption<std::string>& channelEncryption)
        : socket_(std::move(socket)), storage_(storage),
          channelCipher_(channelEncryption) {}

    // Initiate the asynchronous operations associated with the connection.
    void start() {
        assign_callbacks();
        read_request();
        check_deadline();
    }

  private:
    // The socket for the currently connected client.
    tcp::socket socket_;

    // The buffer for performing reads.
    boost::beast::flat_buffer buffer_{8192};

    // The request message.
    http::request<http::string_body> request_;

    // The response message.
    http::response<http::string_body> response_;

    // The timer for putting a deadline on connection processing.
    boost::asio::basic_waitable_timer<std::chrono::steady_clock> deadline_{
        socket_.get_executor().context(), std::chrono::seconds(60)};

    std::map<std::string, std::string> header_;
    std::map<std::string, rpc_function> rpc_endpoints_;
    Storage& storage_;
    ChannelEncryption<std::string>& channelCipher_;
    std::stringstream bodyStream_;

    void assign_callbacks() {
        rpc_endpoints_["store"] = [&](const pt::ptree& params) {
            const auto pubKey = params.get<std::string>("pubKey");
            const auto ttl = params.get<std::string>("ttl");
            const auto nonce = params.get<std::string>("nonce");
            const auto timestamp = params.get<std::string>("timestamp");
            const auto data = params.get<std::string>("data");
            return process_store(pubKey, ttl, nonce, timestamp, data);
        };
        rpc_endpoints_["retrieve"] = [&](const pt::ptree& params) {
            const auto pubKey = params.get<std::string>("pubKey");
            const auto lastHash = params.get("lastHash", "");
            return process_retrieve(pubKey, lastHash);
        };
    }

    // Asynchronously receive a complete request message.
    void read_request() {
        auto self = shared_from_this();

        http::async_read(
            socket_, buffer_, request_,
            [self](boost::beast::error_code ec, std::size_t bytes_transferred) {
                boost::ignore_unused(bytes_transferred);
                if (!ec) {
                    self->process_request();
                    self->write_response();
                }
            });
    }

    template <typename T>
    bool parse_header(T key_list) {
        for (const auto key : key_list) {
            const auto it = request_.find(key);
            if (it == request_.end()) {
                response_.result(http::status::bad_request);
                response_.set(http::field::content_type, "text/plain");
                bodyStream_ << "Missing field in header : " << key;
                BOOST_LOG_TRIVIAL(warning)
                    << "Bad Request. Missing field in header: " << key;
                return false;
            }
            header_[key] = it->value().to_string();
        }
        return true;
    }

    void process_v1() {
        const std::vector<std::string> keys = {LOKI_EPHEMKEY_HEADER};
        if (!parse_header(keys))
            return;
        std::string plainText = request_.body();

        try {
            const std::string decoded =
                boost::beast::detail::base64_decode(plainText);
            plainText =
                channelCipher_.decrypt(decoded, header_[LOKI_EPHEMKEY_HEADER]);
        } catch (const std::exception& e) {
            response_.result(http::status::bad_request);
            response_.set(http::field::content_type, "text/plain");
            bodyStream_ << "Could not decode/decrypt body: ";
            bodyStream_ << e.what();
            BOOST_LOG_TRIVIAL(error) << "Bad Request. Could not decrypt body";
        }

        // parse json
        pt::ptree root;
        std::stringstream ss;
        ss << plainText;
        pt::json_parser::read_json(ss, root);

        const auto method_name = root.get("method", "");
        auto iter = rpc_endpoints_.find(method_name);
        if (iter == rpc_endpoints_.end()) {
            response_.result(http::status::bad_request);
            bodyStream_ << "no method" << method_name;
            BOOST_LOG_TRIVIAL(error)
                << "Bad Request. Unknown method '" << method_name << "'";
            return;
        }
        rpc_function endpoint_callback = iter->second;
        try {
            endpoint_callback(root.get_child("params"));
        } catch (std::exception& e) {
            response_.result(http::status::internal_server_error);
            response_.set(http::field::content_type, "text/plain");
            bodyStream_ << e.what();
            BOOST_LOG_TRIVIAL(error) << "Internal Server Error. Calling "
                                     << method_name << " endpoint failed";
            return;
        }
    }

    void process_retrieve(const std::string& pubKey,
                          const std::string& last_hash) {
        std::vector<storage::Item> items;

        try {
            storage_.retrieve(pubKey, items, last_hash);
        } catch (std::exception e) {
            response_.result(http::status::internal_server_error);
            response_.set(http::field::content_type, "text/plain");
            bodyStream_ << e.what();
            BOOST_LOG_TRIVIAL(error)
                << "Internal Server Error. Could not retrieve messages for "
                << pubKey.substr(0, 2) << "..."
                << pubKey.substr(pubKey.length() - 3, pubKey.length() - 1);
            return;
        }

        pt::ptree root;
        pt::ptree messagesNode;

        for (const auto& item : items) {
            pt::ptree messageNode;
            messageNode.put("hash", item.hash);
            messageNode.put("expiration", item.expirationTimestamp);
            messageNode.put("data", item.bytes);
            messagesNode.push_back(std::make_pair("", messageNode));
        }
        if (messagesNode.size() != 0) {
            root.add_child("messages", messagesNode);
            root.put("lastHash", items.back().hash);
            BOOST_LOG_TRIVIAL(trace)
                << "Successfully retrieved messages for " << pubKey.substr(0, 2)
                << "..."
                << pubKey.substr(pubKey.length() - 3, pubKey.length() - 1);
        }
        std::ostringstream buf;
        pt::write_json(buf, root);
        response_.result(http::status::ok);
        response_.set(http::field::content_type, "application/json");
        bodyStream_ << buf.str();
    }

    void process_store(const std::string& recipient, const std::string& ttl,
                       const std::string& nonce, const std::string& timestamp,
                       const std::string& bytes) {
        uint64_t ttlInt;
        if (!util::parseTTL(ttl, ttlInt)) {
            response_.result(http::status::forbidden);
            response_.set(http::field::content_type, "text/plain");
            bodyStream_ << "Provided TTL is not valid.";
            BOOST_LOG_TRIVIAL(error) << "Forbidden. Invalid TTL " << ttl;
            return;
        }

        // Do not store message if the PoW provided is invalid
        std::string messageHash;
        const bool validPoW =
            checkPoW(nonce, timestamp, ttl, recipient, bytes, messageHash);
        if (!validPoW) {
            response_.result(http::status::forbidden);
            response_.set(http::field::content_type, "text/plain");
            bodyStream_ << "Provided PoW nonce is not valid.";
            BOOST_LOG_TRIVIAL(error)
                << "Forbidden. Invalid PoW nonce " << nonce;
            return;
        }

        bool success;

        try {
            success = storage_.store(messageHash, recipient, bytes, ttlInt);
        } catch (std::exception e) {
            response_.result(http::status::internal_server_error);
            response_.set(http::field::content_type, "text/plain");
            bodyStream_ << e.what();
            BOOST_LOG_TRIVIAL(error)
                << "Internal Server Error. Could not store message for "
                << recipient.substr(0, 2) << "..."
                << recipient.substr(recipient.length() - 3,
                                    recipient.length() - 1);
            return;
        }

        if (!success) {
            response_.result(http::status::conflict);
            response_.set(http::field::content_type, "text/plain");
            bodyStream_ << "hash conflict - resource already present.";
            BOOST_LOG_TRIVIAL(warning) << "Conflict. Message with hash "
                                       << messageHash << " already present";
            return;
        }

        response_.result(http::status::ok);
        response_.set(http::field::content_type, "application/json");
        bodyStream_ << "{ \"status\": \"ok\" }";
        BOOST_LOG_TRIVIAL(trace)
            << "Successfully stored message for " << recipient.substr(0, 2)
            << "..."
            << recipient.substr(recipient.length() - 3, recipient.length() - 1);
        ;
    }

    // Determine what needs to be done with the request message.
    void process_request() {
        response_.version(request_.version());
        response_.keep_alive(false);

        const auto target = request_.target();
        switch (request_.method()) {
        case http::verb::post:
            if (target == "/v1/storage_rpc") {
                process_v1();
                break;
            }
            response_.result(http::status::not_found);
            break;

        default:
            response_.result(http::status::bad_request);
            BOOST_LOG_TRIVIAL(warning)
                << "Bad Request. Unsupported HTTP method used";
            break;
        }
    }

    // Asynchronously transmit the response message.
    void write_response() {
        auto self = shared_from_this();

        std::string body = bodyStream_.str();

        const auto it = header_.find(LOKI_EPHEMKEY_HEADER);
        if (it != header_.end()) {
            const std::string& ephemKey = it->second;
            try {
                body = channelCipher_.encrypt(body, ephemKey);
                body = boost::beast::detail::base64_encode(body);
                response_.set(http::field::content_type, "text/plain");
            } catch (const std::exception& e) {
                response_.result(http::status::internal_server_error);
                response_.set(http::field::content_type, "text/plain");
                body = "Could not encrypt/encode response: ";
                body += e.what();
                BOOST_LOG_TRIVIAL(error)
                    << "Internal Server Error. Could not encrypt response for "
                    << ephemKey.substr(0, 2) << "..."
                    << ephemKey.substr(ephemKey.length() - 3,
                                       ephemKey.length() - 1);
            }
        }

        response_.body() = body;

        response_.set(http::field::content_length, response_.body().size());

        http::async_write(socket_, response_,
                          [self](boost::beast::error_code ec, std::size_t) {
                              self->socket_.shutdown(tcp::socket::shutdown_send,
                                                     ec);
                              self->deadline_.cancel();
                          });
    }

    // Check whether we have spent enough time on this connection.
    void check_deadline() {
        auto self = shared_from_this();

        deadline_.async_wait([self](boost::beast::error_code ec) {
            if (!ec) {
                // Close socket to cancel any outstanding operation.
                self->socket_.close(ec);
            }
        });
    }
};

// "Loop" forever accepting new connections.
void http_server(tcp::acceptor& acceptor, tcp::socket& socket, Storage& storage,
                 ChannelEncryption<std::string>& channelEncryption) {
    acceptor.async_accept(socket, [&](boost::beast::error_code ec) {
        if (!ec)
            std::make_shared<http_connection>(std::move(socket), storage,
                                              channelEncryption)
                ->start();
        http_server(acceptor, socket, storage, channelEncryption);
    });
}
