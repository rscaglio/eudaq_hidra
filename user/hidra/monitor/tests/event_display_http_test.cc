#include "EventDisplayBuffer.hh"
#include "MonitorHttpServer.hh"

#include <TH1I.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void Check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::shared_ptr<THttpCallArg> Request(MonitorHttpServer& server, const char* path) {
  auto request = std::make_shared<THttpCallArg>();
  request->SetMethod("GET");
  request->SetPathAndFileName(path);
  // SubmitHttp's false return denotes a queued (not immediately completed)
  // asynchronous request. ProcessRequestsWithLock below drains that queue.
  server.SubmitHttp(request);
  return request;
}

nlohmann::json Body(const std::shared_ptr<THttpCallArg>& request) {
  Check(request->IsJson(), "HTTP response is not JSON");
  const auto* data = static_cast<const char*>(request->GetContent());
  return nlohmann::json::parse(data, data + request->GetContentLength());
}
} // namespace

int RunTest() {
  using namespace std::chrono;
  using Clock = EventDisplayBuffer::Clock;
  EventDisplayBuffer display(100, 1);
  HidraEvent seed;
  seed.meta.run_number = 42;
  seed.xdc.ADCvalues.assign(224, 123);
  seed.fers.FERShg.assign(1280, 456);
  seed.fers.FERSlg.assign(1280, 78);
  const auto start = Clock::now();
  for (int i = 0; i < 100; ++i) {
    seed.meta.event_number = i;
    seed.xdc.ADCvalues[1] = i;
    display.Fill(seed, start - seconds(5) + milliseconds(50 * i));
  }
  // No listening socket: exercise the real ROOT request dispatcher in-process.
  MonitorHttpServer server("", &display);
  TH1I count("event_count", "Events", 1, 0, 1);
  count.SetDirectory(nullptr);
  server.Register("/Histograms", &count);
  std::mutex histogram_mutex;
  std::atomic<int> processed{0};
  std::atomic<bool> running{true};
  std::atomic<int> captured{0};
  std::atomic<int64_t> producer_ns{0};
  constexpr int n_events = 60000;
  const auto benchmark_start = Clock::now();
  std::thread producer([&] {
    HidraEvent event = seed;
    for (int i = 0; i < n_events; ++i) {
      event.meta.event_number = 1000 + i;
      event.xdc.ADCvalues[1] = 1000 + i;
      captured += display.Fill(event);
      {
        std::lock_guard<std::mutex> lock(histogram_mutex);
        count.Fill(0.5);
        ++processed;
      }
      std::this_thread::sleep_until(benchmark_start + microseconds(100 * (i + 1)));
    }
    producer_ns = duration_cast<nanoseconds>(Clock::now() - benchmark_start).count();
    running = false;
  });

  // Always join the producer, including on an assertion/JSON failure.
  struct Join {
    std::thread& worker;
    ~Join() { worker.join(); }
  } join{producer};
  bool progressed_during_encoding = false;
  nlohmann::json frozen;
  int responses = 0;
  while (running) {
    const auto request = Request(server, "/EventDisplay/batch.json");
    {
      std::unique_lock<std::mutex> lock(histogram_mutex);
      const int before = processed;
      server.ProcessRequestsWithLock(lock);
      // Still holding the histogram lock here: any increment happened while
      // the server deliberately unlocked it to encode the display response.
      progressed_during_encoding |= processed > before;
      Check(lock.owns_lock(), "HTTP dispatcher did not restore the histogram lock");
    }
    const auto group = Body(request);
    Check(group["events"].size() == 100, "atomic batch lost events during concurrent eviction");
    for (const auto& event : group["events"]) {
      Check(event["adc"][1] == event["event_number"], "batch mixed events during concurrent writes");
    }
    if (frozen.empty()) {
      frozen = group;
    }
    ++responses;
  }
  Check(processed == n_events, "display reads reduced histogram event count");
  Check(progressed_during_encoding, "display JSON encoding blocked histogram filling");
  const auto elapsed = producer_ns.load() * 1e-9;
  Check(captured <= static_cast<int>(elapsed * 20) + 2, "real-time display capture exceeded the 20 Hz cap");
  Check(captured > 0, "display captured no events during the high-rate stream");
  if (captured >= 100) {
    Check(display.EventJson(frozen["events"][0]["id"]).empty(), "stress test did not evict the early backend group");
  }

  // ROOT's normal histogram path must remain usable and complete.
  auto histogram = Request(server, "/Histograms/event_count/root.json");
  {
    std::unique_lock<std::mutex> lock(histogram_mutex);
    server.ProcessRequestsWithLock(lock);
  }
  Check(Body(histogram)["fArray"][1] == n_events, "histogram HTTP endpoint changed");
  Check(std::string(histogram->GetContentType()) == "application/json", "ROOT content type changed");
  display.Reset();
  auto empty = Request(server, "/EventDisplay/batch.json");
  {
    std::unique_lock<std::mutex> lock(histogram_mutex);
    server.ProcessRequestsWithLock(lock);
  }
  Check(Body(empty)["events"].empty(), "new run retained the old backend group");
  Check(frozen["events"].size() == 100, "reset invalidated the downloaded group");
  Check(empty->GetHeader("Cache-Control").Contains("no-cache"), "display response may be cached");
  const auto expired = Request(server, "/EventDisplay/expired.json");
  {
    std::unique_lock<std::mutex> lock(histogram_mutex);
    server.ProcessRequestsWithLock(lock);
    Check(lock.owns_lock(), "404 response did not restore the histogram lock");
  }
  Check(expired->Is404(), "legacy expired-event endpoint no longer returns 404");
  std::cout << "Concurrent ROOT/display test: " << n_events << " histogram events, "
            << n_events / elapsed << " events/s, " << captured << " display copies, "
            << responses << " complete HTTP batches; histogram filling continues during JSON encoding\n";
  return 0;
}

int main() {
  try {
    return RunTest();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
