// Test-only RTL-SDR ABI double. No libusb dependency and no hardware access.
// Five streams share deterministic broadband samples with fixed phase/gain errors.
#include <rtl-sdr.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using Clock = std::chrono::steady_clock;
struct rtlsdr_dev {
    unsigned channel;
    std::atomic<bool> cancelled{false}, reading{false}, closed{false}, noise{false};
    std::atomic<unsigned> frequency{100000000}, rate{2400000};
    std::atomic<int> gain{496};
    std::atomic<int> correction{0};
    std::atomic<unsigned> stale_noise_remaining{0};
    std::atomic<long long> stale_noise_ready_ns{0};
};
namespace {
std::mutex event_mutex, round_mutex;
std::condition_variable round_cv;
std::vector<std::unique_ptr<rtlsdr_dev>> devices;
unsigned round_arrived = 0;
uint64_t round_id = 0;
Clock::time_point round_time = Clock::now();

std::string root() {
    const char* value = std::getenv("VECTORWARP_KRAKEN_SIM_ROOT");
    if (!value || !*value) std::_Exit(98);  // Cannot run accidentally as a real driver.
    return value;
}
void event(const char* name, int channel = -1, long long value = 0) {
    std::lock_guard<std::mutex> lock(event_mutex);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count();
    const std::string path = root() + "/driver.jsonl";
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) std::_Exit(97);
    char line[256];
    const int n = std::snprintf(line, sizeof(line),
        "{\"event\":\"%s\",\"channel\":%d,\"value\":%lld,\"timeMs\":%lld}\n",
        name, channel, value, static_cast<long long>(ms));
    if (write(fd, line, static_cast<size_t>(n)) != n) std::_Exit(96);
    close(fd);
}
unsigned count() {
    const char* value = std::getenv("VECTORWARP_KRAKEN_SIM_COUNT");
    return value ? static_cast<unsigned>(std::atoi(value)) : 5;
}
std::string command() {
    std::ifstream file(root() + "/command");
    std::string value;
    std::getline(file, value);
    return value;
}
bool usable(rtlsdr_dev_t* dev, const char* op) {
    if (!dev) return false;
    if (dev->closed.load()) {
        event("write_after_close", dev->channel);
        return false;
    }
    if (dev->channel == 2 && std::strcmp(op, "frequency") == 0 && command() == "fail_frequency") {
        event("injected_runtime_failure", dev->channel);
        return false;
    }
    const char* failure = std::getenv("VECTORWARP_KRAKEN_SIM_FAIL");
    if (failure && std::strcmp(failure, op) == 0 && dev->channel == 2) {
        event("injected_setup_failure", dev->channel);
        return false;
    }
    return true;
}
uint32_t hash(uint64_t index) {
    uint64_t x = index + 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return static_cast<uint32_t>(x ^ (x >> 31));
}
unsigned char quantize(double value) {
    return static_cast<unsigned char>(std::clamp(std::lround(value + 127.5), 0L, 255L));
}
void fill(std::vector<unsigned char>& bytes, unsigned channel, uint64_t block, bool noise) {
    static constexpr std::array<double, 5> phase{{0, 12, -21, 34, -46}};
    static constexpr std::array<double, 5> gain{{1, .9, 1.1, .85, 1.05}};
    const double theta = phase.at(channel) * 3.14159265358979323846 / 180;
    // A distinct envelope exposes calibration-noise blocks mislabeled as live IQ.
    const double scale = noise ? 1.0 : 0.5;
    const double cr = scale * gain.at(channel) * std::cos(theta);
    const double ci = scale * gain.at(channel) * std::sin(theta);
    for (size_t i = 0; i < bytes.size() / 2; ++i) {
        const uint32_t random = hash(block * (bytes.size() / 2) + i);
        const double re = (static_cast<int>(random & 0xffff) - 32767.5) / 512;
        const double im = (static_cast<int>(random >> 16) - 32767.5) / 512;
        bytes[2 * i] = quantize(cr * re - ci * im);
        bytes[2 * i + 1] = quantize(ci * re + cr * im);
    }
    // A deterministic calibration-only tail marker exposes FIR history that
    // survives a transition even when all stale USB callbacks were rejected.
    if (noise && bytes.size() >= 14) std::fill(bytes.end() - 14, bytes.end(), static_cast<unsigned char>(255));
}
}

extern "C" {
uint32_t rtlsdr_get_device_count() { event("simulated_enumeration", -1, count()); return count(); }
const char* rtlsdr_get_device_name(uint32_t) { return "VECTORWARP SIMULATED RTL-SDR (NO USB)"; }
int rtlsdr_get_device_usb_strings(uint32_t index, char* manufacturer, char* product, char* serial) {
    if (index >= count()) return -1;
    if (manufacturer) std::strcpy(manufacturer, "VectorWarp simulated test");
    if (product) std::strcpy(product, "NO USB");
    if (serial) std::snprintf(serial, 256, "%u", 1000 + index);
    return 0;
}
int rtlsdr_open(rtlsdr_dev_t** output, uint32_t index) {
    if (!output || index >= count()) return -1;
    auto dev = std::make_unique<rtlsdr_dev_t>();
    dev->channel = index;
    *output = dev.get();
    { std::lock_guard<std::mutex> lock(round_mutex); devices.push_back(std::move(dev)); }
    event("open", index);
    return 0;
}
int rtlsdr_close(rtlsdr_dev_t* dev) {
    if (!dev) return -1;
    if (dev->reading.load()) { event("close_while_reading", dev->channel); return -1; }
    dev->closed = true;
    event("close", dev->channel, dev->noise.load() ? 1 : 0);
    return 0;  // Retain object to turn later use into a deterministic test failure.
}
int rtlsdr_set_dithering(rtlsdr_dev_t* dev, int) { return usable(dev, "dithering") ? 0 : -1; }
int rtlsdr_set_sample_rate(rtlsdr_dev_t* dev, uint32_t rate) {
    if (!usable(dev, "sample_rate")) return -1;
    dev->rate = rate; event("sample_rate", dev->channel, rate); return 0;
}
uint32_t rtlsdr_get_sample_rate(rtlsdr_dev_t* dev) { return dev->rate.load(); }
int rtlsdr_set_center_freq(rtlsdr_dev_t* dev, uint32_t frequency) {
    if (!usable(dev, "frequency")) return -1;
    dev->frequency = frequency; event("frequency", dev->channel, frequency); return 0;
}
uint32_t rtlsdr_get_center_freq(rtlsdr_dev_t* dev) { return dev->frequency.load(); }
int rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t* dev, int) { return usable(dev, "gain_mode") ? 0 : -1; }
int rtlsdr_get_tuner_gains(rtlsdr_dev_t* dev, int* gains) {
    static constexpr int table[] = {0,9,14,27,37,77,87,125,144,157,166,197,207,229,
                                    254,280,297,328,338,364,372,386,402,421,434,439,445,480,496};
    if (!dev || dev->closed.load()) return -1;
    if (gains) std::copy(std::begin(table), std::end(table), gains);
    return static_cast<int>(std::size(table));
}
int rtlsdr_set_tuner_gain(rtlsdr_dev_t* dev, int gain) {
    if (!usable(dev, "gain")) return -1;
    // Pinned Kraken R820T fork selects alternating LNA/mixer steps, rather
    // than echoing the requested tenths-dB value. E.g.400 reads back402.
    static constexpr int lna[] = {0,9,13,40,38,13,31,22,26,31,26,14,19,5,35,13};
    static constexpr int mixer[] = {0,5,10,10,19,9,10,25,17,10,8,16,13,6,3,-8};
    int actual = 0;
    for (int i = 1; i < 16 && actual < gain; ++i) {
        actual += lna[i];
        if (actual >= gain) break;
        actual += mixer[i];
    }
    dev->gain = actual;
    event("gain", dev->channel, gain);
    event("actual_gain", dev->channel, actual);
    return 0;
}
int rtlsdr_get_tuner_gain(rtlsdr_dev_t* dev) { return dev->gain.load(); }
int rtlsdr_set_freq_correction(rtlsdr_dev_t* dev, int ppm) {
    if (!usable(dev, "ppm")) return -1;
    if (dev->correction.load() == ppm) return -2;  // Real fork's no-op result.
    dev->correction = ppm;
    return 0;
}
int rtlsdr_get_freq_correction(rtlsdr_dev_t* dev) { return dev ? dev->correction.load() : 0; }
int rtlsdr_set_sample_freq_correction_f(rtlsdr_dev_t* dev, float correction) {
    if (!usable(dev, "sample_correction")) return -1;
    event("sample_correction", dev->channel, std::llround(correction * 1e9));
    return 0;  // Shared perfect sample clock; no simulated drift in this fixture.
}
int rtlsdr_set_bias_tee(rtlsdr_dev_t* dev, int on) {
    if (!usable(dev, "noise")) return -1;
    if (!on && dev->channel == 2 && command() == "fail_noise_off") {
        event("injected_noise_failure", dev->channel); return -1;
    }
    const bool previous = dev->noise.exchange(on != 0);
    if (on) {
        dev->stale_noise_remaining = 0;
    } else if (previous) {
        // Model the 32 already-submitted USB transfers. They retain old noise
        // samples even when their callbacks arrive after the settling timer.
        dev->stale_noise_ready_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            (Clock::now() + std::chrono::milliseconds(450)).time_since_epoch()).count();
        dev->stale_noise_remaining = 32;
    }
    event("noise", dev->channel, on); return 0;
}
int rtlsdr_set_bias_tee_gpio(rtlsdr_dev_t* dev, int gpio, int on) {
    if (gpio == 0) return rtlsdr_set_bias_tee(dev, on);
    if (!usable(dev, "gpio")) return -1;
    event("gpio", dev->channel, (gpio << 8) | on); return 0;
}
int rtlsdr_reset_buffer(rtlsdr_dev_t* dev) { return usable(dev, "reset") ? 0 : -1; }
int rtlsdr_cancel_async(rtlsdr_dev_t* dev) {
    if (!dev) return -1;
    if (!dev->reading.load()) return -2;
    dev->cancelled = true; event("cancel", dev->channel); round_cv.notify_all(); return 0;
}
int rtlsdr_read_async(rtlsdr_dev_t* dev, rtlsdr_read_async_cb_t callback, void* context,
                      uint32_t, uint32_t length) {
    if (!usable(dev, "read") || length == 0 || length > 1048576 || (length & 1)) return -1;
    if (dev->reading.exchange(true)) return -2;
    dev->cancelled = false;
    event("read_start", dev->channel, length);
    std::vector<unsigned char> bytes(length);
    uint64_t callbacks = 0;
    bool short_callback_sent = false;
    while (!dev->cancelled.load()) {
        uint64_t block;
        Clock::time_point target;
        {
            std::lock_guard<std::mutex> lock(round_mutex);
            block = round_id; target = round_time;
        }
        std::this_thread::sleep_until(target);
        const auto mode = command();
        // One device loses callbacks while the others keep advancing. No USB involved.
        if (!(mode == "stall_channel_3" && dev->channel == 3)) {
            bool noise = dev->noise.load();
            const unsigned stale = dev->stale_noise_remaining.load();
            if (!noise && stale) {
                std::this_thread::sleep_until(Clock::time_point(std::chrono::nanoseconds(
                    dev->stale_noise_ready_ns.load())));
                noise = true;
                if (stale == 32) event("delayed_noise_completion", dev->channel, 32);
                dev->stale_noise_remaining.fetch_sub(1);
            }
            fill(bytes, dev->channel, block, noise);
            // Asynchronous delivery jitter without altering sample-clock alignment.
            std::this_thread::sleep_for(std::chrono::microseconds(80 * dev->channel));
            if (mode == "short_callback" && dev->channel == 2 && !short_callback_sent) {
                event("injected_short_callback", dev->channel, length - 2);
                callback(bytes.data(), length - 2, context);
                short_callback_sent = true;
            } else {
                callback(bytes.data(), length, context);
            }
            ++callbacks;
        }
        std::unique_lock<std::mutex> lock(round_mutex);
        if (++round_arrived == count()) {
            round_arrived = 0; ++round_id;
            // No catch-up burst after CPU scheduling delay.
            round_time = Clock::now() + std::chrono::microseconds(
                static_cast<long long>(length / 2) * 1000000 / dev->rate.load());
            round_cv.notify_all();
        } else {
            // The deliberate450ms USB-completion delay must not advance one
            // channel twice through the same sample block. Cancellation wakes
            // this barrier; simulated missing callbacks still participate.
            round_cv.wait(lock, [&] {
                return round_id != block || dev->cancelled.load();
            });
        }
    }
    // Exposes the unsafe upstream "cancel, sleep 200ms, close" pattern.
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    dev->reading = false;
    event("read_exit", dev->channel, callbacks);
    return 0;
}
}
