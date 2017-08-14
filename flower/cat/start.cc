// Copyright (c) 2017 Nuxi, https://nuxi.nl/
//
// This file is distributed under a 2-clause BSD license.
// See the LICENSE file for details.

#include <sys/socket.h>

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <arpc++/arpc++.h>

#include <flower/cat/configuration.ad.h>
#include <flower/cat/start.h>
#include <flower/protocol/switchboard.ad.h>
#include <flower/util/fd_streambuf.h>
#include <flower/util/label_map.h>
#include <flower/util/logger.h>
#include <flower/util/null_streambuf.h>

using arpc::Channel;
using arpc::ClientContext;
using arpc::FileDescriptor;
using arpc::Status;
using flower::protocol::switchboard::Switchboard;
using flower::protocol::switchboard::ClientConnectRequest;
using flower::protocol::switchboard::ClientConnectResponse;
using flower::util::LabelMapToJson;
using flower::util::LogTransaction;
using flower::util::Logger;
using flower::util::fd_streambuf;
using flower::util::null_streambuf;

namespace {

void TransferUnidirectionally(FileDescriptor* input, FileDescriptor* output,
                              size_t buffer_size) {
  std::string buffer;
  for (;;) {
    // Call into poll() to wait for reading/writing on the file descriptors.
    bool work_remaining = false;
    struct pollfd pfds[2] = {};
    if (input != nullptr && buffer.size() < buffer_size) {
      work_remaining = true;
      pfds[0].fd = input->get();
      pfds[0].events = POLLIN;
    } else {
      pfds[0].fd = -1;
    }
    if (output != nullptr && buffer.size() > 0) {
      work_remaining = true;
      pfds[1].fd = output->get();
      pfds[1].events = POLLOUT;
    } else {
      pfds[1].fd = -1;
    }
    if (!work_remaining)
      break;
    if (poll(pfds, 2, -1) == -1) {
      // TODO(ed): Log error!
      break;
    }

    // Read data from the input file descriptor.
    if (pfds[0].revents != 0) {
      size_t offset = buffer.size();
      buffer.resize(buffer_size);
      ssize_t read_length =
          read(input->get(), &buffer[offset], buffer_size - offset);
      if (read_length > 0) {
        buffer.resize(offset + read_length);
      } else {
        // TODO(ed): Log error!
        buffer.resize(offset);
        shutdown(input->get(), SHUT_RD);
        input = nullptr;
      }
    }

    // Write data to the output file descriptor.
    if (pfds[1].revents != 0) {
      ssize_t write_length = write(output->get(), buffer.data(), buffer.size());
      if (write_length >= 0) {
        buffer.erase(0, write_length);
      } else {
        // TODO(ed): Log error!
        shutdown(output->get(), SHUT_WR);
        output = nullptr;
      }
    }
  }

  // Shut down file descriptors if not done so already.
  if (input != nullptr)
    shutdown(input->get(), SHUT_RD);
  if (output != nullptr)
    shutdown(output->get(), SHUT_WR);
}

}  // namespace

void flower::cat::Start(const Configuration& configuration) {
  // Make logging work.
  std::unique_ptr<std::streambuf> logger_streambuf;
  if (const auto& logger_output = configuration.logger_output();
      logger_output) {
    logger_streambuf = std::make_unique<fd_streambuf>(logger_output);
  } else {
    logger_streambuf = std::make_unique<null_streambuf>();
  }
  Logger logger(logger_streambuf.get());

  // Check that all required fields are present.
  const auto& switchboard_socket = configuration.switchboard_socket();
  if (!switchboard_socket) {
    logger.Log() << "Cannot start without a switchboard socket";
    std::exit(1);
  }
  size_t buffer_size = configuration.buffer_size();
  if (buffer_size < 1) {
    logger.Log() << "Buffer size not configured";
    std::exit(1);
  }

  // Establish a connection through the switchboard.
  std::shared_ptr<FileDescriptor> connection;
  if (configuration.listen()) {
    // TODO(ed): Implement!
  } else {
    std::shared_ptr<Channel> channel = CreateChannel(switchboard_socket);
    std::unique_ptr<Switchboard::Stub> stub = Switchboard::NewStub(channel);
    ClientContext context;
    ClientConnectRequest request;
    ClientConnectResponse response;
    if (Status status = stub->ClientConnect(&context, request, &response);
        !status.ok()) {
      logger.Log() << status.error_message();
      std::exit(1);
    }
    LogTransaction log_transaction = logger.Log();
    log_transaction << "Established connection with server ";
    LabelMapToJson(response.connection_labels(), &log_transaction);
    connection = response.server();
  }
  if (!connection) {
    logger.Log() << "Switchboard did not return a connection";
    std::exit(1);
  }

  // Run two threads to copy data on the descriptors in both directions.
  std::thread thread_input(TransferUnidirectionally,
                           configuration.stream_input().get(), connection.get(),
                           buffer_size);
  TransferUnidirectionally(connection.get(),
                           configuration.stream_output().get(), buffer_size);
  thread_input.join();
  std::exit(0);
}