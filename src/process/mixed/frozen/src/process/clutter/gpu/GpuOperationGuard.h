#pragma once

// Stack ownership for an in-flight benchmark operation. abort() must be
// noexcept and idempotent after successful finish.
template<class Operation>
class GpuOperationGuard {
  Operation* operation_;
public:
  explicit GpuOperationGuard(Operation* operation) : operation_(operation) {}
  ~GpuOperationGuard() { if (operation_) operation_->abort(); }
  GpuOperationGuard(const GpuOperationGuard&) = delete;
  GpuOperationGuard& operator=(const GpuOperationGuard&) = delete;
};
