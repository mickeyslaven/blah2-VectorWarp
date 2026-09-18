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

#include <random>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <complex>
#include <deque>

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
    const int saved = fftw_planner_nthreads();
    fftw_plan_with_nthreads(3);
    {
      Ambiguity ambiguity(-7, 7, 0, 0, 1000, 1000);
    }
    CHECK(fftw_planner_nthreads() == 3);
    fftw_plan_with_nthreads(saved);
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
