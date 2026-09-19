/// @file TestAmbiguity.cpp
/// @brief Unit test for Ambiguity.cpp
/// @author 30hours
/// @author Dan G
/// @todo Add golden data IqData file for testing.
/// @todo Declaration match to coding style?

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "process/ambiguity/Ambiguity.h"
#include "process/utility/FftwThreads.h"

#include <random>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <complex>
#include <cstdlib>
#include <deque>
#include <string>

namespace {
class ScopedFftSetting {
  const char* name_;
  const char* previous_;
  std::string saved_;
public:
  explicit ScopedFftSetting(const char* value, const char* name = "VECTORWARP_FFTW_PLAN")
      : name_(name), previous_(std::getenv(name)) {
    if (previous_) saved_ = previous_;
    setenv(name_, value, 1);
  }
  ~ScopedFftSetting() {
    if (previous_) setenv(name_, saved_.c_str(), 1);
    else unsetenv(name_);
  }
};
}

/// @brief Use random_device as RNG.
std::random_device g_rd;

/// @brief Generate random IQ data.
/// @param iqData Address of IqData object.
/// @details Have to use out ref parameter because there's no copy/move ctors.
/// @return Void.
void random_iq(IqData& iq_data) {
    std::mt19937 gen(g_rd());
    std::uniform_real_distribution<> dist(-100.0, 100.0);

    for (uint32_t i = 0; i < iq_data.get_n(); ++i) {
        iq_data.push_back({dist(gen), dist(gen)});
    }
}

/// @brief Read file to IqData buffer.
/// @param buffer1 IqData buffer reference.
/// @param buffer2 IqData buffer surveillance.
/// @param file String of file name.
/// @return Void.
void read_file(IqData& buffer1, IqData& buffer2, const std::string& file)
{
  short i1, q1, i2, q2;
  auto file_replay = fopen(file.c_str(), "rb");
  if (!file_replay) {
    return;
  }

  auto read_short = [](short& v, FILE* fid) {
    auto rv{fread(&v, 1, sizeof(short), fid)};
    return rv == sizeof(short);
  };

  while (!feof(file_replay))
  {
    if (!read_short(i1, file_replay)) break;
    if (!read_short(q1, file_replay)) break;
    if (!read_short(i2, file_replay)) break;
    if (!read_short(q2, file_replay)) break;

    buffer1.push_back({(double)i1, (double)q1});
    buffer2.push_back({(double)i2, (double)q2});

    // only read for the buffer length - this class is very poorly designed
    if (buffer1.get_length() == buffer1.get_n()) {
        break;
    }
  }

  fclose(file_replay);
}

/// @brief Test constructor.
/// @details Check constructor parameters created correctly.
TEST_CASE("Constructor", "[constructor]")
{
    int32_t delayMin{-10};
    int32_t delayMax{300};
    int32_t dopplerMin{-300};
    int32_t dopplerMax{300};

    uint32_t fs{2'000'000};
    float tCpi{0.5};
    uint32_t nSamples = tCpi * fs;    // narrow on purpose

    Ambiguity ambiguity(delayMin, delayMax, dopplerMin, 
      dopplerMax, fs, nSamples);

    CHECK_THAT(ambiguity.get_cpi(), Catch::Matchers::WithinAbs(tCpi, 0.02));
    CHECK(ambiguity.get_doppler_middle() == 0);
    CHECK(ambiguity.get_n_corr() == 3322);
    CHECK(ambiguity.get_n_delay_bins() == delayMax + std::abs(delayMin) + 1);
    CHECK(ambiguity.get_n_doppler_bins() == 301);
    CHECK(ambiguity.get_nfft() == 3622);
}

/// @brief Test constructor with rounded Hamming number FFT length.
TEST_CASE("Constructor_Round", "[constructor]")
{
    int32_t delayMin{-10};
    int32_t delayMax{300};
    int32_t dopplerMin{-300};
    int32_t dopplerMax{300};

    uint32_t fs{2'000'000};
    float tCpi{0.5};
    uint32_t nSamples = tCpi * fs;    // narrow on purpose

    Ambiguity ambiguity(delayMin, delayMax, dopplerMin, 
      dopplerMax, fs, nSamples, true);

    CHECK_THAT(ambiguity.get_cpi(), Catch::Matchers::WithinAbs(tCpi, 0.02));
    CHECK(ambiguity.get_doppler_middle() == 0);
    CHECK(ambiguity.get_n_corr() == 3322);
    CHECK(ambiguity.get_n_delay_bins() == delayMax + std::abs(delayMin) + 1);
    CHECK(ambiguity.get_n_doppler_bins() == 301);
    CHECK(ambiguity.get_nfft() == 3645);
}

TEST_CASE("Ambiguity preserves the caller FFTW planning budget", "[constructor][fftw]")
{
    REQUIRE(fftw_init_threads() != 0);
    const int saved = blah2::fftw_planner_threads();
    blah2::set_fftw_planner_threads(3);
    {
      Ambiguity ambiguity(-7, 7, 0, 0, 1000, 1000);
    }
    CHECK(blah2::fftw_planner_threads() == 3);
    blah2::set_fftw_planner_threads(saved);
}

TEST_CASE("Ambiguity FFTW plan mode validates and preserves results", "[constructor][fftw]")
{
  {
    ScopedFftSetting invalid("invalid");
    REQUIRE_THROWS_AS(Ambiguity(-2, 2, 0, 0, 100, 8), std::invalid_argument);
  }
  std::deque<std::complex<double>> reference;
  for (int i = 0; i < 8; ++i) reference.push_back({double(i + 1), -double(i)});
  auto fill = [](IqData& value) {
    for (int i = 0; i < 8; ++i) value.push_back({double(i - 2), double(i + 3)});
  };
  std::vector<std::vector<std::complex<double>>> expected;
  {
    ScopedFftSetting estimate("estimate");
    Ambiguity ambiguity(-2, 2, 0, 0, 100, 8);
    IqData surveillance(8); fill(surveillance);
    expected = ambiguity.process(reference, &surveillance)->data;
  }
  {
    ScopedFftSetting measure("measure");
    Ambiguity ambiguity(-2, 2, 0, 0, 100, 8);
    IqData surveillance(8); fill(surveillance);
    const auto* reusable = &surveillance.view_data().front();
    const auto& actual = ambiguity.process(reference, &surveillance, true)->data;
    REQUIRE(surveillance.get_length() == 8);
    REQUIRE(&surveillance.view_data().front() == reusable);
    REQUIRE(actual.size() == expected.size());
    for (size_t row = 0; row < expected.size(); ++row)
      for (size_t col = 0; col < expected[row].size(); ++col)
        CHECK_THAT(std::abs(actual[row][col] - expected[row][col]),
          Catch::Matchers::WithinAbs(0, 1e-10));
    ambiguity.process(reference, &surveillance);
    REQUIRE(surveillance.get_length() == 0);
  }
}

TEST_CASE("Power-of-two padding preserves signed delay-Doppler maps", "[process][regression][range-fft]")
{
  constexpr unsigned samples = 160;
  const bool rounded = GENERATE(false, true);
  std::deque<std::complex<double>> reference;
  for (unsigned i = 0; i < samples; ++i)
    reference.push_back({double(int(i % 11) - 5), double(int(i % 7) - 3)});
  auto fill = [](IqData& signal) {
    for (unsigned i = 0; i < samples; ++i)
      signal.push_back({double(int(i % 13) - 6), double(int(i % 5) - 2)});
  };
  std::vector<std::vector<std::complex<double>>> expected;
  {
    ScopedFftSetting setting("default", "VECTORWARP_RANGE_FFT");
    Ambiguity baseline(-7, 7, -40, 40, 1000, samples, rounded);
    CHECK(baseline.get_nfft() == (rounded ? 20 : 19));
    IqData signal(samples); fill(signal);
    expected = baseline.process(reference, &signal)->data;
  }
  {
    ScopedFftSetting setting("power2", "VECTORWARP_RANGE_FFT");
    Ambiguity padded(-7, 7, -40, 40, 1000, samples, rounded);
    REQUIRE(padded.get_nfft() == 32);
    REQUIRE(padded.get_n_corr() == 12);
    REQUIRE(padded.get_n_delay_bins() == 15);
    REQUIRE(padded.get_n_doppler_bins() == 13);
    IqData signal(samples); fill(signal);
    const auto* actual = padded.process(reference, &signal);
    REQUIRE(actual->delay.front() == -7);
    REQUIRE(actual->delay.back() == 7);
    REQUIRE(actual->data.size() == expected.size());
    for (size_t row = 0; row < expected.size(); ++row) {
      REQUIRE(actual->data[row].size() == expected[row].size());
      for (size_t col = 0; col < expected[row].size(); ++col)
        CHECK_THAT(std::abs(actual->data[row][col] - expected[row][col]),
          Catch::Matchers::WithinAbs(0, 1e-10));
    }
    REQUIRE_THROWS_AS(Ambiguity(-1, 1, 0, 0, 65534, 65534), std::invalid_argument);
  }
  {
    ScopedFftSetting setting("invalid", "VECTORWARP_RANGE_FFT");
    REQUIRE_THROWS_AS(Ambiguity(-2, 2, 0, 0, 100, 8), std::invalid_argument);
  }
}

TEST_CASE("Doppler buffer can exceed range FFT", "[process][regression]")
{
    const int high = GENERATE(530, 2600);
    constexpr uint32_t samples = 4800;
    Ambiguity ambiguity(-2, 4, -high, high, 48000, samples, false);
    REQUIRE(ambiguity.get_n_doppler_bins() > ambiguity.get_nfft());
    std::deque<std::complex<double>> reference(samples, {1, 0});
    IqData surveillance(samples);
    for (unsigned i = 0; i < samples; ++i) surveillance.push_back({1, 0});
    const auto* result = ambiguity.process(reference, &surveillance);
    const unsigned bins = ambiguity.get_n_doppler_bins();
    for (unsigned d = 0; d < bins; ++d)
      for (unsigned r = 0; r < result->delay.size(); ++r) {
        const double expected = d == bins / 2 ?
          (ambiguity.get_n_corr() - std::abs(result->delay[r])) * bins : 0;
        CHECK_THAT(std::abs(result->data[d][r] - expected),
          Catch::Matchers::WithinAbs(0, 1e-8));
      }
}

TEST_CASE("Signed delays must fit the correlation block", "[process][regression]")
{
    const bool rounded = GENERATE(false, true);
    // The 400-point rounded FFT has room for index 245, but its 199-sample
    // blocks cannot distinguish that label from a real negative delay of -155.
    REQUIRE_THROWS_AS(Ambiguity(-10, 245, -6000, 6000, 2400000, 480000, rounded),
      std::invalid_argument);
    REQUIRE_THROWS_AS(Ambiguity(-199, 10, -6000, 6000, 2400000, 480000, rounded),
      std::invalid_argument);
    REQUIRE_NOTHROW(Ambiguity(-198, 198, -6000, 6000, 2400000, 480000, rounded));
    REQUIRE_NOTHROW(Ambiguity(-10, 245, -4000, 4000, 2400000, 480000, rounded));
}

TEST_CASE("Impulse correlation preserves signed boundary lags", "[process][regression]")
{
    const bool rounded = GENERATE(false, true);
    const int lag = GENERATE(-7, -3, 0, 3, 7);
    Ambiguity ambiguity(-7, 7, 0, 0, 100, 8, rounded);
    std::deque<std::complex<double>> reference(8, {0, 0});
    reference[lag < 0 ? -lag : 0] = {1, 0};
    IqData surveillance(8);
    for (int i = 0; i < 8; ++i)
      surveillance.push_back({i == (lag > 0 ? lag : 0) ? 1.0 : 0.0, 0});
    const auto* result = ambiguity.process(reference, &surveillance);
    for (unsigned r = 0; r < result->delay.size(); ++r)
      CHECK_THAT(std::abs(result->data[0][r] - (result->delay[r] == lag ? 1.0 : 0.0)),
        Catch::Matchers::WithinAbs(0, 1e-12));
    REQUIRE_THROWS_AS(Ambiguity(-8, 7, 0, 0, 100, 8, rounded), std::invalid_argument);
    REQUIRE_THROWS_AS(Ambiguity(-7, 8, 0, 0, 100, 8, rounded), std::invalid_argument);
}

TEST_CASE("Lag-bounded FFT matches a direct signed-lag correlation oracle", "[process][regression]")
{
  const int middle = GENERATE(-25, 0, 17);
  struct Geometry { int32_t first, last; };
  for (const auto geometry : {Geometry{-7, -2}, Geometry{-3, 4}, Geometry{2, 7}})
  {
    constexpr uint32_t samples = 8;
    Ambiguity ambiguity(geometry.first, geometry.last, middle, middle, 100, samples, false);
    REQUIRE(ambiguity.get_nfft() == samples +
      static_cast<uint32_t>(std::max(std::abs(geometry.first), std::abs(geometry.last))));
    std::deque<std::complex<double>> reference;
    IqData surveillance(samples);
    for (uint32_t i = 0; i < samples; ++i) {
      reference.push_back({double(i + 1), double(int((i * 3) % 5) - 2)});
      surveillance.push_back({double(int((i * 2) % 7) - 3), double(i + 2)});
    }
    const auto signal = surveillance.view_data();
    const auto* result = ambiguity.process(reference, &surveillance);
    for (uint16_t index = 0; index < result->delay.size(); ++index) {
      const int32_t lag = result->delay[index];
      std::complex<double> expected{};
      for (int32_t sample = 0; sample < static_cast<int32_t>(samples); ++sample) {
        const int32_t referenceSample = sample - lag;
        if (referenceSample >= 0 && referenceSample < static_cast<int32_t>(samples))
          expected += signal[sample] * std::conj(reference[referenceSample] *
            std::polar(1.0, 2 * std::acos(-1.0) * middle * referenceSample / 100));
      }
      CHECK_THAT(std::abs(result->data[0][index] - expected),
        Catch::Matchers::WithinAbs(0, 1e-10));
    }
  }
}

/// @brief Test simple ambiguity processing.
TEST_CASE("Process_Simple", "[process]")
{
    auto round_hamming = GENERATE(true, false);

    int32_t delayMin{-10};
    int32_t delayMax{300};
    int32_t dopplerMin{-300};
    int32_t dopplerMax{300};

    uint32_t fs{2'000'000};
    float tCpi{0.5};
    uint32_t nSamples = tCpi * fs;    // narrow on purpose

    Ambiguity ambiguity(delayMin, delayMax, dopplerMin, 
      dopplerMax, fs, nSamples, round_hamming);

    IqData x{nSamples};
    IqData y{nSamples};

    random_iq(x);
    random_iq(y);
    auto map{ambiguity.process(&x, &y)};
    map->set_metrics();
    CHECK(map->maxPower > 0.0);
    CHECK(map->noisePower > 0.0);
}

/// @brief Test processing from a file.
TEST_CASE("Process_File", "[process]")
{
    std::filesystem::path test_input_file("20231214-230611.rspduo");
    // Bail if the test file doesn't exist
    if (!std::filesystem::exists(test_input_file)) {
      SKIP("Input test file does not exist.");
    }
    
    auto round_hamming = GENERATE(true, false);

    int32_t delayMin{-10};
    int32_t delayMax{300};
    int32_t dopplerMin{-300};
    int32_t dopplerMax{300};

    uint32_t fs{2'000'000};
    float tCpi{0.5};
    uint32_t nSamples = tCpi * fs;    // narrow on purpose

    Ambiguity ambiguity(delayMin, delayMax, dopplerMin, 
      dopplerMax, fs, nSamples, round_hamming);
    IqData x{nSamples};
    IqData y{nSamples};

    read_file(x, y, "20231214-230611.rspduo");
    REQUIRE(x.get_length() == x.get_n());

    auto map{ambiguity.process(&x ,&y)};
    map->set_metrics();
    CHECK_THAT(map->maxPower, Catch::Matchers::WithinAbs(30.2816, 0.001));
    CHECK_THAT(map->noisePower, Catch::Matchers::WithinAbs(76.918, 0.001));
}
