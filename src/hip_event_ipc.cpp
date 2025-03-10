/* Copyright (c) 2015 - 2021 Advanced Micro Devices, Inc.

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE. */

#include <hip/hip_runtime.h>

#include "hip_event.hpp"
#if !defined(_MSC_VER)
#include <unistd.h>
#endif

void ipcEventCallback(hipStream_t stream, hipError_t status, void* user_data) {
  std::atomic<int>* signal = reinterpret_cast<std::atomic<int>*>(user_data);
  signal->store(0);
  return;
}
// ================================================================================================

hipError_t ihipEventCreateWithFlags(hipEvent_t* event, unsigned flags);

namespace hip {

bool IPCEvent::createIpcEventShmemIfNeeded() {
#if !defined(_MSC_VER)
  if (ipc_evt_.ipc_shmem_) {
    // ipc_shmem_ already created, no need to create it again
    return true;
  }
  char name_template[] = "/tmp/eventXXXXXX";
  int temp_fd = mkstemp(name_template);

  ipc_evt_.ipc_name_ = name_template;
  ipc_evt_.ipc_name_.replace(0, 5, "/hip_");
  if (!amd::Os::MemoryMapFileTruncated(
          ipc_evt_.ipc_name_.c_str(),
          const_cast<const void**>(reinterpret_cast<void**>(&(ipc_evt_.ipc_shmem_))),
          sizeof(hip::ihipIpcEventShmem_t))) {
    return false;
  }
  ipc_evt_.ipc_shmem_->owners = 1;
  ipc_evt_.ipc_shmem_->read_index = -1;
  ipc_evt_.ipc_shmem_->write_index = 0;
  for (uint32_t sig_idx = 0; sig_idx < IPC_SIGNALS_PER_EVENT; ++sig_idx) {
    ipc_evt_.ipc_shmem_->signal[sig_idx] = 0;
  }

  close(temp_fd);
  return true;
#else
  return false;
#endif
}

hipError_t IPCEvent::query() {
  if (ipc_evt_.ipc_shmem_) {
    int prev_read_idx = ipc_evt_.ipc_shmem_->read_index;
    int offset = (prev_read_idx % IPC_SIGNALS_PER_EVENT);
    if (ipc_evt_.ipc_shmem_->read_index < prev_read_idx + IPC_SIGNALS_PER_EVENT &&
        ipc_evt_.ipc_shmem_->signal[offset] != 0) {
      return hipErrorNotReady;
    }
  }
  return hipSuccess;
}

hipError_t IPCEvent::synchronize() {
  if (ipc_evt_.ipc_shmem_) {
    int prev_read_idx = ipc_evt_.ipc_shmem_->read_index;
    if (prev_read_idx >= 0) {
      int offset = (prev_read_idx % IPC_SIGNALS_PER_EVENT);
      while ((ipc_evt_.ipc_shmem_->read_index < prev_read_idx + IPC_SIGNALS_PER_EVENT) &&
             (ipc_evt_.ipc_shmem_->signal[offset] != 0)) {
        amd::Os::sleep(1);
      }
    }
  }
  return hipSuccess;
}

hipError_t IPCEvent::streamWaitCommand(amd::Command*& command, amd::HostQueue* queue) {
  command = new amd::Marker(*queue, false);
  if (command == NULL) {
    return hipErrorOutOfMemory;
  }
  return hipSuccess;
}

hipError_t IPCEvent::enqueueStreamWaitCommand(hipStream_t stream, amd::Command* command) {
  auto t{new CallbackData{ipc_evt_.ipc_shmem_->read_index, ipc_evt_.ipc_shmem_}};
  StreamCallback* cbo = new StreamCallback(
      stream, reinterpret_cast<hipStreamCallback_t>(WaitThenDecrementSignal), t, command);
  if (!command->setCallback(CL_COMPLETE, ihipStreamCallback, cbo)) {
    command->release();
    return hipErrorInvalidHandle;
  }
  command->enqueue();
  command->awaitCompletion();
  return hipSuccess;
}

hipError_t IPCEvent::streamWait(hipStream_t stream, uint flags) {
  amd::HostQueue* queue = hip::getQueue(stream);

  amd::ScopedLock lock(lock_);
  if(query() != hipSuccess) {
    amd::Command* command;
    hipError_t status = streamWaitCommand(command, queue);
    if (status != hipSuccess) {
      return status;
    }
    status = enqueueStreamWaitCommand(stream, command);
    return status;
  }
  return hipSuccess;
}

hipError_t IPCEvent::recordCommand(amd::Command*& command, amd::HostQueue* queue) {
  bool recorded = isRecorded();
  if (!recorded) {
    command = new amd::Marker(*queue, kMarkerDisableFlush);
  } else {
    return Event::recordCommand(command, queue);
  }
  return hipSuccess;
}

hipError_t IPCEvent::enqueueRecordCommand(hipStream_t stream, amd::Command* command, bool record) {
  amd::HostQueue* queue = hip::getQueue(stream);
  bool recorded = isRecorded();
  if (!recorded) {
    amd::Event& tEvent = command->event();
    createIpcEventShmemIfNeeded();
    int write_index = ipc_evt_.ipc_shmem_->write_index++;
    int offset = write_index % IPC_SIGNALS_PER_EVENT;
    while (ipc_evt_.ipc_shmem_->signal[offset] != 0) {
      amd::Os::sleep(1);
    }
    // Lock signal.
    ipc_evt_.ipc_shmem_->signal[offset] = 1;
    ipc_evt_.ipc_shmem_->owners_device_id = deviceId();

    std::atomic<int>* signal = &ipc_evt_.ipc_shmem_->signal[offset];
    StreamCallback* cbo = new StreamCallback(
        stream, reinterpret_cast<hipStreamCallback_t>(ipcEventCallback), signal, command);
    if (!tEvent.setCallback(CL_COMPLETE, ihipStreamCallback, cbo)) {
      command->release();
      return hipErrorInvalidHandle;
    }
    command->enqueue();
    // waiting for the call back to be called
    command->awaitCompletion();

    // Update read index to indicate new signal.
    int expected = write_index - 1;
    while (!ipc_evt_.ipc_shmem_->read_index.compare_exchange_weak(expected, write_index)) {
      amd::Os::sleep(1);
    }
  } else {
    return Event::enqueueRecordCommand(stream, command, record);
  }
  return hipSuccess;
}

hipError_t IPCEvent::GetHandle(ihipIpcEventHandle_t* handle) {
  if (!createIpcEventShmemIfNeeded()) {
    return hipErrorInvalidConfiguration;
  }
  ipc_evt_.ipc_shmem_->owners_device_id = deviceId();
  ipc_evt_.ipc_shmem_->owners_process_id = getpid();
  memset(handle->shmem_name, 0, HIP_IPC_HANDLE_SIZE);
  ipc_evt_.ipc_name_.copy(handle->shmem_name, std::string::npos);
  return hipSuccess;
}

hipError_t IPCEvent::OpenHandle(ihipIpcEventHandle_t* handle) {
  ipc_evt_.ipc_name_ = handle->shmem_name;
  if (!amd::Os::MemoryMapFileTruncated(ipc_evt_.ipc_name_.c_str(),
                                       (const void**)&(ipc_evt_.ipc_shmem_),
                                       sizeof(ihipIpcEventShmem_t))) {
    return hipErrorInvalidValue;
  }

  if (getpid() == ipc_evt_.ipc_shmem_->owners_process_id.load()) {
    // If this is in the same process, return error.
    return hipErrorInvalidContext;
  }

  ipc_evt_.ipc_shmem_->owners += 1;
  setDeviceId(ipc_evt_.ipc_shmem_->owners_device_id.load());

  return hipSuccess;
}

}  // namespace hip

// ================================================================================================

hipError_t hipIpcGetEventHandle(hipIpcEventHandle_t* handle, hipEvent_t event) {
  HIP_INIT_API(hipIpcGetEventHandle, handle, event);
#if !defined(_MSC_VER)
  if (handle == nullptr || event == nullptr) {
    HIP_RETURN(hipErrorInvalidValue);
  }
  hip::Event* e = reinterpret_cast<hip::Event*>(event);
  HIP_RETURN(e->GetHandle(reinterpret_cast<ihipIpcEventHandle_t*>(handle)));
#else
  assert(0 && "Unimplemented");
  HIP_RETURN(hipErrorNotSupported);
#endif
}

hipError_t hipIpcOpenEventHandle(hipEvent_t* event, hipIpcEventHandle_t handle) {
  HIP_INIT_API(hipIpcOpenEventHandle, event, handle);
#if !defined(_MSC_VER)
  hipError_t hip_err = hipSuccess;
  if (event == nullptr) {
    HIP_RETURN(hipErrorInvalidValue);
  }
  hip_err = ihipEventCreateWithFlags(event, hipEventDisableTiming | hipEventInterprocess);
  if (hip_err != hipSuccess) {
    HIP_RETURN(hip_err);
  }
  hip::Event* e = reinterpret_cast<hip::Event*>(*event);
  ihipIpcEventHandle_t* iHandle = reinterpret_cast<ihipIpcEventHandle_t*>(&handle);
  HIP_RETURN(e->OpenHandle(iHandle));
#else
  assert(0 && "Unimplemented");
  HIP_RETURN(hipErrorNotSupported);
#endif
}
