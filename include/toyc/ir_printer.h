#pragma once

#include <iosfwd>

namespace toyc {

class Module;
class Function;

void print_module(const Module& module, std::ostream& out);
void print_function(const Function& function, std::ostream& out);

}  // namespace toyc
