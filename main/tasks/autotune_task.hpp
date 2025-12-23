#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <vector>
#include <string>
#include <atomic>
#include <mutex>

struct AutotuneConfig {
    uint16_t max_frequency = 0;   // 0 = Auto (Board Max)
    uint16_t max_voltage = 0;     // 0 = Auto (Board Max)
    uint16_t max_power = 0;       // 0 = Auto (Board Max)
    uint16_t max_temp = 0;        // 0 = Auto (Config Overheat Temp)
};

struct AutotuneResult {
    uint16_t frequency = 0;
    uint16_t voltage = 0;
    float hashrate_ghs = 0;
    float power_w = 0;
    float efficiency_j_th = 0;
    float temp_c = 0;
    bool valid = false;
};

enum class AutotuneState {
    IDLE,
    INIT,
    SCAN_FREQ_INIT,
    SCAN_FREQ_VOLT_SWEEP,
    WAIT_STABLE,
    MEASURE,
    ANALYZE,
    FINISHED,
    ERROR_STOPPED
};

class AutotuneTask {
public:
    static AutotuneTask& getInstance() {
        static AutotuneTask instance;
        return instance;
    }

    // Public API
    bool start(const AutotuneConfig& config);
    void stop();
    bool isRunning() const { return m_state != AutotuneState::IDLE && m_state != AutotuneState::FINISHED && m_state != AutotuneState::ERROR_STOPPED; }
    
    // Status getters
    AutotuneState getState() const { return m_state; }
    AutotuneConfig getConfig() const { return m_config; }
    AutotuneResult getBestHashrate() const { return m_best_hashrate; }
    AutotuneResult getBestEfficiency() const { return m_best_efficiency; }
    std::vector<std::string> getLogLines() const; // Returns recent log lines
    
    // Current progress
    uint16_t getCurrentFreq() const { return m_current_freq; }
    uint16_t getCurrentVolt() const { return m_current_volt; }
    int getCurrentStep() const { return m_step_counter; }
    int getTotalSteps() const { return m_total_estimated_steps; }
    
    // Debug logging (public for handler access)
    void log(const char* format, ...);

private:
    AutotuneTask();
    ~AutotuneTask() = default;
    AutotuneTask(const AutotuneTask&) = delete;
    AutotuneTask& operator=(const AutotuneTask&) = delete;

    static void taskFunction(void* pvParameters);
    void run();

    // Strategy helpers
    bool applySettings(uint16_t freq, uint16_t volt);
    bool checkLimits(); // Returns true if OK, false if limit exceeded

    TaskHandle_t m_taskHandle = nullptr;
    std::atomic<AutotuneState> m_state{AutotuneState::IDLE};
    std::atomic<bool> m_stop_requested{false};
    
    AutotuneConfig m_config;
    
    // Candidates
    AutotuneResult m_best_hashrate;
    AutotuneResult m_best_efficiency;
    AutotuneResult m_current_attempt;

    // Internal State
    uint16_t m_current_freq = 0;
    uint16_t m_current_volt = 0;
    uint16_t m_start_volt = 1150;
    
    int m_val_sweep_step = 0; // -2 to +2
    int m_step_counter = 0;
    int m_total_estimated_steps = 0;
    
    std::vector<std::string> m_log_buffer;
    mutable std::mutex m_log_mutex;
    
    // Timing constants (ms)
    // Using conservative values from python script but can be tuned
    static constexpr uint32_t SETTLE_TIME_MS = 30000; // 30s (reduced from 90s for testing)
    static constexpr uint32_t MEASURE_TIME_MS = 30000; // 30s (reduced from 60s)
    static constexpr uint32_t POLLING_INTERVAL_MS = 2000; // Check sensors every 2s
};
