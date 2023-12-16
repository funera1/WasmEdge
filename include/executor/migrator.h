#pragma once

#include "ast/instruction.h"
#include "runtime/instance/module.h"
#include "runtime/instance/function.h"
#include "runtime/stackmgr.h"
#include "runtime/storemgr.h"
#include "executor/executor.h"

#include <map>
#include <iostream>
#include <vector>
#include <string>
#include <cassert>

namespace WasmEdge {
  
namespace Runtime {
  class StackManager;
}

namespace Executor {
struct SourceLoc {
  bool operator==(const SourceLoc& rhs) const {
    return (FuncIdx == rhs.FuncIdx && Offset == rhs.Offset);
  }

  uint32_t FuncIdx;
  uint32_t Offset;
};

class Migrator {
public:
  /// TODO: ModuleInstanceがnullだったときの名前。重複しないようにする
  const std::string NULL_MOD_NAME = "null";
  using IterMigratorType = std::map<AST::InstrView::iterator, SourceLoc>;

  /// ================
  /// Interface
  /// ================
  // const Runtime::Instance::FunctionInstance& restoreFuncInstance(const Runtime::Instance::FunctionInstance& Func) {
  //   Runtime::Instance::FunctionInstance* newFunc = const_cast<Runtime::Instance::FunctionInstance*>(&Func);

  //   const Runtime::Instance::ModuleInstance* restoredModInst = restoreModInst(Func.getModule());
  //   newFunc->setModule(restoredModInst);

  //   const Runtime::Instance::FunctionInstance* constFunc = const_cast<const Runtime::Instance::FunctionInstance*>(newFunc);
  //   return constFunc;
  // } 
  // 

  /// Find module by name.
  const Runtime::Instance::ModuleInstance *findModule(std::string_view Name) const {
    auto Iter = NamedMod.find(Name);
    if (likely(Iter != NamedMod.cend())) {
      return Iter->second;
    }
    return nullptr;
  }

  /// ================
  /// Tools
  /// ================
  void setIterMigrator(const Runtime::Instance::ModuleInstance* ModInst) {
    if (IterMigrators.count((std::string)ModInst->getModuleName())) {
      return;
    }

    IterMigratorType IterMigrator;

    for (uint32_t I = 0; I < ModInst->getFuncNum(); ++I) {
      Runtime::Instance::FunctionInstance* FuncInst = ModInst->getFunc(I).value();
      AST::InstrView Instr = FuncInst->getInstrs();
      AST::InstrView::iterator PC = Instr.begin();
      AST::InstrView::iterator PCEnd = Instr.end();
      
      uint32_t Offset = 0;
      while (PC != PCEnd) {
        IterMigrator[PC] = SourceLoc{I, Offset};
        Offset++;
        PC++;
      }
    }

    IterMigrators[(std::string)ModInst->getModuleName()] = IterMigrator;
  }
  
  IterMigratorType getIterMigrator(const Runtime::Instance::ModuleInstance* ModInst) {
    if (IterMigrators.count((std::string)ModInst->getModuleName())) {
      return IterMigrators[(std::string)ModInst->getModuleName()];
    }

    std::string_view ModName = ModInst->getModuleName();
    setIterMigrator(ModInst);
    return IterMigrators[(std::string)ModName];
  }

  IterMigratorType getIterMigratorByName(std::string ModName) {
    if (IterMigrators.count(ModName)) {
      return IterMigrators[ModName];
    } else {
      IterMigratorType I;
      return I;
    }
  }

  SourceLoc getSourceLoc(AST::InstrView::iterator Iter) {
    IterMigratorType IterMigrator = getIterMigratorByName(BaseModName);
    // assert(IterMigrator);

    struct SourceLoc Data = IterMigrator[Iter];
    return Data;
  }
  
  uint128_t stou128(std::string s) {
    uint128_t ret = 0;
    for (size_t i = 0; i < s.size(); i++) {
      if (s[i] < '0' || '9' < s[i]) {
        // std::cout << "value is number!!" << std::endl;
        assert(-1);
        return -1;
      }
      int n = (int)(s[i] - '0');
      ret = ret * 10 + n;
    }
    return ret;
  }
  
  // void debugFuncOpcode(uint32_t FuncIdx, uint32_t Offset, AST::InstrView::iterator it) {
  //   std::ofstream opcodeLog;
  //   opcodeLog.open("wasmedge_opcode.log", std::ios::app);
  //   opcodeLog << "fidx: " << FuncIdx << "\n";
  //   for (uint32_t i = 0; i < Offset; i++) it--;
  //   for (uint32_t i = 0; i < Offset; i++) {
  //     opcodeLog << i+1 << ") opcode: 0x" << std::hex << static_cast<uint16_t>(it->getOpCode()) << "\n";
  //     opcodeLog << std::dec;
  //     it++;
  //   }
  // }

  /// ================
  /// Dump functions for WAMR
  /// ================
  void dumpStack(Runtime::StackManager& StackMgr) {
    std::ofstream fout;
    // fout.open("wamr_stack.img", std::ios::out | std::ios::binary | std::ios::trunc);
    fout.open("wamr_stack.img", std::ios::out | std::ios::trunc);
    
    using Value = ValVariant;
    std::vector<Value> Vals = StackMgr.getValueStack();
    std::vector<uint8_t> Typs = StackMgr.getTypeStack();
    
    if (Vals.size() != Typs.size()) {
      std::cerr << "ValStack size != TypStack size" << std::endl;
      std::cerr << "ValStack size: " << Vals.size() << std::endl;
      std::cerr << "TypStack size: " << Typs.size() << std::endl;
      exit(1);
    }

    std::cout << "[DEBUG]TypeStack: [";
    for (size_t I = 0; I < Vals.size(); ++I) std::cout << +Typs[I];
    std::cout << "]" << std::endl;

    for (size_t I = 0; I < Vals.size(); ++I) {
      Value v = Vals[I];
      int t = Typs[I];
      // 32bitのとき
      fout << t << ", " << v.get<uint128_t>() << std::endl;
    }
    
    fout.close();
  }
  
  struct CtrlInfo {
    // AST::InstrView::iterator Iter;
    uint32_t BeginAddrOfs;
    uint32_t TargetAddrOfs;
    uint32_t ElseAddrOfs;
    uint32_t TspOfs;
    uint32_t SpOfs;
    uint32_t ResultCells;
    uint32_t ResultCount;
  };

  std::vector<struct CtrlInfo> getCtrlStack(const AST::InstrView::iterator PCNow, Runtime::Instance::FunctionInstance *Func, const std::vector<uint32_t> &WamrCellSums) {
    std::vector<struct CtrlInfo> CtrlStack;
    
    IterMigratorType IterMigrator = getIterMigratorByName(BaseModName);
    // assert(IterMigrator);

    AST::InstrView::iterator PCStart = Func->getInstrs().begin();
    AST::InstrView::iterator PCEnd = Func->getInstrs().end();
    AST::InstrView::iterator PC = PCStart;
    
    uint32_t BaseAddr = PCStart->getOffset();

    auto CtrlPush = [&](AST::InstrView::iterator Begin, AST::InstrView::iterator Target, uint32_t SpOfs, uint32_t TspOfs) {
      uint32_t BeginOfs = Begin->getOffset() - BaseAddr;
      uint32_t TargetOfs = Target->getOffset() - BaseAddr;
      uint32_t ElseOfs = (Begin + Begin->getJumpElse())->getOffset() - BaseAddr;
      CtrlStack.push_back({BeginOfs, TargetOfs, ElseOfs, SpOfs, TspOfs, 0, 0});
    };
    
    auto CtrlPop = [&]() {
      if (CtrlStack.size() == 0) {
        std::cerr << "CtrlStack is empty" << std::endl;
        return;
      }
      CtrlStack.pop_back();
    };
    
    // 関数ブロックを一番最初にpushする
    // ダミーブロックぽさがすこしあるので、適当に入れる（ちゃんとやると、target_addrに関数の一番最後のアドレスを入れる必要があり、無駄が増えるため）
    uint32_t SpOfs, TspOfs;
    CtrlPush(PCStart, PCEnd-1, 0, 0);

    // 命令をなめる
    while (PC < PCNow) {
      switch (PC->getOpCode()) {
        // push
        case OpCode::Block:
        case OpCode::If:
          SpOfs = WamrCellSums[PC->getJump().StackEraseBegin];
          TspOfs = PC->getJump().StackEraseBegin;
          CtrlPush(PC+1, PC+PC->getJumpEnd(), SpOfs, TspOfs);
          break;
        case OpCode::Loop:
          SpOfs = WamrCellSums[PC->getJump().StackEraseBegin];
          TspOfs = PC->getJump().StackEraseBegin;
          CtrlPush(PC+1, PC+1, SpOfs, TspOfs);
          break;

        // pop
        case OpCode::End:
          CtrlPop();
          break;
        default:
          break;
      }

      PC++;
    }
    
    return CtrlStack;
  }
  
  /// dumpするもの
  void dumpFrame(Runtime::StackManager &StackMgr) {
    using Value = ValVariant;

    std::vector<Runtime::StackManager::Frame> FrameStack = StackMgr.getFrameStack();
    std::vector<Value> ValueStack = StackMgr.getValueStack();
    std::vector<uint8_t> TypeStack = StackMgr.getTypeStack();

    // TypeStackからWAMRのセルの個数累積和みたいにする
    // 累積和 1-indexed
    std::vector<uint32_t> WamrCellSums(TypeStack.size()+1, 0);

    IterMigratorType IterMigrator = getIterMigratorByName(BaseModName);

    // WamrCelSumStackの初期化
    for (uint32_t I = 0; I < TypeStack.size(); I++) {
        WamrCellSums[I+1] = WamrCellSums[I] + (TypeStack[I] == 0 ? 1 : 2);
    }
    std::ofstream fout, csp_tsp_fout, tsp_fout, tsp2_fout;
    fout.open("wamr_frame.img", std::ios::trunc);
    csp_tsp_fout.open("ctrl_tsp.img", std::ios::trunc | std::ios::binary);
    tsp_fout.open("type_stack.img", std::ios::trunc | std::ios::binary);
    
    auto dump = [&](std::string desc, auto val, std::string end = "\n") {
      fout << desc << std::endl;
      fout << val << std::endl;
      fout << end;
    };
    
    // WAMRは4bytesセルが何個でカウントするので、それに合わせてOffsetをdumpする
    // [Start, End]の区間のcell_numを取得
    auto getStackOffset = [&](uint32_t Start, uint32_t End) {
      return WamrCellSums.at(End) - WamrCellSums.at(Start);
    };

    
    AST::InstrView::iterator StartIt;
    std::vector<struct CtrlInfo> CtrlStack;
    uint32_t CurrentIpOfs;
    uint32_t CurrentSpOfs;
    uint32_t CurrentCspOfs;
    uint32_t CurrentTspOfs;

    fout << "frame num" << std::endl;
    // I=1のとき飛ばすから-1
    fout << FrameStack.size()-1 << std::endl;
    fout << std::endl;

    // Frame Stackを走査
    for (size_t I = 0; I < FrameStack.size(); ++I) {
      // I = 1と2のときに__start()を持ってて重複してる。かつI=1の方はWAMRでは使わないので排除
      if (I == 1) continue;

      Runtime::StackManager::Frame pf = (I > 0 ? FrameStack[I-1] : FrameStack[0]);
      Runtime::StackManager::Frame f = FrameStack[I];
      const Runtime::Instance::ModuleInstance *ModInst = f.Module;


      // dummpy frame
      if (ModInst == nullptr) {
        dump("func_idx", -1);
        // TODO: dummy frameのall_cell_numいる
        // fout << all_cell_num << std::endl;
        dump("all_cell_num", 2, "");
      }
      else {
        // debug: 関数インデックスの出力
        auto Data = IterMigrator[f.From];

        StartIt = f.From - Data.Offset;
        auto Res = ModInst->getFunc(Data.FuncIdx);
        if (!Res) {
          std::cout << "FuncIdx is not correct" << std::endl;
        }
        Runtime::Instance::FunctionInstance* Func = Res.value();
        CtrlStack = getCtrlStack(f.From, Func, WamrCellSums);

        CurrentIpOfs = (f.From+1)->getOffset() - StartIt->getOffset();
        // func_idx
        dump("func_idx", Data.FuncIdx);
        // ip_offset
        dump("ip_offset", CurrentIpOfs);
        // sp_offset
        CurrentSpOfs = getStackOffset(pf.VPos + f.Locals, f.VPos);
        dump("sp_offset", CurrentSpOfs);
        // csp_offset
        CurrentCspOfs = CtrlStack.size();
        dump("csp_offset", CurrentCspOfs);
        // tsp offset
        CurrentTspOfs = TypeStack.size();
        // lp (params, locals)
        dump("lp_num", pf.Locals, "");
        fout << "lp(params, locals)" << std::endl;
        for (uint32_t I = pf.VPos-pf.Locals; I < pf.VPos; I++) {
          Value V = ValueStack[I];
          uint8_t T = TypeStack[I];

          if (T == 0) 
            fout << std::setw(32) << std::setfill('0') << V.get<uint32_t>() << std::endl;
          else  
            fout << std::setw(64) << std::setfill('0') << V.get<uint64_t>() << std::endl;
        }
        fout << std::endl;

        // value stack
        dump("stack_num", (f.VPos-f.Locals) - pf.VPos, "");

        fout << "stack" << std::endl;
        for (uint32_t I = pf.VPos; I < f.VPos-f.Locals; I++) {
          Value V = ValueStack[I];
          uint32_t T = static_cast<uint32_t>(TypeStack[I]);

          if (T == 0) 
            fout << std::setw(32) << std::setfill('0') << V.get<uint32_t>() << std::endl;
          else  
            fout << std::setw(64) << std::setfill('0') << V.get<uint64_t>() << std::endl;
          
          // type stack
          tsp_fout.write(reinterpret_cast<char *>(&T), sizeof(T));
        }
        fout << std::endl;
        
        // for csp_num
        dump("csp_num", CtrlStack.size(), "");

        fout << "csp" << std::endl;
        for (uint32_t I = 0; I < CtrlStack.size(); I++) {
          struct CtrlInfo info = CtrlStack[I];

          // cps->begin_addr_offset
          fout << info.BeginAddrOfs << std::endl;
          // cps->target_addr_offset
          fout << info.TargetAddrOfs << std::endl;
          // csp->frame_sp_offset
          fout << info.SpOfs << std::endl;
          // csp->frame_tsp_offset
          csp_tsp_fout.write(reinterpret_cast<char *>(&info.TspOfs), sizeof(info.TspOfs));
          // csp->cell_num
          fout << info.ResultCells << std::endl;
          // csp->count
          csp_tsp_fout.write(reinterpret_cast<char *>(&info.ResultCount), sizeof(info.ResultCount));
        }
      }
      fout << "===" << std::endl;
    }
    fout.close();


    /// Addr.img
    std::ofstream tsp_addr_fout;
    fout.open("wamr_addrs.img", std::ios::trunc);
    tsp_addr_fout.open("tsp_addr.img", std::ios::trunc | std::ios::binary);

    // sp_ofs
    dump("sp_ofs", CurrentSpOfs);
    // csp_ofs
    dump("csp_ofs", CurrentCspOfs);
    // tsp_ofs
    tsp_addr_fout.write(reinterpret_cast<char *>(&CurrentTspOfs), sizeof(CurrentTspOfs));

    struct CtrlInfo CtrlTop = CtrlStack.back();
    // else_addr
    dump("else_addr", CtrlTop.ElseAddrOfs);
    // end_addr
    dump("end_addr", CtrlTop.TargetAddrOfs);
    // done flag
    dump("done_flag", 1);
    
    fout.close();
    csp_tsp_fout.close();
    tsp_fout.close();
    tsp_addr_fout.close();
  }

  /// ================
  /// Dump functions
  /// ================
  /// TODO: 関数名を中身にあったものにrenameする
  void preDumpIter(const Runtime::Instance::ModuleInstance* ModInst) {
    setIterMigrator(ModInst);
    BaseModName = ModInst->getModuleName();
  }

  Expect<void> dumpProgramCounter(const Runtime::Instance::ModuleInstance* ModInst, AST::InstrView::iterator Iter) {
    IterMigratorType IterMigrator = getIterMigratorByName(BaseModName);
    // assert(IterMigrator);

    struct SourceLoc Data = IterMigrator[Iter];
    std::ofstream ofs("program_counter.img", std::ios::trunc | std::ios::binary);
    if (!ofs) {
      return Unexpect(ErrCode::Value::IllegalPath);
    }

    auto Res = ModInst->getFunc(Data.FuncIdx);
    if (unlikely(!Res)) {
      return Unexpect(Res);
    }
    Runtime::Instance::FunctionInstance* FuncInst = Res.value();
    AST::InstrView::iterator PCStart = FuncInst->getInstrs().begin();


    uint32_t Offset = Iter->getOffset() - PCStart->getOffset();
    ofs.write(reinterpret_cast<char *>(&Data.FuncIdx), sizeof(uint32_t));
    ofs.write(reinterpret_cast<char *>(&Offset), sizeof(uint32_t));

    ofs.close();
    return {};
  }
  
  void dumpStackMgrFrame(Runtime::StackManager& StackMgr, std::string fname_header = "") {
    std::vector<Runtime::StackManager::Frame> FrameStack = StackMgr.getFrameStack();
    std::ofstream FrameStream;
    FrameStream.open(fname_header + "stackmgr_frame.img", std::ios::trunc);

    std::map<std::string_view, bool> seenModInst;
    for (size_t I = 0; I < FrameStack.size(); ++I) {
      Runtime::StackManager::Frame f = FrameStack[I];

      // ModuleInstance
      const Runtime::Instance::ModuleInstance* ModInst = f.Module;

      // ModInstがnullの場合は、ModNameだけ出力して、continue
      if (ModInst == nullptr) {
        FrameStream << NULL_MOD_NAME << std::endl;
        FrameStream << std::endl; 
        continue; 
      }

      std::string_view ModName = ModInst->getModuleName();
      FrameStream << ModName << std::endl;

      // まだそのModInstを保存してなければ、dumpする
      if(!seenModInst[ModName]) {
        ModInst->dumpMemInst(fname_header + std::string(ModName));
        ModInst->dumpGlobInst(fname_header + std::string(ModName));
        seenModInst[ModName] = true;
      }
      
      // Iterator
      IterMigratorType IterMigrator = getIterMigrator(ModInst);
      struct SourceLoc Data = IterMigrator[const_cast<AST::InstrView::iterator>(f.From)];

      FrameStream << Data.FuncIdx << std::endl;
      FrameStream << Data.Offset << std::endl;

      // Locals, VPos, Arity
      FrameStream << f.Locals << std::endl;
      FrameStream << f.VPos << std::endl;
      FrameStream << f.Arity << std::endl;
      FrameStream << std::endl; 
    }  
    
    FrameStream.close();
  }
  
  void dumpStackMgrValue(Runtime::StackManager& StackMgr, std::string fname_header = "") {
    std::ofstream ValueStream;
    ValueStream.open(fname_header + "stackmgr_value.img", std::ios::trunc);

    using Value = ValVariant;
    std::vector<Value> ValueStack = StackMgr.getValueStack();
    for (size_t I = 0; I < ValueStack.size(); ++I) {
      Value v = ValueStack[I];
      ValueStream << v.get<uint128_t>() << std::endl;
    }
    
    ValueStream.close();
  }
  
  /// ================
  /// Restore functions
  /// ================
  Expect<AST::InstrView::iterator> _restoreIter(const Runtime::Instance::ModuleInstance* ModInst, uint32_t FuncIdx, uint32_t Offset) {
    assert(ModInst != nullptr);
    
    auto Res = ModInst->getFunc(FuncIdx);
    if (unlikely(!Res)) {
      // spdlog::error(ErrInfo::InfoAST(ASTNodeAttr::Seg_Element));
      std::cout << "\x1b[31m";
      std::cout << "ERROR: _restoreIter" << std::endl;
      std::cout << "\x1b[1m";
      return Unexpect(Res);
    }
    Runtime::Instance::FunctionInstance* FuncInst = Res.value();
    assert(FuncInst != nullptr);

    AST::InstrView::iterator Iter = FuncInst->getInstrs().begin();
    assert(Iter != nullptr);

    Iter += Offset;

    return Iter;
  }

  // 命令と引数が混在したOffsetの復元
  Expect<AST::InstrView::iterator> _restorePC(const Runtime::Instance::ModuleInstance* ModInst, uint32_t FuncIdx, uint32_t Offset) {
    assert(ModInst != nullptr);
    
    auto Res = ModInst->getFunc(FuncIdx);
    if (unlikely(!Res)) {
      // spdlog::error(ErrInfo::InfoAST(ASTNodeAttr::Seg_Element));
      return Unexpect(Res);
    }
    Runtime::Instance::FunctionInstance* FuncInst = Res.value();
    assert(FuncInst != nullptr);

    AST::InstrView::iterator PC = FuncInst->getInstrs().begin();
    assert(PC != nullptr);

    // 与えられたOffsetは関数の先頭からの相対オフセットなので、先頭アドレス分を足す
    Offset += PC->getOffset();

    uint32_t PCOfs = 0;
    while (PCOfs < Offset) {
      PCOfs = PC->getOffset();
      if (PCOfs == Offset) {
        return PC;
      }
      PC++;
    }

    // ここまで来たらエラー
    std::cerr << "[WARN] The offset of program_counter.img is incorrect" << std::endl;
    return Unexpect(ErrCode::Value::Terminated);
  }

  Expect<AST::InstrView::iterator> restoreProgramCounter(const Runtime::Instance::ModuleInstance* ModInst) {
    std::ifstream ifs("program_counter.img", std::ios::binary);

    uint32_t FuncIdx, Offset;
    ifs.read(reinterpret_cast<char *>(&FuncIdx), sizeof(uint32_t));
    ifs.read(reinterpret_cast<char *>(&Offset), sizeof(uint32_t));

    ifs.close();

    auto Res = _restorePC(ModInst, FuncIdx, Offset);
    return Res;
  }
  
  Expect<std::vector<Runtime::StackManager::Frame>> restoreStackMgrFrame() {
    std::ifstream FrameStream;
    FrameStream.open("stackmgr_frame.img");
    Runtime::StackManager StackMgr;

    std::vector<Runtime::StackManager::Frame> FrameStack;
    FrameStack.reserve(16U);
    std::string FrameString;
    /// TODO: ループ条件見直す
    std::map<std::string, const Runtime::Instance::ModuleInstance*> ModCache;
    while(getline(FrameStream, FrameString)) {
      // ModuleInstance
      std::string ModName = FrameString;
      const Runtime::Instance::ModuleInstance* ModInst;

      // ModInstがnullの場合
      if (ModName == NULL_MOD_NAME) {
        Runtime::StackManager::Frame f(nullptr, nullptr, 0, 0, 0);
        FrameStack.push_back(f);

        // 次の行がなければ終了
        if(!getline(FrameStream, FrameString)) {
          break;
        }
        continue;
      }

      // ModInstがnullじゃない場合
      ModInst = findModule(ModName);
      if (ModInst == nullptr) {
        assert(-1);
      }

      /// TODO: 同じModuleの復元をしないよう、キャッシュを作る
      if (ModCache.count(ModName) == 0) {
        ModInst->restoreMemInst(std::string(ModName));
        ModInst->restoreGlobInst(std::string(ModName));
        ModCache[ModName] = ModInst;
      }
      else {
        ModInst = ModCache[ModName];
      }

      // Iterator
      getline(FrameStream, FrameString);
      uint32_t FuncIdx = static_cast<uint32_t>(std::stoul(FrameString));
      getline(FrameStream, FrameString);
      uint32_t Offset = static_cast<uint32_t>(std::stoul(FrameString));
      auto Res = _restoreIter(ModInst, FuncIdx, Offset);
      if (!Res) {
        return Unexpect(Res);
      }
      AST::InstrView::iterator From = Res.value();
      // AST::InstrView::iterator From = _restoreIter(ModInst, FuncIdx, Offset).value();

      // Locals, VPos, Arity
      getline(FrameStream, FrameString);
      uint32_t Locals = static_cast<uint32_t>(std::stoul(FrameString));
      getline(FrameStream, FrameString);
      uint32_t VPos = static_cast<uint32_t>(std::stoul(FrameString));
      getline(FrameStream, FrameString);
      uint32_t Arity = static_cast<uint32_t>(std::stoul(FrameString));

      Runtime::StackManager::Frame f(ModInst, From, Locals, Arity, VPos);
      FrameStack.push_back(f);

      // 空の行を読み捨て
      getline(FrameStream, FrameString);
    }

    FrameStream.close();
    return FrameStack;
  }
  
  Expect<std::vector<Runtime::StackManager::Value>> restoreStackMgrValue() {	  // Runtime::StackManager restoreStackMgr() {
    std::ifstream ValueStream;	  // }
    ValueStream.open("stackmgr_value.img");	
    Runtime::StackManager StackMgr;	

    std::vector<Runtime::StackManager::Value> ValueStack;	
    ValueStack.reserve(2048U);
    std::string ValueString;	
    /// TODO: ループ条件見直す	
    while(getline(ValueStream, ValueString)) {	
      // ValueStringが空の場合はエラー	
      assert(ValueString.size() > 0);	

      /// TODO: stoullは64bitまでしか受け取らないので、128bitの入力が来たら壊れる
      Runtime::StackManager::Value v = static_cast<uint128_t>(stou128(ValueString));	
      ValueStack.push_back(v);	
    }	

    ValueStream.close();	
    return ValueStack;    	
  }

  Expect<Runtime::StackManager> restoreStackMgr() {
    std::vector<Runtime::StackManager::Frame> fs = restoreStackMgrFrame().value();
    std::cout << "Success to restore stack frame" << std::endl;

    std::vector<Runtime::StackManager::Value> vs = restoreStackMgrValue().value();
    std::cout << "Success to restore stack value" << std::endl;

    Runtime::StackManager StackMgr;
    StackMgr.setFrameStack(fs);
    StackMgr.setValueStack(vs);
    std::cout << "Success to restore stack manager" << std::endl;

    return StackMgr;
  }

  bool DumpFlag; 
  bool RestoreFlag;
private:
  friend class Executor;

  std::map<std::string, IterMigratorType> IterMigrators;
  std::string BaseModName;
  /// \name Module name mapping.
  std::map<std::string, const Runtime::Instance::ModuleInstance *, std::less<>> NamedMod;
};

} // namespace Runtime
} // namespace WasmEdge
