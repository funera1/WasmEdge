#include "vm/vm.h"
#include "executor/hostfunc.h"
#include "loader/loader.h"
#include "vm/result.h"

/// EEI Functions
#include "vm/hostfunc/ethereum/calldatacopy.h"
#include "vm/hostfunc/ethereum/callstatic.h"
#include "vm/hostfunc/ethereum/finish.h"
#include "vm/hostfunc/ethereum/getcalldatasize.h"
#include "vm/hostfunc/ethereum/getcaller.h"
#include "vm/hostfunc/ethereum/returndatacopy.h"
#include "vm/hostfunc/ethereum/revert.h"
#include "vm/hostfunc/ethereum/storageload.h"
#include "vm/hostfunc/ethereum/storagestore.h"

/// Wasi Functions
#include "vm/hostfunc/wasi/args_Get.h"
#include "vm/hostfunc/wasi/args_SizesGet.h"
#include "vm/hostfunc/wasi/environ_Get.h"
#include "vm/hostfunc/wasi/environ_SizesGet.h"
#include "vm/hostfunc/wasi/fd_Close.h"
#include "vm/hostfunc/wasi/fd_FdstatGet.h"
#include "vm/hostfunc/wasi/fd_FdstatSetFlags.h"
#include "vm/hostfunc/wasi/fd_PrestatDirName.h"
#include "vm/hostfunc/wasi/fd_PrestatGet.h"
#include "vm/hostfunc/wasi/fd_Read.h"
#include "vm/hostfunc/wasi/fd_Seek.h"
#include "vm/hostfunc/wasi/fd_Write.h"
#include "vm/hostfunc/wasi/path_Open.h"
#include "vm/hostfunc/wasi/proc_Exit.h"

#include <stdio.h>

namespace SSVM {
namespace VM {

namespace detail {

template <typename T> bool testAndSetError(T Status, Result &VMResult) {
  if (Status != T::Success) {
    VMResult.setErrCode(static_cast<unsigned int>(Status));
    return true;
  }
  return false;
}

} // namespace detail

VM::VM(Configure &InputConfig) : Config(InputConfig) {
  Configure::VMType Type = Config.getVMType();
  switch (Type) {
  case Configure::VMType::Ewasm:
    Env = std::make_unique<EVMEnvironment>();
    break;
  case Configure::VMType::Wasi:
    Env = std::make_unique<WasiEnvironment>();
    break;
  default:
    Env.reset();
    break;
  }
}

ErrCode VM::setPath(const std::string &FilePath) {
  WasmPath = FilePath;
  return ErrCode::Success;
}

ErrCode VM::execute() {
  /// Prepare VM according to VM type.
  prepareVMHost();

  /// Run code.
  ErrCode Status = runLoader();
  if (Status == ErrCode::Success) {
    Status = runExecutor();
  }

  /// Clear loader and executor engine.
  LoaderEngine.reset();
  ExecutorEngine.reset();
  Mod.reset();
  Args.clear();
  return Status;
}

Environment *VM::getEnvironment() { return Env.get(); }

ErrCode VM::runLoader() {
  Loader::ErrCode LoaderStatus = Loader::ErrCode::Success;
  VMResult.setStage(Result::Stage::Loader);

  LoaderStatus = LoaderEngine.setPath(WasmPath);
  if (detail::testAndSetError(LoaderStatus, VMResult)) {
    return ErrCode::Failed;
  }
  LoaderStatus = LoaderEngine.parseModule();
  if (detail::testAndSetError(LoaderStatus, VMResult)) {
    return ErrCode::Failed;
  }
  LoaderStatus = LoaderEngine.validateModule();
  if (detail::testAndSetError(LoaderStatus, VMResult)) {
    return ErrCode::Failed;
  }
  LoaderStatus = LoaderEngine.getModule(Mod);
  if (detail::testAndSetError(LoaderStatus, VMResult)) {
    return ErrCode::Failed;
  }

  if (VMResult.hasError()) {
    return ErrCode::Failed;
  }
  return ErrCode::Success;
}

ErrCode VM::runExecutor() {
  Executor::ErrCode ExecutorStatus = Executor::ErrCode::Success;
  VMResult.setStage(Result::Stage::Executor);

  ExecutorEngine.setStartFuncName(Config.getStartFuncName());
  ExecutorStatus = ExecutorEngine.setModule(Mod);
  if (detail::testAndSetError(ExecutorStatus, VMResult)) {
    return ErrCode::Failed;
  }

  ExecutorStatus = ExecutorEngine.instantiate();
  if (detail::testAndSetError(ExecutorStatus, VMResult)) {
    return ErrCode::Failed;
  }

  ExecutorStatus = ExecutorEngine.setArgs(Args);
  if (detail::testAndSetError(ExecutorStatus, VMResult)) {
    return ErrCode::Failed;
  }

  ExecutorStatus = ExecutorEngine.run();
  if (detail::testAndSetError(ExecutorStatus, VMResult)) {
    return ErrCode::Failed;
  }

  if (VMResult.hasError()) {
    return ErrCode::Failed;
  }
  return ErrCode::Success;
}

ErrCode VM::prepareVMHost() {
  ErrCode Status = ErrCode::Success;
  Configure::VMType Type = Config.getVMType();
  if (Type == Configure::VMType::Ewasm) {
    /// Ewasm case, insert EEI host functions.
    EVMEnvironment *EVMEnv = dynamic_cast<EVMEnvironment *>(Env.get());
    auto FuncEEICallDataCopy =
        std::make_unique<Executor::EEICallDataCopy>(*EVMEnv);
    auto FuncEEICallStatic = std::make_unique<Executor::EEICallStatic>(*EVMEnv);
    auto FuncEEIFinish = std::make_unique<Executor::EEIFinish>(*EVMEnv);
    auto FuncEEIGetCallDataSize =
        std::make_unique<Executor::EEIGetCallDataSize>(*EVMEnv);
    auto FuncEEIGetCaller = std::make_unique<Executor::EEIGetCaller>(*EVMEnv);
    auto FuncEEIReturnDataCopy =
        std::make_unique<Executor::EEIReturnDataCopy>(*EVMEnv);
    auto FuncEEIRevert = std::make_unique<Executor::EEIRevert>(*EVMEnv);
    auto FuncEEIStorageLoad =
        std::make_unique<Executor::EEIStorageLoad>(*EVMEnv);
    auto FuncEEIStorageStore =
        std::make_unique<Executor::EEIStorageStore>(*EVMEnv);

    if (Status == ErrCode::Success) {
      Status = setHostFunction(FuncEEICallDataCopy, "ethereum", "callDataCopy");
    }
    if (Status == ErrCode::Success) {
      Status = setHostFunction(FuncEEICallStatic, "ethereum", "callStatic");
    }
    if (Status == ErrCode::Success) {
      Status = setHostFunction(FuncEEIFinish, "ethereum", "finish");
    }
    if (Status == ErrCode::Success) {
      Status = setHostFunction(FuncEEIGetCallDataSize, "ethereum",
                               "getCallDataSize");
    }
    if (Status == ErrCode::Success) {
      Status = setHostFunction(FuncEEIGetCaller, "ethereum", "getCaller");
    }
    if (Status == ErrCode::Success) {
      Status =
          setHostFunction(FuncEEIReturnDataCopy, "ethereum", "returnDataCopy");
    }
    if (Status == ErrCode::Success) {
      Status = setHostFunction(FuncEEIRevert, "ethereum", "revert");
    }
    if (Status == ErrCode::Success) {
      Status = setHostFunction(FuncEEIStorageLoad, "ethereum", "storageLoad");
    }
    if (Status == ErrCode::Success) {
      Status = setHostFunction(FuncEEIStorageStore, "ethereum", "storageStore");
    }
  } else if (Type == Configure::VMType::Wasi) {
    /// Wasi case, insert Wasi host functions.
    WasiEnvironment *WasiEnv = dynamic_cast<WasiEnvironment *>(Env.get());
    auto FuncWasiArgsGet = std::make_unique<Executor::WasiArgsGet>(*WasiEnv);
    auto FuncWasiArgsSizesGet =
        std::make_unique<Executor::WasiArgsSizesGet>(*WasiEnv);
    auto FuncWasiEnvironGet =
        std::make_unique<Executor::WasiEnvironGet>(*WasiEnv);
    auto FuncWasiEnvironSizesGet =
        std::make_unique<Executor::WasiEnvironSizesGet>(*WasiEnv);
    auto FuncWasiFdClose = std::make_unique<Executor::WasiFdClose>(*WasiEnv);
    auto FuncWasiFdFdstatGet =
        std::make_unique<Executor::WasiFdFdstatGet>(*WasiEnv);
    auto FuncWasiFdFdstatSetFlags =
        std::make_unique<Executor::WasiFdFdstatSetFlags>(*WasiEnv);
    auto FuncWasiFdPrestatDirName =
        std::make_unique<Executor::WasiFdPrestatDirName>(*WasiEnv);
    auto FuncWasiFdPrestatGet =
        std::make_unique<Executor::WasiFdPrestatGet>(*WasiEnv);
    auto FuncWasiFdRead = std::make_unique<Executor::WasiFdRead>(*WasiEnv);
    auto FuncWasiFdSeek = std::make_unique<Executor::WasiFdSeek>(*WasiEnv);
    auto FuncWasiFdWrite = std::make_unique<Executor::WasiFdWrite>(*WasiEnv);
    auto FuncWasiPathOpen = std::make_unique<Executor::WasiPathOpen>(*WasiEnv);
    auto FuncWasiProcExit = std::make_unique<Executor::WasiProcExit>(*WasiEnv);

    if (Status == ErrCode::Success) {
      Status = setHostFunction(FuncWasiArgsGet, "wasi_unstable", "args_get");
    }
    if (Status == ErrCode::Success) {
      Status = setHostFunction(FuncWasiArgsSizesGet, "wasi_unstable",
                               "args_sizes_get");
    }
    if (Status == ErrCode::Success) {
      Status =
          setHostFunction(FuncWasiEnvironGet, "wasi_unstable", "environ_get");
    }
    if (Status == ErrCode::Success) {
      Status = setHostFunction(FuncWasiEnvironSizesGet, "wasi_unstable",
                               "environ_sizes_get");
    }
    if (Status == ErrCode::Success) {
      Status = setHostFunction(FuncWasiFdClose, "wasi_unstable", "fd_close");
    }
    if (Status == ErrCode::Success) {
      Status = setHostFunction(FuncWasiFdFdstatGet, "wasi_unstable",
                               "fd_fdstat_get");
    }
    if (Status == ErrCode::Success) {
      Status = setHostFunction(FuncWasiFdFdstatSetFlags, "wasi_unstable",
                               "fd_fdstat_set_flags");
    }
    if (Status == ErrCode::Success) {
      Status = setHostFunction(FuncWasiFdPrestatDirName, "wasi_unstable",
                               "fd_prestat_dir_name");
    }
    if (Status == ErrCode::Success) {
      Status = setHostFunction(FuncWasiFdPrestatGet, "wasi_unstable",
                               "fd_prestat_get");
    }
    if (Status == ErrCode::Success) {
      Status = setHostFunction(FuncWasiFdRead, "wasi_unstable", "fd_read");
    }
    if (Status == ErrCode::Success) {
      Status = setHostFunction(FuncWasiFdSeek, "wasi_unstable", "fd_seek");
    }
    if (Status == ErrCode::Success) {
      Status = setHostFunction(FuncWasiFdWrite, "wasi_unstable", "fd_write");
    }
    if (Status == ErrCode::Success) {
      Status = setHostFunction(FuncWasiPathOpen, "wasi_unstable", "path_open");
    }
    if (Status == ErrCode::Success) {
      Status = setHostFunction(FuncWasiProcExit, "wasi_unstable", "proc_exit");
    }
  }
  return Status;
}

} // namespace VM
} // namespace SSVM