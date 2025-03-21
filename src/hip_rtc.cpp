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
#include "hiprtc_internal.hpp"
#include <hip/hiprtc.h>
#include "platform/program.hpp"

#ifdef __HIP_ENABLE_RTC
extern "C" {
extern const char __hipRTC_header[];
extern unsigned __hipRTC_header_size;
}
void __hipGetRTC(const char** rtc, unsigned int* size) {
  *rtc = __hipRTC_header;
  *size = __hipRTC_header_size;
}
#endif

namespace hiprtc {
thread_local hiprtcResult g_lastRtcError = HIPRTC_SUCCESS;
}

class ProgramState {
  amd::Monitor lock_;
private:
  static ProgramState* programState_;

  ProgramState() : lock_("Guards program state") {}
  ~ProgramState() {}
public:
  std::unordered_map<amd::Program*,
                     std::pair<std::vector<std::string>, std::vector<std::string>>> progHeaders_;

  std::map<std::string, std::pair<std::string, std::string>> nameExpresssion_;

  static ProgramState& instance();
  uint32_t addNameExpression(const char* name_expression);
  char* getLoweredName(const char* name_expression);
};

ProgramState* ProgramState::programState_ = nullptr;

ProgramState& ProgramState::instance() {
  if (programState_ == nullptr) {
    programState_ = new ProgramState;
  }
  return *programState_;
}

uint32_t ProgramState::addNameExpression(const char* name_expression) {
  amd::ScopedLock lock(lock_);

  // Strip clean of any '(' or ')' or '&'
  std::string strippedName(name_expression);
  if (strippedName.back() == ')') {
      strippedName.pop_back();
      strippedName.erase(0, strippedName.find('('));
  }
  if (strippedName.front() == '&') {
      strippedName.erase(0, 1);
  }
  auto it = nameExpresssion_.find(name_expression);
  if (it == nameExpresssion_.end()) {
    nameExpresssion_.insert(std::pair<std::string, std::pair<std::string, std::string>>
                            (name_expression, std::make_pair(strippedName,"")));
  }
  return nameExpresssion_.size();
}

static std::string handleMangledName(std::string loweredName) {
  if (loweredName.empty()) {
    return loweredName;
  }

  if (loweredName.find(".kd") != std::string::npos) {
    return {};
  }

  if (loweredName.find("void ") == 0) {
    loweredName.erase(0, strlen("void "));
  }

  auto dx{loweredName.find_first_of("(<")};

  if (dx == std::string::npos) {
    return loweredName;
  }

  if (loweredName[dx] == '<') {
    uint32_t count = 1;
    do {
        ++dx;
        count += (loweredName[dx] == '<') ? 1 : ((loweredName[dx] == '>') ? -1 : 0);
    } while (count);

    loweredName.erase(++dx);
  } else {
    loweredName.erase(dx);
  }

  return loweredName;
}

static std::string getValueOf(const std::string& option) {
  std::string res;
  auto f = std::find(option.begin(), option.end(), '=');
  if (f != option.end()) res = std::string(f + 1, option.end());
  return res;
}

static void transformOptions(std::vector<std::string>& options, amd::Program* program) {
  for (auto& i : options) {
    // This functionality is no longer supported - just consume at the moment and remove this in
    // couple of releases
    if (i == "-hip-pch") {
      i.clear();
      continue;
    }
    // Some rtc samples use --gpu-architecture
    if (i.rfind("--gpu-architecture=", 0) == 0) {
      auto val = getValueOf(i);
      i = "--offload-arch=" + val;
      continue;
    }
  }
}

const char* hiprtcGetErrorString(hiprtcResult x) {
  switch (x) {
    case HIPRTC_SUCCESS:
      return "HIPRTC_SUCCESS";
    case HIPRTC_ERROR_OUT_OF_MEMORY:
      return "HIPRTC_ERROR_OUT_OF_MEMORY";
    case HIPRTC_ERROR_PROGRAM_CREATION_FAILURE:
      return "HIPRTC_ERROR_PROGRAM_CREATION_FAILURE";
    case HIPRTC_ERROR_INVALID_INPUT:
      return "HIPRTC_ERROR_INVALID_INPUT";
    case HIPRTC_ERROR_INVALID_PROGRAM:
      return "HIPRTC_ERROR_INVALID_PROGRAM";
    case HIPRTC_ERROR_INVALID_OPTION:
      return "HIPRTC_ERROR_INVALID_OPTION";
    case HIPRTC_ERROR_COMPILATION:
      return "HIPRTC_ERROR_COMPILATION";
    case HIPRTC_ERROR_BUILTIN_OPERATION_FAILURE:
      return "HIPRTC_ERROR_BUILTIN_OPERATION_FAILURE";
    case HIPRTC_ERROR_NO_NAME_EXPRESSIONS_AFTER_COMPILATION:
      return "HIPRTC_ERROR_NO_NAME_EXPRESSIONS_AFTER_COMPILATION";
    case HIPRTC_ERROR_NO_LOWERED_NAMES_BEFORE_COMPILATION:
      return "HIPRTC_ERROR_NO_LOWERED_NAMES_BEFORE_COMPILATION";
    case HIPRTC_ERROR_NAME_EXPRESSION_NOT_VALID:
      return "HIPRTC_ERROR_NAME_EXPRESSION_NOT_VALID";
    case HIPRTC_ERROR_INTERNAL_ERROR:
      return "HIPRTC_ERROR_INTERNAL_ERROR";
    default:
      LogPrintfError("Invalid HIPRTC error code: %d \n", x);
      return nullptr;
  };

  ShouldNotReachHere();

  return nullptr;
}

hiprtcResult hiprtcCreateProgram(hiprtcProgram* prog, const char* src, const char* name,
                                 int numHeaders, const char** headers, const char** headerNames) {
  HIPRTC_INIT_API(prog, src, name, numHeaders, headers, headerNames);

  if (prog == nullptr) {
    HIPRTC_RETURN(HIPRTC_ERROR_INVALID_PROGRAM);
  }
  if (numHeaders < 0) {
    HIPRTC_RETURN(HIPRTC_ERROR_INVALID_INPUT);
  }
  if (numHeaders && (headers == nullptr || headerNames == nullptr)) {
    HIPRTC_RETURN(HIPRTC_ERROR_INVALID_INPUT);
  }

  // Add header from hiprtc_header.o
  std::vector<const char*> header_sources;
  std::vector<const char*> header_names;

  header_sources.reserve(numHeaders + 1);
  header_names.reserve(numHeaders + 1);

#ifdef __HIP_ENABLE_RTC
  const char* rtc_pch = nullptr;
  unsigned rtc_size = 0;
  __hipGetRTC(&rtc_pch, &rtc_size);
  if (rtc_pch == nullptr || rtc_size == 0) {
    HIPRTC_RETURN(HIPRTC_ERROR_BUILTIN_OPERATION_FAILURE);
  }
  std::string rtc_pch_str(rtc_pch, rtc_size);
  header_sources.push_back(rtc_pch_str.data());
  header_names.push_back("hiprtc_runtime.h");
#endif

  for (int i = 0; i < numHeaders; i++) {
    header_sources.push_back(headers[i]);
    header_names.push_back(headerNames[i]);
  }

  amd::Program* program =
      new amd::Program(*hip::getCurrentDevice()->asContext(), src, amd::Program::HIP,
                       header_sources.size(), header_sources.data(), header_names.data());
  if (program == NULL) {
    HIPRTC_RETURN(HIPRTC_ERROR_INVALID_INPUT);
  }

  if (CL_SUCCESS != program->addDeviceProgram(*hip::getCurrentDevice()->devices()[0])) {
    program->release();
    HIPRTC_RETURN(HIPRTC_ERROR_PROGRAM_CREATION_FAILURE);
  }

  *prog = reinterpret_cast<hiprtcProgram>(as_cl(program));

  HIPRTC_RETURN(HIPRTC_SUCCESS);
}

hiprtcResult hiprtcCompileProgram(hiprtcProgram prog, int numOptions, const char** options) {

  // FIXME[skudchad] Add headers to amd::Program::build and device::Program::build,
  // pass the saved from ProgramState to amd::Program::build
  HIPRTC_INIT_API(prog, numOptions, options);

  amd::Program* program = as_amd(reinterpret_cast<cl_program>(prog));

  std::ostringstream ostrstr;
  std::vector<std::string> oarr;
  oarr.reserve(numOptions + 12);

  const std::string hipVerOpt{"--hip-version=" + std::to_string(HIP_VERSION_MAJOR) + '.' +
                              std::to_string(HIP_VERSION_MINOR) + '.' +
                              std::to_string(HIP_VERSION_PATCH)};
  const std::string hipVerMajor{"-DHIP_VERSION_MAJOR=" + std::to_string(HIP_VERSION_MAJOR)};
  const std::string hipVerMinor{"-DHIP_VERSION_MINOR=" + std::to_string(HIP_VERSION_MINOR)};
  const std::string hipVerPatch{"-DHIP_VERSION_PATCH=" + std::to_string(HIP_VERSION_PATCH)};

  oarr.push_back(hipVerOpt);
  oarr.push_back(hipVerMajor);
  oarr.push_back(hipVerMinor);
  oarr.push_back(hipVerPatch);
#ifdef __HIP_ENABLE_RTC
  oarr.push_back("-D__HIPCC_RTC__");
  oarr.push_back("-include hiprtc_runtime.h");
  oarr.push_back("-std=c++14");
  oarr.push_back("-nogpuinc");
#ifdef _WIN32
  oarr.push_back("-target x86_64-pc-windows-msvc");
  oarr.push_back("-fms-extensions");
  oarr.push_back("-fms-compatibility");
#endif
#endif

  // Append the rest of options
  oarr.insert(oarr.end(), &options[0], &options[numOptions]);

  transformOptions(oarr, program);
  std::copy(oarr.begin(), oarr.end(), std::ostream_iterator<std::string>(ostrstr, " "));

  std::vector<amd::Device*> devices{hip::getCurrentDevice()->devices()[0]};
  if (CL_SUCCESS != program->build(devices, ostrstr.str().c_str(), nullptr, nullptr)) {
    HIPRTC_RETURN(HIPRTC_ERROR_COMPILATION);
  }

  HIPRTC_RETURN(HIPRTC_SUCCESS);
}

hiprtcResult hiprtcAddNameExpression(hiprtcProgram prog, const char* name_expression) {
  HIPRTC_INIT_API(prog, name_expression);

  if (name_expression == nullptr) {
    HIPRTC_RETURN(HIPRTC_ERROR_INVALID_INPUT);
  }
  amd::Program* program = as_amd(reinterpret_cast<cl_program>(prog));

  uint32_t id = ProgramState::instance().addNameExpression(name_expression);

  const auto var{"__hiprtc_" + std::to_string(id)};
  const auto code{"\nextern \"C\" constexpr auto " + var + " = " + name_expression + ';'};

  program->appendToSource(code.c_str());

  HIPRTC_RETURN(HIPRTC_SUCCESS);
}

hiprtcResult hiprtcGetLoweredName(hiprtcProgram prog, const char* name_expression,
                                  const char** loweredName) {
  HIPRTC_INIT_API(prog, name_expression, loweredName);

  if (name_expression == nullptr || loweredName == nullptr) {
     HIPRTC_RETURN(HIPRTC_ERROR_INVALID_INPUT);
  }

  amd::Program* program = as_amd(reinterpret_cast<cl_program>(prog));

  device::Program* dev_program
    = program->getDeviceProgram(*hip::getCurrentDevice()->devices()[0]);

  auto it = ProgramState::instance().nameExpresssion_.find(name_expression);
  if (it == ProgramState::instance().nameExpresssion_.end()) {
    return HIPRTC_RETURN(HIPRTC_ERROR_NAME_EXPRESSION_NOT_VALID);
  }

  std::string strippedName = it->second.first;
  std::string strippedNameNoSpace = strippedName;
  strippedNameNoSpace.erase(std::remove_if(strippedNameNoSpace.begin(),
                                           strippedNameNoSpace.end(),
                                           [](unsigned char c) {
                                             return std::isspace(c);
                                           }), strippedNameNoSpace.end());
  std::vector<std::string> mangledNames;

  if (!dev_program->getLoweredNames(&mangledNames)) {
    HIPRTC_RETURN(HIPRTC_ERROR_COMPILATION);
  }

  std::vector<std::string> demangledNames;
  for (const auto& name : mangledNames) {
    std::string demangledName;
    if (!dev_program->getDemangledName(name, demangledName)) {
      LogPrintfError("Unable to demangle name: %s \n", name.c_str());
      HIPRTC_RETURN(HIPRTC_ERROR_COMPILATION);
    }
    demangledNames.push_back(demangledName);
  }


  for(size_t i = 0; i < demangledNames.size(); i++) {
    std::string demangledName = handleMangledName(demangledNames[i]);
    demangledName.erase(std::remove_if(demangledName.begin(), demangledName.end(),
                                       [](unsigned char c) { return std::isspace(c); }),
                        demangledName.end());
    if (demangledName == strippedNameNoSpace) {
      it->second.second.assign(mangledNames[i]);
      break;
    }
  }

  // Return error if the added symbol is not in code
  if (it->second.second.size() == 0) {
    return HIPRTC_RETURN(HIPRTC_ERROR_NAME_EXPRESSION_NOT_VALID);
  }

  *loweredName = it->second.second.c_str();

  HIPRTC_RETURN(HIPRTC_SUCCESS);
}

hiprtcResult hiprtcDestroyProgram(hiprtcProgram* prog) {
  HIPRTC_INIT_API(prog);

  if (prog == NULL) {
     HIPRTC_RETURN(HIPRTC_ERROR_INVALID_INPUT);
  }

  // Release program. hiprtcProgram is a double pointer so free *prog
  amd::Program* program = as_amd(reinterpret_cast<cl_program>(*prog));

  program->release();

  HIPRTC_RETURN(HIPRTC_SUCCESS);
}

hiprtcResult hiprtcGetCode(hiprtcProgram prog, char* binaryMem) {
 HIPRTC_INIT_API(prog, binaryMem);


  amd::Program* program = as_amd(reinterpret_cast<cl_program>(prog));
  const device::Program::binary_t& binary =
      program->getDeviceProgram(*hip::getCurrentDevice()->devices()[0])->binary();

  ::memcpy(binaryMem, binary.first, binary.second);

  HIPRTC_RETURN(HIPRTC_SUCCESS);
}

hiprtcResult hiprtcGetCodeSize(hiprtcProgram prog, size_t* binarySizeRet) {

  HIPRTC_INIT_API(prog, binarySizeRet);

  amd::Program* program = as_amd(reinterpret_cast<cl_program>(prog));

  *binarySizeRet =
      program->getDeviceProgram(*hip::getCurrentDevice()->devices()[0])->binary().second;

  HIPRTC_RETURN(HIPRTC_SUCCESS);
}

hiprtcResult hiprtcGetProgramLog(hiprtcProgram prog, char* dst) {

  HIPRTC_INIT_API(prog, dst);
  amd::Program* program = as_amd(reinterpret_cast<cl_program>(prog));
  const device::Program* devProgram =
      program->getDeviceProgram(*hip::getCurrentDevice()->devices()[0]);

  auto log = program->programLog() + devProgram->buildLog().c_str();

  log.copy(dst, log.size());
  dst[log.size()] = '\0';

  HIPRTC_RETURN(HIPRTC_SUCCESS);
}

hiprtcResult hiprtcGetProgramLogSize(hiprtcProgram prog, size_t* logSizeRet) {

  HIPRTC_INIT_API(prog, logSizeRet);

  amd::Program* program = as_amd(reinterpret_cast<cl_program>(prog));
  const device::Program* devProgram =
      program->getDeviceProgram(*hip::getCurrentDevice()->devices()[0]);

  auto log = program->programLog() + devProgram->buildLog().c_str();

  *logSizeRet = log.size() + 1;

  HIPRTC_RETURN(HIPRTC_SUCCESS);
}

hiprtcResult hiprtcVersion(int* major, int* minor) {
  HIPRTC_INIT_API(major, minor);

  if (major == nullptr || minor == nullptr) {
    HIPRTC_RETURN(HIPRTC_ERROR_INVALID_INPUT);
  }

  *major = 9;
  *minor = 0;

  HIPRTC_RETURN(HIPRTC_SUCCESS);
}
