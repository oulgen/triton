#include "triton/Dialect/TritonGPU/IR/Dialect.h"
#include "triton/Dialect/TritonGPU/Transforms/Passes.h"

#include "mlir/IR/BlockAndValueMapping.h"
#include <llvm-6.0/llvm/Support/ErrorHandling.h>
#include <llvm-6.0/llvm/Support/raw_ostream.h>

//===----------------------------------------------------------------------===//
//
// This file implements loop software pipelining
// The implementation here is inspired by the pipeline pass in Triton (-v2.0) 
// and SCF's LoopPipelining.
//
//===----------------------------------------------------------------------===//


using namespace mlir;

#define GEN_PASS_CLASSES
#include "triton/Dialect/TritonGPU/Transforms/Passes.h.inc"

namespace {
class LoopPipeliner {
  /// comments on numStages:
  ///   [0, numStages-1) are in the prologue
  ///   numStages-1 is appended after the loop body
  int numStages;

  /// cache forOp we are working on
  scf::ForOp forOp;

  /// cahce YieldOp for this forOp
  scf::YieldOp yieldOp;

  /// loads to be pipelined
  SetVector<Value> loads;

  /// value (in loop) => value at stage N
  DenseMap<Value, SmallVector<Value>> valueMapping;

  /// Block arguments that loads depend on
  DenseSet<BlockArgument> depArgs;
  /// Operations (inside the loop body) that loads depend on
  DenseSet<Operation*> depOps;

  /// collect values that v depends on and are defined inside the loop
  void collectDeps(Value v, int stages, DenseSet<Value> &deps);

  void setValueMapping(Value origin, Value newValue, int stage);
public:
  LoopPipeliner(scf::ForOp forOp, int numStages) 
      : forOp(forOp), numStages(numStages) {
    // cache yieldOp
    yieldOp = cast<scf::YieldOp>(forOp.getBody()->getTerminator());
  }

  /// Collect loads to pipeline. Return success if we can pipeline this loop
  LogicalResult initialize();

  /// emit pipelined loads (before loop body)
  void emitPrologue();

  /// create the new ForOp (add new args & insert prefetched ops)
  scf::ForOp createNewForOp();

  friend class PipelinePass;
};

// helpers
void LoopPipeliner::setValueMapping(Value origin, Value newValue, int stage) {
  if (valueMapping.find(origin) == valueMapping.end())
    valueMapping[origin] = SmallVector<Value>(numStages);
  valueMapping[origin][stage] = newValue;
}

void LoopPipeliner::collectDeps(Value v, int stages, DenseSet<Value> &deps) {
  // Loop-invarant value. skip
  if (v.getParentRegion() != &forOp.getLoopBody())
    return;

  // Since we only need to peel the loop numStages-1 times, don't worry about
  // depends that are too far away
  if (stages < 0)
    return;

  if (auto arg = v.dyn_cast<BlockArgument>()) {
    deps.insert(v);
    // Note: we have iv as the first arg, so the op idx is arg.getArgNumber()-1
    collectDeps(yieldOp->getOperand(arg.getArgNumber() - 1), stages-1, deps);
  } else { // value
    // v might be in deps, but we still need to visit v.
    // This is because v might depends on value in previous iterations
    deps.insert(v);
    for (Value op : v.getDefiningOp()->getOperands())
      collectDeps(op, stages, deps);
  }
}

/// A load instruction can be pipelined if:
///   - the load doesn't depend on any other loads (after loop peeling)
///   - (?) this load is not a loop-invariant value (we should run LICM before
///                                                  this pass?)
LogicalResult LoopPipeliner::initialize() {
  Block *loop = forOp.getBody();

  // can we use forOp.walk(...) here?
  SmallVector<triton::LoadOp, 2> allLoads;
  for (Operation &op : *loop)
    if (auto loadOp = dyn_cast<triton::LoadOp>(&op))
      allLoads.push_back(loadOp);

  // Early stop: no need to continue if there is no load in the loop.
  if (allLoads.empty())
    return failure();

  // load => values that it depends on
  DenseMap<Value, DenseSet<Value>> loadDeps;
  for (triton::LoadOp loadOp : allLoads) {
    DenseSet<Value> deps;
    for (Value op : loadOp->getOperands())
      collectDeps(op, numStages - 1, deps);
    loadDeps[loadOp] = deps;
  }

  // for (triton::LoadOp loadOp : allLoads) {
  //   llvm::errs() << loadOp << " depends on: #" << loadDeps[loadOp].size() << " values\n";
  //   for (Value dep : loadDeps[loadOp])
  //     llvm::errs() << dep << "\n";
  //   llvm::errs() << "\n";
  // }

  // Don't pipeline loads that depend on other loads
  // (Because if a load depends on another load, this load needs to wait on the
  //  other load in the prologue, which is against the point of the pipeline
  //  pass)
  for (triton::LoadOp loadOp : allLoads) {
    bool isCandiate = true;
    for (triton::LoadOp other : allLoads) {
      if (loadDeps[loadOp].contains(other)) {
        isCandiate = false;
        break;
      }
    }
    if (isCandiate)
      loads.insert(loadOp);
  }


  // we have some loads to pipeline
  if (!loads.empty()) {
    // update depArgs & depOps
    for (Value loadOp : loads) {
      for (Value dep : loadDeps[loadOp]) {
        // TODO: we should record the stage that the value is depended on
        if (auto arg = dep.dyn_cast<BlockArgument>())
          depArgs.insert(arg);
        else
          depOps.insert(dep.getDefiningOp());
      }
    }
    return success();
  }

  // llvm::errs() << allLoads.size() << " loads inside the loop\n"
  //              << loads.size() << " loads to be pipelined\n";

  return failure();
}

void LoopPipeliner::emitPrologue() {
  // llvm::errs() << "to pipeline...\n";
  // for (Value load : loads)
  //   llvm::errs() << load << "\n";

  // TODO: should we use rewriter here?
  OpBuilder builder(forOp);
  for (BlockArgument &arg : forOp.getRegionIterArgs()) {
    OpOperand &operand = forOp.getOpOperandForRegionIterArg(arg);
    setValueMapping(arg, operand.get(), 0);
  }

  // prologue from [0, numStage-1)
  Value iv = forOp.getLowerBound();
  for (int stage = 0; stage < numStages - 1; ++stage) {
    // special handling for induction variable as the increment is implicit
    if (stage != 0)
      iv = builder.create<arith::AddIOp>(iv.getLoc(), iv, forOp.getStep());
    setValueMapping(forOp.getInductionVar(), iv, stage);

    // special handling for loop condition as there is no condition in ForOp
    Value loopCond = builder.create<arith::CmpIOp>(
      iv.getLoc(), arith::CmpIPredicate::slt, iv, forOp.getUpperBound());

    // rematerialize peeled values
    SmallVector<Operation*> orderedDeps;
    for (Operation &op : forOp.getLoopBody().front()) {
      if (depOps.contains(&op))
        orderedDeps.push_back(&op);
      else if (loads.contains(op.getResult(0)))
        orderedDeps.push_back(&op);
    }
    assert(depOps.size() + loads.size() == orderedDeps.size() &&
           "depOps contains invalid values");
    for (Operation *op : orderedDeps) {
      Operation *newOp = nullptr;
      if (loads.contains(op->getResult(0))) {
        // load => copy async
        // TODO: check if the hardware supports copyasync
        if (auto loadOp = llvm::dyn_cast<triton::LoadOp>(op)) {
          newOp = builder.create<triton::gpu::CopyAsyncOp>(
            op->getLoc(), op->getResult(0).getType(),
            loadOp.ptr(), loadOp.mask(), loadOp.other(),
            loadOp.cache(), loadOp.evict(), loadOp.isVolatile()
          );
        } else
          llvm_unreachable("This should be LoadOp");
      } else
        newOp = builder.clone(*op);
      // llvm::errs() << "cloning " << *op << "\n";
      for (unsigned opIdx = 0; opIdx < op->getNumOperands(); ++opIdx) {
        auto it = valueMapping.find(op->getOperand(opIdx));
        if (it != valueMapping.end()) {
          Value v = it->second[stage];
          assert(v);
          newOp->setOperand(opIdx, v);
        } // else, op at opIdx is a loop-invariant value
      }

      // TODO: if this is a load, we need to update the mask

      // update mapping of results
      for (unsigned dstIdx : llvm::seq(unsigned(0), op->getNumResults())) {
        setValueMapping(op->getResult(dstIdx), newOp->getResult(dstIdx), stage);
        // update mapping for loop-carried values (args)
        for (OpOperand &operand : yieldOp->getOpOperands()) {
          if (operand.get() == op->getResult(dstIdx))
            setValueMapping(forOp.getRegionIterArgs()[operand.getOperandNumber()],
                            newOp->getResult(dstIdx), stage + 1);
        }
      }
    }
  }
}

scf::ForOp LoopPipeliner::createNewForOp() {
  OpBuilder builder(forOp);

  // order of new args:
  //   (original args),
  //   for each load result x:
  //     (x at stage[0, numStages-1))
  //   (depArgs at stage numStages-1)
  //   (iv at stage numStages-1)
  SmallVector<Value> newLoopArgs;
  // We need this to update operands for yield
  // original block arg => new arg's idx
  DenseMap<BlockArgument, size_t> depArgsIdx;
  for (auto v : forOp.getIterOperands())
    newLoopArgs.push_back(v);

  size_t loadIdx = newLoopArgs.size();
  for (Value loadOp : loads)
    for (int i = 0; i < numStages - 1; ++i)
      newLoopArgs.push_back(valueMapping[loadOp][i]);

  size_t depArgsBeginIdx = newLoopArgs.size();
  for (BlockArgument depArg : depArgs) {
    depArgsIdx[depArg] = newLoopArgs.size();
    newLoopArgs.push_back(valueMapping[depArg][numStages-1]);
  }

  size_t nextIVIdx = newLoopArgs.size();
  newLoopArgs.push_back(valueMapping[forOp.getInductionVar()][numStages-2]);

  for (size_t i = 0; i < newLoopArgs.size(); ++i)
    assert(newLoopArgs[i]);

  // llvm::errs() << "mapped load is:\n" << newLoopArgs[loadIdx] << "\n\n";

  // 1. signature of the new ForOp
  auto newForOp = builder.create<scf::ForOp>(forOp.getLoc(),
                                             forOp.getLowerBound(),
                                             forOp.getUpperBound(),
                                             forOp.getStep(),
                                             newLoopArgs);

  // 2. body of the new ForOp
  builder.setInsertionPointToStart(newForOp.getBody());
  BlockAndValueMapping mapping;
  for (const auto &arg : llvm::enumerate(forOp.getRegionIterArgs()))
    mapping.map(arg.value(), newForOp.getRegionIterArgs()[arg.index()]);

  for (Operation &op : forOp.getBody()->without_terminator()) {
    Operation *newOp = builder.clone(op, mapping);
    // update mapping of results
    for (unsigned dstIdx : llvm::seq(unsigned(0), op.getNumResults()))
      mapping.map(op.getResult(dstIdx), newOp->getResult(dstIdx));
  }

  // 3. replace loads with args
  for (size_t idx = 0; idx < loads.size(); ++idx) {
    Value load = loads[idx];
    mapping.lookup(load).replaceAllUsesWith(
      newForOp.getRegionIterArgs()[loadIdx+idx]);
  }


  // 4. prefetch the next iteration
  SmallVector<Operation*> orderedDeps;
  for (Operation &op : forOp.getLoopBody().front()) {
    if (depOps.contains(&op))
      orderedDeps.push_back(&op);
    else if (loads.contains(op.getResult(0)))
      orderedDeps.push_back(&op);
  }
  assert(depOps.size() + loads.size() == orderedDeps.size() &&
         "depOps contains invalid values");
  BlockAndValueMapping nextMapping;
  DenseMap<BlockArgument, Value> depArgsMapping;
  size_t argIdx = 0;
  for (BlockArgument arg : depArgs) {
    nextMapping.map(arg, newForOp.getRegionIterArgs()[argIdx + depArgsBeginIdx]);
    ++argIdx;
  }
  // special handling for iv & loop condition
  Value nextIV = builder.create<arith::AddIOp>(newForOp.getInductionVar().getLoc(),
                                               newForOp.getRegionIterArgs()[nextIVIdx],
                                               newForOp.getStep());
  Value nextLoopCond = builder.create<arith::CmpIOp>(
    nextIV.getLoc(), arith::CmpIPredicate::slt,
    nextIV, newForOp.getUpperBound());
  for (Operation *op : orderedDeps) {
    Operation *nextOp = nullptr;
    // update loading mask
    if (loads.contains(op->getResult(0))) {
      auto loadOp = llvm::cast<triton::LoadOp>(op);
      Value mask = loadOp.mask();
      Value splatCond = builder.create<triton::BroadcastOp>(mask.getLoc(),
                                                            mask.getType(),
                                                            nextLoopCond);
      Value newMask = builder.create<arith::AndIOp>(mask.getLoc(),
                                                    splatCond,
                                                    nextMapping.lookupOrDefault(mask));
      // if mask is defined outside the loop, don't update the map more than once
      if (!(forOp.isDefinedOutsideOfLoop(mask) && nextMapping.contains(mask)))
        nextMapping.map(mask, newMask);
      // TODO: more elegant way to do this?
      nextOp = builder.create<triton::gpu::CopyAsyncOp>(
        op->getLoc(), op->getResult(0).getType(),
        nextMapping.lookupOrDefault(loadOp.ptr()),
        nextMapping.lookupOrDefault(loadOp.mask()),
        nextMapping.lookupOrDefault(loadOp.other()),
        loadOp.cache(), loadOp.evict(), loadOp.isVolatile()
      );
    }
    else
      nextOp = builder.clone(*op, nextMapping);
    // llvm::errs() << "epilogue cloning...: " << *op << "\n";
    // update mapping of results
    for (unsigned dstIdx : llvm::seq(unsigned(0), op->getNumResults())) {
      nextMapping.map(op->getResult(dstIdx), nextOp->getResult(dstIdx));
      // if this is a loop-carried value, update the mapping for yield
      auto originYield = cast<scf::YieldOp>(forOp.getBody()->getTerminator());
      for (OpOperand &operand : originYield->getOpOperands()) {
        if (operand.get() == op->getResult(dstIdx)) {
          size_t originIdx = operand.getOperandNumber();
          size_t newArgIdx = depArgsIdx[forOp.getRegionIterArgs()[originIdx]];
          BlockArgument newArg = newForOp.getRegionIterArgs()[newArgIdx];
          depArgsMapping[newArg] = nextOp->getResult(dstIdx);
        }
      }
    }
  }

  // Finally, the YieldOp, need to sync with the order of newLoopArgs
  SmallVector<Value> yieldValues;
  for (Value v : forOp.getBody()->getTerminator()->getOperands())
    yieldValues.push_back(mapping.lookup(v));
  // for (int i = 1; i < numStages - 1; ++i)
  //   yieldValues.push_back(newForOp.getRegionIterArgs()[aArgIdx + i]);
  // yieldValues.push_back(nextMapping.lookup(info.dotOp.a()));
  // for (int i = 1; i < numStages - 1; ++i)
  //   yieldValues.push_back(newForOp.getRegionIterArgs()[bArgIdx + i]);
  // yieldValues.push_back(nextMapping.lookup(info.dotOp.b()));
  for (size_t idx = 0; idx < loads.size(); ++idx) {
    Value load = loads[idx];
    for (int stage = 1; stage < numStages - 1; ++stage) {
      yieldValues.push_back(newForOp.getRegionIterArgs()[
        loadIdx + idx*(numStages-1) + stage-1
      ]);
    }
    yieldValues.push_back(nextMapping.lookup(load));
  }

  for (size_t i = depArgsBeginIdx; i < nextIVIdx; ++i)
    yieldValues.push_back(depArgsMapping.lookup(newForOp.getRegionIterArgs()[i]));
  yieldValues.push_back(nextIV);
  builder.setInsertionPointToEnd(newForOp.getBody());
  builder.create<scf::YieldOp>(forOp.getBody()->getTerminator()->getLoc(),
                               yieldValues);
  return newForOp;
}

// ref: mlir/lib/Dialect/SCF/Transforms/LoopPipelining.cpp
struct PipelinePass : public TritonGPUPipelineBase<PipelinePass> {
  PipelinePass() = default;
  PipelinePass(int numStages) {
    this->numStages = numStages;
  }

  void runOnOperation() override {
    int numStages = this->numStages;

    if (numStages <= 1)
      return;

    getOperation()->walk([&](scf::ForOp forOp) -> void {
      LoopPipeliner pipeliner(forOp, numStages);

      if (pipeliner.initialize().failed())
        return;

      // llvm::errs() << "find a loop to pipeline...\n";
      pipeliner.emitPrologue();
      // llvm::errs() << "\nprologue emitted\n"
                  //  << *forOp->getParentOp();

      scf::ForOp newForOp = pipeliner.createNewForOp();

      // llvm::errs() << "new for created:\n" << newForOp << "\n"
                  //  << "inside:\n" << *newForOp->getParentOp() << "\n";

      // replace the original loop
      for (unsigned i = 0; i < forOp->getNumResults(); ++i)
        forOp->getResult(i).replaceAllUsesWith(newForOp->getResult(i));
      forOp->erase();
    });
  }
};
} // anonymous namespace

std::unique_ptr<Pass> mlir::createTritonGPUPipelinePass(int numStages) {
  return std::make_unique<PipelinePass>(numStages);
}