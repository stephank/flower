// Copyright (c) 2017 Nuxi, https://nuxi.nl/
//
// This file is distributed under a 2-clause BSD license.
// See the LICENSE file for details.

#include <cstring>
#include <memory>
#include <ostream>
#include <streambuf>
#include <thread>

#include <arpc++/arpc++.h>

#include <flower/switchboard/configuration.ad.h>
#include <flower/switchboard/directory.h>
#include <flower/switchboard/handle.h>
#include <flower/switchboard/start.h>
#include <flower/switchboard/target_picker.h>
#include <flower/util/fd_streambuf.h>
#include <flower/util/null_streambuf.h>
#include <flower/util/socket.h>

using arpc::FileDescriptor;
using arpc::Server;
using arpc::ServerBuilder;
using arpc::Status;
using flower::switchboard::Directory;
using flower::switchboard::Handle;
using flower::switchboard::TargetPicker;
using flower::util::AcceptSocketConnection;
using flower::util::fd_streambuf;
using flower::util::null_streambuf;

void flower::switchboard::Start(const Configuration& configuration) {
  // Make logging work.
  std::unique_ptr<std::streambuf> error_streambuf;
  if (const auto& error_log = configuration.error_log(); error_log) {
    error_streambuf = std::make_unique<fd_streambuf>(error_log);
  } else {
    error_streambuf = std::make_unique<null_streambuf>();
  }
  std::ostream error_log(error_streambuf.get());

  // Check that all required fields are present.
  const auto& listening_socket(configuration.listening_socket());
  if (!listening_socket) {
    error_log << "Cannot start without a listening socket" << std::endl;
    return;
  }

  Directory directory;
  TargetPicker target_picker;
  for (;;) {
    // Accept incoming connection to switchboard.
    std::unique_ptr<FileDescriptor> connection;
    if (Status status = AcceptSocketConnection(*listening_socket, &connection,
                                               nullptr, nullptr);
        !status.ok()) {
      error_log << "Failed to accept incoming connection: "
                << status.error_message() << std::endl;
      return;
    }

    // Process incoming RPCs in a separate thread.
    // TODO(ed): Deal with thread creation errors!
    std::thread([
      connection{std::move(connection)}, &directory, &error_log, &target_picker
    ]() mutable {
      ServerBuilder builder(std::move(connection));
      Handle handle(&directory, &target_picker);
      builder.RegisterService(&handle);
      std::shared_ptr<Server> server = builder.Build();
      int error;
      while ((error = server->HandleRequest()) == 0) {
      }
      if (error > 0)
        error_log << "Failed to process incoming request: "
                  << std::strerror(errno) << std::endl;
    }).detach();
  }
}
