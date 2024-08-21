// Copyright 2024 Advanced Micro Devices, Inc
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef SHORTFIN_SUPPORT_IREE_HELPERS_H
#define SHORTFIN_SUPPORT_IREE_HELPERS_H

#include <memory>
#include <stdexcept>

#include "iree/base/api.h"
#include "iree/base/internal/file_io.h"
#include "iree/hal/api.h"
#include "iree/modules/hal/types.h"
#include "iree/vm/api.h"
#include "shortfin/support/api.h"

namespace shortfin {

// -------------------------------------------------------------------------- //
// Type conversion from C types.
// These are in the global shortfin namespace.
// -------------------------------------------------------------------------- //

inline std::string_view to_string_view(iree_string_view_t isv) {
  return std::string_view(isv.data, isv.size);
}

namespace iree {

// -------------------------------------------------------------------------- //
// RAII wrappers
// -------------------------------------------------------------------------- //

namespace detail {

struct hal_buffer_ptr_helper {
  static void retain(iree_hal_buffer_t *obj) { iree_hal_buffer_retain(obj); }
  static void release(iree_hal_buffer_t *obj) { iree_hal_buffer_release(obj); }
};

struct hal_command_buffer_helper {
  static void retain(iree_hal_command_buffer_t *obj) {
    iree_hal_command_buffer_retain(obj);
  }
  static void release(iree_hal_command_buffer_t *obj) {
    iree_hal_command_buffer_release(obj);
  }
};

struct hal_device_ptr_helper {
  static void retain(iree_hal_device_t *obj) { iree_hal_device_retain(obj); }
  static void release(iree_hal_device_t *obj) { iree_hal_device_release(obj); }
};

struct hal_driver_ptr_helper {
  static void retain(iree_hal_driver_t *obj) { iree_hal_driver_retain(obj); }
  static void release(iree_hal_driver_t *obj) { iree_hal_driver_release(obj); }
};

struct hal_fence_ptr_helper {
  static void retain(iree_hal_fence_t *obj) { iree_hal_fence_retain(obj); }
  static void release(iree_hal_fence_t *obj) { iree_hal_fence_release(obj); }
};

struct hal_semaphore_ptr_helper {
  static void retain(iree_hal_semaphore_t *obj) {
    iree_hal_semaphore_retain(obj);
  }
  static void release(iree_hal_semaphore_t *obj) {
    iree_hal_semaphore_release(obj);
  }
};

struct vm_context_ptr_helper {
  static void retain(iree_vm_context_t *obj) { iree_vm_context_retain(obj); }
  static void release(iree_vm_context_t *obj) { iree_vm_context_release(obj); }
};

struct vm_instance_ptr_helper {
  static void retain(iree_vm_instance_t *obj) { iree_vm_instance_retain(obj); }
  static void release(iree_vm_instance_t *obj) {
    iree_vm_instance_release(obj);
  }
};

struct vm_module_ptr_helper {
  static void retain(iree_vm_module_t *obj) { iree_vm_module_retain(obj); }
  static void release(iree_vm_module_t *obj) { iree_vm_module_release(obj); }
};

};  // namespace detail

// Wraps an IREE retain/release style object pointer in a smart-pointer
// like wrapper.
template <typename T, typename Helper>
class object_ptr {
 public:
  object_ptr() : ptr(nullptr) {}
  object_ptr(const object_ptr &other) : ptr(other.ptr) {
    if (ptr) {
      Helper::retain(ptr);
    }
  }
  object_ptr(object_ptr &&other) : ptr(other.ptr) { other.ptr = nullptr; }
  object_ptr &operator=(object_ptr &&other) {
    ptr = other.ptr;
    other.ptr = nullptr;
    return *this;
  }
  ~object_ptr() {
    if (ptr) {
      Helper::release(ptr);
    }
  }

  // Constructs a new object_ptr by transferring ownership of a raw
  // pointer.
  static object_ptr steal_reference(T *owned) { return object_ptr(owned); }
  static object_ptr borrow_reference(T *owned) {
    Helper::retain(owned);
    return object_ptr(owned);
  }
  operator T *() const noexcept { return ptr; }

  // Releases any current reference held by this instance and returns a
  // pointer to the raw backing pointer. This is typically used for passing
  // to out parameters which are expected to store a new owned pointer directly.
  T **for_output() {
    reset();
    return &ptr;
  }

  operator bool() const { return ptr != nullptr; }
  T *get() const { return ptr; }
  void reset(T *other = nullptr) {
    if (ptr) {
      Helper::release(ptr);
    }
    ptr = other;
  }
  T *release() {
    T *ret = ptr;
    ptr = nullptr;
    return ret;
  }

 private:
  // Assumes the reference count for owned_ptr.
  object_ptr(T *owned_ptr) : ptr(owned_ptr) {}
  T *ptr = nullptr;
};

using hal_buffer_ptr =
    object_ptr<iree_hal_buffer_t, detail::hal_buffer_ptr_helper>;
using hal_command_buffer_ptr =
    object_ptr<iree_hal_command_buffer_t, detail::hal_command_buffer_helper>;
using hal_driver_ptr =
    object_ptr<iree_hal_driver_t, detail::hal_driver_ptr_helper>;
using hal_device_ptr =
    object_ptr<iree_hal_device_t, detail::hal_device_ptr_helper>;
using hal_fence_ptr =
    object_ptr<iree_hal_fence_t, detail::hal_fence_ptr_helper>;
using hal_semaphore_ptr =
    object_ptr<iree_hal_semaphore_t, detail::hal_semaphore_ptr_helper>;
using vm_context_ptr =
    object_ptr<iree_vm_context_t, detail::vm_context_ptr_helper>;
using vm_instance_ptr =
    object_ptr<iree_vm_instance_t, detail::vm_instance_ptr_helper>;
using vm_module_ptr =
    object_ptr<iree_vm_module_t, detail::vm_module_ptr_helper>;

// Holds a pointer allocated by some allocator, deleting it if still owned
// at destruction time.
template <typename T>
struct allocated_ptr {
  iree_allocator_t allocator;

  allocated_ptr(iree_allocator_t allocator) : allocator(allocator) {}
  ~allocated_ptr() { reset(); }

  void reset(T *other = nullptr) {
    iree_allocator_free(allocator, ptr);
    ptr = other;
  }

  T *release() {
    T *ret = ptr;
    ptr = nullptr;
    return ret;
  }

  T *operator->() noexcept { return ptr; }
  T **operator&() noexcept { return &ptr; }
  operator T *() noexcept { return ptr; }
  T *get() noexcept { return ptr; }

  // Releases any current reference held by this instance and returns a
  // pointer to the raw backing pointer. This is typically used for passing
  // to out parameters which are expected to store a new owned pointer directly.
  T **for_output() {
    reset();
    return &ptr;
  }

 private:
  T *ptr = nullptr;
};

// Wraps an iree_file_contents_t*, freeing it when it goes out of scope.
// The contents can be released as an iree_allocator_t which transfers
// ownership to some consumer.
class file_contents_ptr {
 public:
  file_contents_ptr() {}

  // Frees any contained contents.
  void reset() noexcept {
    if (contents_) {
      iree_file_contents_free(contents_);
      contents_ = nullptr;
    }
  }

  // Frees any contained contents and returns a pointer to the pointer that
  // can be passed as an out parameter, causing this instance to take ownership
  // of anything set on it.
  iree_file_contents_t **for_output() noexcept {
    reset();
    return &contents_;
  }

  operator iree_file_contents_t *() noexcept { return contents_; }

  // Access the raw contents.
  iree_const_byte_span_t const_buffer() const noexcept {
    return contents_->const_buffer;
  }

  // Returns a deallocator that can be used to free the contents. Note that
  // this method alone does not release ownership of the contents. Typically
  // that is done once the consumer of this allocator returns successfully.
  iree_allocator_t deallocator() {
    return iree_file_contents_deallocator(contents_);
  }

  // Releases ownership of the contained contents.
  iree_file_contents_t *release() {
    iree_file_contents_t *p = contents_;
    contents_ = nullptr;
    return p;
  }

 private:
  iree_file_contents_t *contents_ = nullptr;
};

// -------------------------------------------------------------------------- //
// error and status handling
// -------------------------------------------------------------------------- //

// Captures an iree_status_t as an exception. The intent is that this is
// only used for failing statuses and the error instance will be
// immediately thrown.
class SHORTFIN_API error : public std::exception {
 public:
  error(std::string message, iree_status_t failing_status);
  error(iree_status_t failing_status);
  error(const error &) = delete;
  error &operator=(const error &) = delete;
  ~error() { iree_status_ignore(failing_status_); }
  const char *what() const noexcept override {
    if (!status_appended_) {
      AppendStatus();
    }
    return message_.c_str();
  };

 private:
  void AppendStatus() const noexcept;
  mutable std::string message_;
  mutable iree_status_t failing_status_;
  mutable bool status_appended_ = false;
};

#define SHORTFIN_IMPL_HANDLE_IF_API_ERROR(var, ...)                          \
  iree_status_t var = (IREE_STATUS_IMPL_IDENTITY_(                           \
      IREE_STATUS_IMPL_IDENTITY_(IREE_STATUS_IMPL_GET_EXPR_)(__VA_ARGS__))); \
  if (IREE_UNLIKELY(var)) {                                                  \
    throw ::shortfin::iree::error(                                           \
        IREE_STATUS_IMPL_ANNOTATE_SWITCH_(var, __VA_ARGS__));                \
  }

// Similar to IREE_RETURN_IF_ERROR but throws an error exception instead.
#define SHORTFIN_THROW_IF_ERROR(...)                    \
  SHORTFIN_IMPL_HANDLE_IF_API_ERROR(                    \
      IREE_STATUS_IMPL_CONCAT_(__status_, __COUNTER__), \
      IREE_STATUS_IMPL_IDENTITY_(IREE_STATUS_IMPL_IDENTITY_(__VA_ARGS__)))

// Convert an arbitrary C++ exception to an iree_status_t.
// WARNING: This is not to be used as flow control as it is expensive,
// perfoming type probing and comparisons in some cases!
inline iree_status_t exception_to_status(std::exception &e) {
  return iree_make_status(IREE_STATUS_UNKNOWN, "Unhandled exception: %s",
                          e.what());
}

// RAII wrapper around an iree_status_t that will ignore it when going out
// of scope. This is needed to avoid resource leaks when statuses are being
// used to signal a failure which may not be harvested.
class ignorable_status {
 public:
  ignorable_status() : status_(iree_ok_status()) {}
  ignorable_status(iree_status_t status) : status_(status) {}
  ignorable_status(const ignorable_status &) = delete;
  ignorable_status &operator=(iree_status_t status) {
    iree_status_ignore(status_);
    status_ = status;
    return *this;
  }
  ignorable_status &operator=(const ignorable_status &) = delete;
  ignorable_status(ignorable_status &&other) = delete;
  ~ignorable_status() { iree_status_ignore(status_); }

  operator iree_status_t() const { return status_; }
  iree_status_t status() const { return status_; }

 private:
  iree_status_t status_;
};

}  // namespace iree
}  // namespace shortfin

#endif  // SHORTFIN_SUPPORT_IREE_HELPERS_H