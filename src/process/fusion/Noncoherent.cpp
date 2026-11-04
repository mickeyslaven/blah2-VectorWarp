#include "Noncoherent.h"

#include <cmath>
#include <stdexcept>

std::unique_ptr<Map<std::complex<double>>> Noncoherent::process(
  const std::vector<Map<std::complex<double>> *>& maps) const
{
  if (maps.empty() || maps.front() == nullptr)
    throw std::invalid_argument("At least one ambiguity map is required");
  const uint32_t rows = maps.front()->get_nRows();
  const uint32_t columns = maps.front()->get_nCols();
  auto fused = std::make_unique<Map<std::complex<double>>>(rows, columns);
  process_into(maps, *fused);
  return fused;
}

void Noncoherent::process_into(
  const std::vector<Map<std::complex<double>> *>& maps,
  Map<std::complex<double>>& fused) const
{
  if (maps.empty() || maps.front() == nullptr)
    throw std::invalid_argument("At least one ambiguity map is required");
  const uint32_t rows = maps.front()->get_nRows();
  const uint32_t columns = maps.front()->get_nCols();
  if (fused.get_nRows() != rows || fused.get_nCols() != columns)
    throw std::invalid_argument("Fused ambiguity-map dimensions do not match");
  fused.delay = maps.front()->delay;
  fused.doppler = maps.front()->doppler;
  for (auto *map : maps)
    if (!map || map->get_nRows() != rows || map->get_nCols() != columns)
      throw std::invalid_argument("Ambiguity-map dimensions do not match");
  for (uint32_t row = 0; row < rows; row++)
    for (uint32_t column = 0; column < columns; column++)
    {
      double power = 0;
      for (auto *map : maps) power += std::norm(map->data[row][column]);
      fused.data[row][column] = {
        std::sqrt(power / static_cast<double>(maps.size())), 0};
    }
}
