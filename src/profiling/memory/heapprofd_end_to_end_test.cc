/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "gtest/gtest.h"
#include "perfetto/base/build_config.h"
#include "src/base/test/test_task_runner.h"
#include "test/test_helper.h"

#include "src/profiling/memory/heapprofd_producer.h"
#include "src/tracing/ipc/default_socket.h"

#include <sys/system_properties.h>

// This test only works when run on Android using an Android Q version of
// Bionic.
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#error "This test can only be used on Android."
#endif

// If we're building on Android and starting the daemons ourselves,
// create the sockets in a world-writable location.
#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
#define TEST_PRODUCER_SOCK_NAME "/data/local/tmp/traced_producer"
#else
#define TEST_PRODUCER_SOCK_NAME ::perfetto::GetProducerSocket()
#endif

namespace perfetto {
namespace profiling {
namespace {

class HeapprofdDelegate : public ThreadDelegate {
 public:
  HeapprofdDelegate(const std::string& producer_socket)
      : producer_socket_(producer_socket) {}
  ~HeapprofdDelegate() override = default;

  void Initialize(base::TaskRunner* task_runner) override {
    producer_.reset(new HeapprofdProducer(task_runner));
    producer_->ConnectWithRetries(producer_socket_.c_str());
  }

 private:
  std::string producer_socket_;
  std::unique_ptr<HeapprofdProducer> producer_;
};

constexpr const char* kEnableHeapprofdProperty = "persist.heapprofd.enable";

int __attribute__((unused)) SetProperty(const char* value) {
  __system_property_set(kEnableHeapprofdProperty, value);
  return 0;
}

TEST(HeapprofdEndToEnd, Smoke) {
  base::TestTaskRunner task_runner;

  TestHelper helper(&task_runner);
  helper.StartServiceIfRequired();

#if PERFETTO_BUILDFLAG(PERFETTO_START_DAEMONS)
  TaskRunnerThread producer_thread("perfetto.prd");
  producer_thread.Start(std::unique_ptr<HeapprofdDelegate>(
      new HeapprofdDelegate(TEST_PRODUCER_SOCK_NAME)));
#else
  base::ignore_result(TEST_PRODUCER_SOCK_NAME);
  std::string prev_property_value = "0";
  const prop_info* pi = __system_property_find(kEnableHeapprofdProperty);
  if (pi) {
    __system_property_read_callback(
        pi,
        [](void* cookie, const char*, const char* value, uint32_t) {
          *reinterpret_cast<std::string*>(cookie) = value;
        },
        &prev_property_value);
  }
  __system_property_set(kEnableHeapprofdProperty, "1");
  base::ScopedResource<const char*, SetProperty, nullptr> unset_property(
      prev_property_value.c_str());
#endif

  helper.ConnectConsumer();
  helper.WaitForConsumerConnect();

  TraceConfig trace_config;
  trace_config.add_buffers()->set_size_kb(10 * 1024);
  trace_config.set_duration_ms(1000);

  pid_t pid = fork();
  switch (pid) {
    case -1:
      PERFETTO_FATAL("Failed to fork.");
    case 0:
      for (;;) {
        // This volatile is needed to prevent the compiler from trying to be
        // helpful and compiling a "useless" malloc + free into a noop.
        volatile char* x = static_cast<char*>(malloc(1024));
        if (x) {
          x[1] = 'x';
          free(const_cast<char*>(x));
        }
      }
    default:
      break;
  }
  auto* ds_config = trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("android.heapprofd");
  ds_config->set_target_buffer(0);

  auto* heapprofd_config = ds_config->mutable_heapprofd_config();
  heapprofd_config->set_sampling_interval_bytes(1);
  *heapprofd_config->add_pid() = static_cast<uint64_t>(pid);
  heapprofd_config->set_all(false);
  heapprofd_config->mutable_continuous_dump_config()->set_dump_phase_ms(0);
  heapprofd_config->mutable_continuous_dump_config()->set_dump_interval_ms(100);

  helper.StartTracing(trace_config);
  helper.WaitForTracingDisabled(5000);

  helper.ReadData();
  helper.WaitForReadData();

  PERFETTO_CHECK(kill(pid, SIGKILL) == 0);

  const auto& packets = helper.trace();
  ASSERT_GT(packets.size(), 0u);
  size_t profile_packets = 0;
  for (const protos::TracePacket& packet : packets) {
    if (packet.has_profile_packet() &&
        packet.profile_packet().process_dumps().size() > 0) {
      profile_packets++;
    }
  }
  EXPECT_GT(profile_packets, 0);
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto