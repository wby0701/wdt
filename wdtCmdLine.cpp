/*
 * Copyright 2014 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wdt/Sender.h"
#include "wdt/Receiver.h"
#include <chrono>
#include <future>
#include <folly/String.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <iostream>
#include <signal.h>
#include <thread>

#ifndef FLAGS_INCLUDE_FILE
#define FLAGS_INCLUDE_FILE "WdtFlags.cpp.inc"
#endif

#ifndef ADDITIONAL_SENDER_SETUP
#define ADDITIONAL_SENDER_SETUP
#endif

#define STANDALONE_APP
#include FLAGS_INCLUDE_FILE

// Flags not already in WdtOptions.h/WdtFlags.cpp.inc
DEFINE_bool(run_as_daemon, true,
            "If true, run the receiver as never ending process");

DEFINE_string(directory, ".", "Source/Destination directory");
DEFINE_bool(files, false,
            "If true, read a list of files and optional "
            "filesizes from stdin relative to the directory and transfer then");
DEFINE_string(
    destination, "",
    "empty is server (destination) mode, non empty is destination host");
DEFINE_bool(parse_transfer_log, false,
            "If true, transfer log is parsed and fixed");

DEFINE_string(transfer_id, "", "Transfer id (optional, should match");
DEFINE_int32(
    protocol_version, 0,
    "Protocol version to use, this is used to simulate protocol negotiation");

DECLARE_bool(logtostderr);  // default of standard glog is off - let's set it on

DEFINE_int32(abort_after_seconds, 0,
             "Abort transfer after given seconds. 0 means don't abort.");

using namespace facebook::wdt;
template <typename T>
std::ostream &operator<<(std::ostream &os, const std::set<T> &v) {
  std::copy(v.begin(), v.end(), std::ostream_iterator<T>(os, " "));
  return os;
}

// Example of use of a std::atomic for abort even though in this
// case we could check the time directly (but this is cheaper if more code)
class AbortChecker : public WdtBase::IAbortChecker {
 public:
  explicit AbortChecker(const std::atomic<bool> &abortTrigger)
      : abortTriggerPtr_(&abortTrigger) {
  }
  bool shouldAbort() const {
    return abortTriggerPtr_->load();
  }

 private:
  std::atomic<bool> const *abortTriggerPtr_;
};

std::mutex abortMutex;
std::condition_variable abortCondVar;

void setUpAbort(WdtBase &senderOrReceiver) {
  int abortSeconds = FLAGS_abort_after_seconds;
  LOG(INFO) << "Setting up abort " << abortSeconds << " seconds.";
  if (abortSeconds <= 0) {
    return;
  }
  static std::atomic<bool> abortTrigger{false};
  static AbortChecker chkr(abortTrigger);
  senderOrReceiver.setAbortChecker(&chkr);
  auto lambda = [=] {
    LOG(INFO) << "Will abort in " << abortSeconds << " seconds.";
    std::unique_lock<std::mutex> lk(abortMutex);
    if (abortCondVar.wait_for(lk, std::chrono::seconds(abortSeconds)) ==
        std::cv_status::no_timeout) {
      LOG(INFO) << "Already finished normally, no abort.";
    } else {
      LOG(INFO) << "Requesting abort.";
      abortTrigger.store(true);
    }
  };
  // we want to run in bg, not block
  static std::future<void> abortThread = std::async(std::launch::async, lambda);
}

void cancelAbort() {
  std::unique_lock<std::mutex> lk(abortMutex);
  abortCondVar.notify_one();
}

int main(int argc, char *argv[]) {
  FLAGS_logtostderr = true;
  // Ugliness in gflags' api; to be able to use program name
  google::SetArgv(argc, const_cast<const char **>(argv));
  google::SetVersionString(WDT_VERSION_STR);
  std::string usage("WDT Warp-speed Data Transfer. version ");
  usage.append(google::VersionString());
  usage.append(". Sample usage:\n\t");
  usage.append(google::ProgramInvocationShortName());
  usage.append(" # for a server/receiver\n\t");
  usage.append(google::ProgramInvocationShortName());
  usage.append(" -destination host # for a sender");
  google::SetUsageMessage(usage);
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  signal(SIGPIPE, SIG_IGN);

#define STANDALONE_APP
#define ASSIGN_OPT
#include FLAGS_INCLUDE_FILE  //nolint

  LOG(INFO) << "Starting with directory = " << FLAGS_directory
            << " and destination = " << FLAGS_destination
            << " num sockets = " << FLAGS_num_ports
            << " from port = " << FLAGS_start_port;
  ErrorCode retCode = OK;
  if (FLAGS_parse_transfer_log) {
    TransferLogManager transferLogManager;
    transferLogManager.setRootDir(FLAGS_directory);
    if (!transferLogManager.parseAndPrint()) {
      LOG(ERROR) << "Transfer log parsing failed";
      retCode = ERROR;
    }
  } else if (FLAGS_destination.empty()) {
    Receiver receiver(FLAGS_start_port, FLAGS_num_ports, FLAGS_directory);
    receiver.setTransferId(FLAGS_transfer_id);
    if (FLAGS_protocol_version > 0) {
      receiver.setProtocolVersion(FLAGS_protocol_version);
    }
    int numSuccess = receiver.registerPorts();
    if (numSuccess == 0) {
      LOG(ERROR) << "Couldn't bind on any port";
      return 0;
    }
    setUpAbort(receiver);
    // TODO fix this
    if (!FLAGS_run_as_daemon) {
      receiver.transferAsync();
      std::unique_ptr<TransferReport> report = receiver.finish();
      retCode = report->getSummary().getErrorCode();
    } else {
      receiver.runForever();
      retCode = OK;
    }
  } else {
    std::vector<FileInfo> fileInfo;
    if (FLAGS_files) {
      // Each line should have the filename and optionally
      // the filesize separated by a single space
      std::string line;
      while (std::getline(std::cin, line)) {
        std::vector<std::string> fields;
        folly::split('\t', line, fields, true);
        if (fields.empty() || fields.size() > 2) {
          LOG(FATAL) << "Invalid input in stdin: " << line;
        }
        int64_t filesize =
            fields.size() > 1 ? folly::to<int64_t>(fields[1]) : -1;
        fileInfo.emplace_back(fields[0], filesize);
      }
    }
    std::vector<int32_t> ports;
    const auto &options = WdtOptions::get();
    for (int i = 0; i < options.num_ports; i++) {
      ports.push_back(options.start_port + i);
    }
    Sender sender(FLAGS_destination, FLAGS_directory, ports, fileInfo);
    ADDITIONAL_SENDER_SETUP
    setUpAbort(sender);
    sender.setTransferId(FLAGS_transfer_id);
    if (FLAGS_protocol_version > 0) {
      sender.setProtocolVersion(FLAGS_protocol_version);
    }
    sender.setIncludeRegex(FLAGS_include_regex);
    sender.setExcludeRegex(FLAGS_exclude_regex);
    sender.setPruneDirRegex(FLAGS_prune_dir_regex);
    // TODO fix that
    std::unique_ptr<TransferReport> report = sender.transfer();
    retCode = report->getSummary().getErrorCode();
  }
  cancelAbort();
  return retCode;
}
