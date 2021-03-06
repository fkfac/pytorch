#include "torch/csrc/jit/passes/canonicalize_ops.h"
#include "torch/csrc/jit/passes/dead_code_elimination.h"
#include "torch/csrc/jit/symbolic_variable.h"


namespace torch { namespace jit {

struct ChunkOutput {
  ChunkOutput(Value * v, size_t o)
    : val(v), offset(o) {};
  Value * val;
  size_t offset;
};

static at::optional<std::vector<ChunkOutput>> getChunkOutputs(Node *chunk) {
  std::vector<ChunkOutput> outputs;
  for (auto list_use : chunk->output()->uses()) {
    if (list_use.user->matches("aten::select(Tensor[] a, int b) -> Tensor", attr::b)) {
      outputs.emplace_back(list_use.user->output(),
                            list_use.user->get<int64_t>(attr::b).value());
    } else if (list_use.user->kind() == prim::ListUnpack) {
      // This sometimes happens if the sizes can't be evenly divided by the number of chunks
      if (static_cast<int64_t>(list_use.user->outputs().size()) != chunk->get<int64_t>(attr::chunks).value()) {
        return at::nullopt;
      }
      auto unpack_outputs = list_use.user->outputs();
      for (size_t i = 0; i < unpack_outputs.size(); ++i) {
        outputs.emplace_back(unpack_outputs[i], i);
      }
    } else {
      return at::nullopt;
    }
  }
  return outputs;
}

static void CanonicalizeOps(Block* block) {
  for (auto it = block->nodes().begin(), end = block->nodes().end(); it != end;
       ++it) {
    for (auto sub : it->blocks())
      CanonicalizeOps(sub);
    // For the case where we have an addmm where alpha and beta are Attributes
    // and both of those scalars are equal to 1.0, decompose this into an mm
    // followed by an add so that it can go through the existing optimization,
    // shape analysis and differentiation passes for those two individual ops.
    // Later, we will fuse together those two ops into a single addmm.
    if (it->matches("aten::addmm(Tensor self, Tensor mat1, Tensor mat2, *, Scalar beta, Scalar alpha) -> Tensor",
                    /*const_inputs=*/{attr::beta, attr::alpha})) {
      if (it->get<at::Scalar>(attr::alpha)->toDouble() != 1.0 ||
          it->get<at::Scalar>(attr::beta)->toDouble() != 1.0) {
        continue;
      }

      WithInsertPoint guard(*it);

      SymbolicVariable mat(it->inputs()[0]);
      SymbolicVariable mat1(it->inputs()[1]);
      SymbolicVariable mat2(it->inputs()[2]);

      auto mm_result = mat1.mm(mat2);
      auto result = mat + mm_result;

      it->output()->replaceAllUsesWith(result);
      it.destroyCurrent();
    } else if (it->matches("aten::chunk(Tensor self, int chunks, int dim) -> Tensor[]",
                           /*const_inputs=*/{attr::chunks, attr::dim})) {
      if (auto orig_outputs = getChunkOutputs(*it)) {
        WithInsertPoint guard(*it);
        SymbolicVariable self {it->namedInput(attr::self)};
        auto outputs = self.chunk(it->get<int64_t>(attr::chunks).value(),
                                  it->get<int64_t>(attr::dim).value());
        for (ChunkOutput orig_out : *orig_outputs) {
          orig_out.val->replaceAllUsesWith(outputs.at(orig_out.offset));
          outputs[orig_out.offset].value()->setType(orig_out.val->type());
        }
      }
    }
  }
}

void CanonicalizeOps(const std::shared_ptr<Graph>& graph) {
  CanonicalizeOps(graph->block());
  EliminateDeadCode(graph);
}


}}
