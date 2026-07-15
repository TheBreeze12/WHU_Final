#pragma once

namespace toyc {

class Module;
class Function;

// Each pass is a pure IR->IR function returning whether it changed anything.
// They run only under -opt (see run_optim). All operate per-function.
bool constprop(Function& fn);
bool algebraic_simplify(Function& fn);
bool dce(Function& fn);
bool gvn(Function& fn);
bool cfs(Function& fn);
bool eliminate_tail_recursion(Function& fn);

// Drives the four passes to a fixpoint over every function (design §9).
bool run_optim(Module& module);

}  // namespace toyc
