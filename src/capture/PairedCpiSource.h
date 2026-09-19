#pragma once
class PairedCpiQueue;

// Optional receiver extension.  Source deliberately remains unchanged so an
// installed receiver module compiled before this feature still uses FIFO I/O.
class PairedCpiSource {
public:
  virtual ~PairedCpiSource() = default;
  virtual void set_paired_cpi_queue(PairedCpiQueue* queue) = 0;
};
