/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "twinstall/adb_install.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <utility>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/memory.h>
#include <android-base/properties.h>
#include <android-base/scopeguard.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>

#include "fuse_sideload.h"
#include "twinstall/install.h"
#include "twinstall.h"
#include "twinstall/wipe_data.h"
#include "minadbd/types.h"
#include "otautil/sysutil.h"
#include "recovery_ui/device.h"
#include "recovery_ui/ui.h"

// A CommandFunction returns a pair of (result, should_continue), which indicates the command
// execution result and whether it should proceed to the next iteration. The execution result will
// always be sent to the minadbd side.
using CommandFunction = std::function<std::pair<bool, bool>()>;

using namespace std::chrono_literals;

static std::atomic<pid_t> minadbd_pid(0);
static std::atomic<bool> sideload_cancel_requested(false);
static std::atomic<bool> sideload_prepared(false);

void PrepareSideload() {
  sideload_cancel_requested.store(false, std::memory_order_release);
  sideload_prepared.store(true, std::memory_order_release);
}

bool CancelSideload() {
  sideload_cancel_requested.store(true, std::memory_order_release);

  pid_t pid = minadbd_pid.load(std::memory_order_acquire);
  if (pid > 0 && kill(pid, SIGKILL) == -1 && errno != ESRCH) {
    PLOG(ERROR) << "Failed to kill minadbd " << pid;
  }

  // Do not stat FUSE_SIDELOAD_HOST_EXIT_PATHNAME here. A lookup is serviced by
  // the daemon that may be stuck, so using it as the cancellation primitive can
  // deadlock the cancel thread. Detaching is local to the kernel and makes any
  // outstanding package reads fail promptly after minadbd is killed.
  if (umount2(FUSE_SIDELOAD_HOST_MOUNTPOINT, MNT_DETACH) == -1 && errno != EINVAL &&
      errno != ENOENT) {
    PLOG(WARNING) << "Failed to detach sideload FUSE mount";
  }
  return true;
}

static bool SetUsbConfig(const std::string& state, bool cancelable = false) {
  if (!android::base::SetProperty("sys.usb.config", state)) {
    LOG(ERROR) << "Failed to request USB config " << state;
    return false;
  }

  // A broken or incomplete vendor USB init path must not wedge recovery
  // forever. Polling also gives the cancel button a chance to abort setup.
  constexpr int kUsbConfigPollCount = 80;
  for (int i = 0; i < kUsbConfigPollCount; ++i) {
    if (android::base::WaitForProperty("sys.usb.state", state, 250ms)) {
      return true;
    }
    if (cancelable && sideload_cancel_requested.load(std::memory_order_acquire)) {
      return false;
    }
  }
  LOG(ERROR) << "Timed out waiting for USB config " << state;
  return false;
}

static int ReapMinadbd(pid_t pid) {
  pid_t expected = pid;
  minadbd_pid.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);

  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(pid, &status, 0);
  } while (waited == -1 && errno == EINTR);

  if (waited == -1) {
    PLOG(ERROR) << "Failed to wait for minadbd " << pid;
    return -1;
  }
  return status;
}

// Parses the minadbd command in |message|; returns MinadbdCommand::kError upon errors.
static MinadbdCommand ParseMinadbdCommand(const std::string& message) {
  if (!android::base::StartsWith(message, kMinadbdCommandPrefix)) {
    LOG(ERROR) << "Failed to parse command in message " << message;
    return MinadbdCommand::kError;
  }

  auto cmd_code_string = message.substr(strlen(kMinadbdCommandPrefix));
  auto cmd_code = android::base::get_unaligned<uint32_t>(cmd_code_string.c_str());
  if (cmd_code >= static_cast<uint32_t>(MinadbdCommand::kError)) {
    LOG(ERROR) << "Unsupported command code: " << cmd_code;
    return MinadbdCommand::kError;
  }

  return static_cast<MinadbdCommand>(cmd_code);
}

static bool WriteStatusToFd(MinadbdCommandStatus status, int fd) {
  char message[kMinadbdMessageSize];
  memcpy(message, kMinadbdStatusPrefix, strlen(kMinadbdStatusPrefix));
  android::base::put_unaligned(message + strlen(kMinadbdStatusPrefix), status);

  if (!android::base::WriteFully(fd, message, kMinadbdMessageSize)) {
    PLOG(ERROR) << "Failed to write message " << message;
    return false;
  }
  return true;
}

// Installs the package from FUSE. Returns the installation result and whether it should continue
// waiting for new commands.
static auto AdbInstallPackageHandler(int* result, int* wipe_cache) {
  // How long (in seconds) we wait for the package path to be ready. It doesn't need to be too long
  // because the minadbd service has already issued an install command. FUSE_SIDELOAD_HOST_PATHNAME
  // will start to exist once the host connects and starts serving a package. Poll for its
  // appearance. (Note that inotify doesn't work with FUSE.)
  constexpr int ADB_INSTALL_TIMEOUT = 15;
  bool should_continue = true;
  *result = INSTALL_ERROR;
  for (int i = 0; i < ADB_INSTALL_TIMEOUT; ++i) {
    PLOG(ERROR) << "i: " << i;
    struct stat st;
    if (stat(FUSE_SIDELOAD_HOST_PATHNAME, &st) != 0) {
      if (errno == ENOENT && i < ADB_INSTALL_TIMEOUT - 1) {
        sleep(1);
        continue;
      } else {
        should_continue = false;
        break;
      }
    }
    *result = TWinstall_zip(FUSE_SIDELOAD_HOST_PATHNAME, wipe_cache);
    break;
  }

  // Calling stat() on this magic filename signals the FUSE to exit.
  struct stat st;
  stat(FUSE_SIDELOAD_HOST_EXIT_PATHNAME, &st);
  return std::make_pair(*result == INSTALL_SUCCESS, should_continue);
}

static auto AdbRebootHandler(MinadbdCommand command, int* result,
                             Device::BuiltinAction* reboot_action) {
  // Use Device::REBOOT_{FASTBOOT,RECOVERY,RESCUE}, instead of the ones with ENTER_. This allows
  // rebooting back into fastboot/recovery/rescue mode through bootloader, which may use a newly
  // installed bootloader/recovery image.
  switch (command) {
    case MinadbdCommand::kRebootBootloader:
      *reboot_action = Device::REBOOT_BOOTLOADER;
      break;
    case MinadbdCommand::kRebootFastboot:
      *reboot_action = Device::REBOOT_FASTBOOT;
      break;
    case MinadbdCommand::kRebootRecovery:
      *reboot_action = Device::REBOOT_RECOVERY;
      break;
    case MinadbdCommand::kRebootRescue:
      *reboot_action = Device::REBOOT_RESCUE;
      break;
    case MinadbdCommand::kRebootAndroid:
    default:
      *reboot_action = Device::REBOOT;
      break;
  }
  *result = INSTALL_REBOOT;
  return std::make_pair(true, false);
}

// Parses and executes the command from minadbd. Returns whether the caller should keep waiting for
// next command.
static bool HandleMessageFromMinadbd(int socket_fd,
                                     const std::map<MinadbdCommand, CommandFunction>& command_map) {
  char buffer[kMinadbdMessageSize];
  if (!android::base::ReadFully(socket_fd, buffer, kMinadbdMessageSize)) {
    PLOG(ERROR) << "Failed to read message from minadbd";
    return false;
  }

  std::string message(buffer, buffer + kMinadbdMessageSize);
  auto command_type = ParseMinadbdCommand(message);
  if (command_type == MinadbdCommand::kError) {
    return false;
  }
  if (command_map.find(command_type) == command_map.end()) {
    LOG(ERROR) << "Unsupported command: "
               << android::base::get_unaligned<unsigned int>(
                      message.substr(strlen(kMinadbdCommandPrefix)).c_str());
    return false;
  }

  // We have received a valid command, execute the corresponding function.
  const auto& command_func = command_map.at(command_type);
  const auto [result, should_continue] = command_func();
  LOG(INFO) << "Command " << static_cast<uint32_t>(command_type) << " finished with " << result;
  if (!WriteStatusToFd(result ? MinadbdCommandStatus::kSuccess : MinadbdCommandStatus::kFailure,
                       socket_fd)) {
    return false;
  }
  return should_continue;
}

// TODO(xunchang) add a wrapper function and kill the minadbd service there.
static void ListenAndExecuteMinadbdCommands(
    pid_t minadbd_pid, android::base::unique_fd&& socket_fd,
    const std::map<MinadbdCommand, CommandFunction>& command_map) {
  android::base::unique_fd epoll_fd(epoll_create1(O_CLOEXEC));
  if (epoll_fd == -1) {
    PLOG(ERROR) << "Failed to create epoll";
    kill(minadbd_pid, SIGKILL);
    return;
  }

  constexpr int EPOLL_MAX_EVENTS = 10;
  struct epoll_event ev = {};
  ev.events = EPOLLIN | EPOLLHUP;
  ev.data.fd = socket_fd.get();
  struct epoll_event events[EPOLL_MAX_EVENTS];
  if (epoll_ctl(epoll_fd.get(), EPOLL_CTL_ADD, socket_fd.get(), &ev) == -1) {
    PLOG(ERROR) << "Failed to add socket fd to epoll";
    kill(minadbd_pid, SIGKILL);
    return;
  }

  // Set the timeout to be 300s when waiting for minadbd commands.
  constexpr int TIMEOUT_MILLIS = 300 * 1000;
  while (true) {
    // Poll for the status change of the socket_fd, and handle the message if the fd is ready to
    // read.
    int event_count =
        TEMP_FAILURE_RETRY(epoll_wait(epoll_fd.get(), events, EPOLL_MAX_EVENTS, TIMEOUT_MILLIS));
    if (event_count == -1) {
      PLOG(ERROR) << "Failed to wait for epoll events";
      kill(minadbd_pid, SIGKILL);
      return;
    }
    if (event_count == 0) {
      LOG(ERROR) << "Timeout waiting for messages from minadbd";
      kill(minadbd_pid, SIGKILL);
      return;
    }

    for (int n = 0; n < event_count; n++) {
      if (events[n].events & EPOLLHUP) {
        LOG(INFO) << "Socket has been closed";
        kill(minadbd_pid, SIGKILL);
        return;
      }
      if (!HandleMessageFromMinadbd(socket_fd.get(), command_map)) {
        kill(minadbd_pid, SIGKILL);
        return;
      }
    }
  }
}

// Recovery starts minadbd service as a child process, and spawns another thread to listen for the
// message from minadbd through a socket pair. Here is an example to execute one command from adb
// host.
//  a. recovery                    b. listener thread               c. minadbd service
//
//  a1. create socket pair
//  a2. fork minadbd service
//                                                                c3. wait for the adb commands
//                                                                    from host
//                                                                c4. after receiving host commands:
//                                                                  1) set up pre-condition (i.e.
//                                                                     start fuse for adb sideload)
//                                                                  2) issue command through
//                                                                     socket.
//                                                                  3) wait for result
//  a5. start listener thread
//                               b6. listen for message from
//                                   minadbd in a loop.
//                               b7. After receiving a minadbd
//                                   command from socket
//                                 1) execute the command function
//                                 2) send the result back to
//                                    minadbd
//  ......
//                                                                 c8. exit upon receiving the
//                                                                     result
// a9.  wait for listener thread
//      to exit.
//
// a10. wait for minadbd to
//      exit
//                               b11. exit the listening loop
//
static void CreateMinadbdServiceAndExecuteCommands(
    const std::map<MinadbdCommand, CommandFunction>& command_map,
    bool rescue_mode __unused, std::string install_file __unused) {
  signal(SIGPIPE, SIG_IGN);
  auto restore_sigpipe = android::base::make_scope_guard([] { signal(SIGPIPE, SIG_DFL); });

  android::base::unique_fd recovery_socket;
  android::base::unique_fd minadbd_socket;
  if (!android::base::Socketpair(AF_UNIX, SOCK_STREAM, 0, &recovery_socket, &minadbd_socket)) {
    PLOG(ERROR) << "Failed to create socket";
    return;
  }

  pid_t child = fork();
  if (child == -1) {
    PLOG(ERROR) << "Failed to fork child process";
    return;
  }
  if (child == 0) {
    recovery_socket.reset();
    std::vector<std::string> minadbd_commands = {
      "/system/bin/minadbd",
      "--socket_fd",
      std::to_string(minadbd_socket.release()),
    };
    // if (rescue_mode) {
    //   minadbd_commands.push_back("--rescue");
    // }
    auto exec_args = StringVectorToNullTerminatedArray(minadbd_commands);
    execv(exec_args[0], exec_args.data());
    _exit(EXIT_FAILURE);
  }

  minadbd_socket.reset();
  minadbd_pid.store(child, std::memory_order_release);

  if (sideload_cancel_requested.load(std::memory_order_acquire)) {
    kill(child, SIGKILL);
    ReapMinadbd(child);
    return;
  }

  // We need to call SetUsbConfig() after forking minadbd service. Because the function waits for
  // the usb state to be updated, which depends on sys.usb.ffs.ready=1 set in the adb daemon.
  if (!SetUsbConfig("sideload", true)) {
    LOG(ERROR) << "Failed to set usb config to sideload";
    kill(child, SIGKILL);
    ReapMinadbd(child);
    return;
  }

  std::thread listener_thread(ListenAndExecuteMinadbdCommands, child,
                              std::move(recovery_socket), std::ref(command_map));
  if (listener_thread.joinable()) {
    listener_thread.join();
  }

  int status = ReapMinadbd(child);
  if (status == -1) {
    return;
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
    if (WEXITSTATUS(status) == MinadbdErrorCode::kMinadbdAdbVersionError) {
      LOG(ERROR) << "\nYou need adb 1.0.32 or newer to sideload\nto this device.\n";
    } else {
      LOG(ERROR) << "\n(adbd status " << WEXITSTATUS(status) << ")";
    }
  } else if (WIFSIGNALED(status) &&
             !sideload_cancel_requested.load(std::memory_order_acquire)) {
    LOG(ERROR) << "minadbd terminated by signal " << WTERMSIG(status);
  }
}

  int twrp_sideload(const char* install_file, Device::BuiltinAction* reboot_action, int* wipe_cache) {

  // GUI callers prepare before disabling MTP, so a cancellation during that
  // setup window is retained. Non-GUI callers get a fresh state here.
  if (!sideload_prepared.exchange(false, std::memory_order_acq_rel)) {
    sideload_cancel_requested.store(false, std::memory_order_release);
  }
  if (minadbd_pid.load(std::memory_order_acquire) != 0) {
    LOG(ERROR) << "A minadbd sideload process is already active";
    return INSTALL_ERROR;
  }
  if (wipe_cache != nullptr) {
    *wipe_cache = 0;
  }

  // Save the usb state to restore after the sideload operation.
  std::string usb_state = android::base::GetProperty("sys.usb.state", "none");
  // Clean up state and stop adbd.
  if (usb_state != "none" && !SetUsbConfig("none")) {
    LOG(ERROR) << "Failed to clear USB config";
    return INSTALL_ERROR;
  }

  int install_result = INSTALL_ERROR;
  int requested_wipe_cache = 0;
  std::map<MinadbdCommand, CommandFunction> command_map{
  { MinadbdCommand::kInstall,
    std::bind(&AdbInstallPackageHandler, &install_result, &requested_wipe_cache) },
  { MinadbdCommand::kRebootAndroid, std::bind(&AdbRebootHandler, MinadbdCommand::kRebootAndroid,
                                              &install_result, reboot_action) },
  { MinadbdCommand::kRebootBootloader,
    std::bind(&AdbRebootHandler, MinadbdCommand::kRebootBootloader, &install_result,
              reboot_action) },
  { MinadbdCommand::kRebootFastboot, std::bind(&AdbRebootHandler, MinadbdCommand::kRebootFastboot,
                                                &install_result, reboot_action) },
  { MinadbdCommand::kRebootRecovery, std::bind(&AdbRebootHandler, MinadbdCommand::kRebootRecovery,
                                                &install_result, reboot_action) },
  { MinadbdCommand::kRebootRescue,
    std::bind(&AdbRebootHandler, MinadbdCommand::kRebootRescue, &install_result, reboot_action) },
};

  CreateMinadbdServiceAndExecuteCommands(command_map, false, install_file);

  if (wipe_cache != nullptr) {
    *wipe_cache = requested_wipe_cache;
  }

  // Clean up before switching to the older state, for example setting the state
  // to none sets sys/class/android_usb/android0/enable to 0.
  if (!SetUsbConfig("none")) {
    LOG(ERROR) << "Failed to clear USB config";
  }

  if (usb_state != "none") {
    if (!SetUsbConfig(usb_state)) {
      LOG(ERROR) << "Failed to set USB config to " << usb_state;
    }
  }

  return install_result;
}
