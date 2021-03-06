/* clang-format off */
#include "drake/solvers/mosek_solver.h"
/* clang-format on */

#include <stdexcept>

namespace drake {
namespace solvers {

bool MosekSolver::available() const {
  return false;
}

SolutionResult MosekSolver::Solve(MathematicalProgram&) const {
  throw std::runtime_error(
      "Mosek is not installed in your build. You'll need to use a different "
      "solver.");
}

}  // namespace solvers
}  // namespace drake
