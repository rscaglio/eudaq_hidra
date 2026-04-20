#include "Status.hh"
#include "eudaq/RunControl.hh"

class HidraRunControl : public eudaq::RunControl {
public:
  HidraRunControl(const std::string &listenaddress);
  void Configure() override;
  void StartRun() override;
  void StopRun() override;
  void Exec() override;
  static const uint32_t m_id_factory = eudaq::cstr2hash("HidraRunControl");

  void DoStatus(eudaq::ConnectionSPC con, eudaq::StatusSPC st) override;

private:
  uint32_t m_stop_second;
  bool m_flag_running;
  std::chrono::steady_clock::time_point m_tp_start_run;
  std::chrono::steady_clock::time_point m_tp_stop_run;
  std::map<std::string, std::string> module_state;
  std::map<std::string, std::string> last_printed_state;
  std::mutex mtx;
};

namespace {
  auto dummy0 = eudaq::Factory<eudaq::RunControl>::Register<
      HidraRunControl, const std::string &>(HidraRunControl::m_id_factory);
}

HidraRunControl::HidraRunControl(const std::string &listenaddress)
    : RunControl(listenaddress) {
  m_flag_running = false;
}

void HidraRunControl::StartRun() {
  RunControl::StartRun();
  m_tp_start_run = std::chrono::steady_clock::now();
  m_flag_running = true;
}

void HidraRunControl::StopRun() {
  RunControl::StopRun();
  m_tp_stop_run = std::chrono::steady_clock::now();
  m_flag_running = false;
}

void HidraRunControl::Configure() { RunControl::Configure(); }

void HidraRunControl::Exec() {
  StartRunControl();

    static bool stop_sent = false;

    while (IsActiveRunControl()) {

        bool producer_done = false;
        bool collector_ready = false;

        {
            std::lock_guard<std::mutex> lock(mtx);

            // --- To check component states ---
            for (const auto &p : module_state) {
                const std::string &name = p.first;
                const std::string &state = p.second;

                if (last_printed_state[name] != state) {
                    EUDAQ_INFO("[DEVICE]: " + name + " [STATUS]: " + state);
                    last_printed_state[name] = state;
                }
            }
            
	    // --- Check conditions on the producer and collector ---
            for (const auto &p : module_state) {
                const std::string &name = p.first;
                const std::string &state = p.second;

                if (name == "my_pd0" && state == "END_OF_STREAM") {
                    producer_done = true;
                }

                if (name == "my_dc" && state == "STOP_REQUEST") {
                    collector_ready = true;
                }
            }
        }

        // --- When producer has produced max events and collector has collected max events ---> STOP OF THE RUN ---
        if (!stop_sent && producer_done && collector_ready) {
            EUDAQ_INFO("All devices ready → stopping run");
            stop_sent = true;
            StopRun();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

void HidraRunControl::DoStatus(eudaq::ConnectionSPC con, eudaq::StatusSPC st) {
  std::lock_guard<std::mutex> lock(mtx);
  module_state[con->GetName()] = st->GetMessage();
}
