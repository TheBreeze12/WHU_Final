#include "frontend_fixture.h"

#include "toyc/ast.h"
#include "toyc/diagnostics.h"
#include "toyc/sema.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {

toyc::SemaResult analyze_source(const std::string& source, toyc::DiagnosticEngine& diagnostics) {
    toyc::test::ParseResult parsed = toyc::test::parse_source(source);
    diagnostics = std::move(parsed.diagnostics);
    EXPECT_FALSE(parsed.parser_had_error);
    EXPECT_NE(nullptr, parsed.unit);
    if (!parsed.unit) {
        return {};
    }
    EXPECT_TRUE(toyc::validate_comp_unit(*parsed.unit, diagnostics));
    return toyc::analyze(*parsed.unit, diagnostics);
}

}  // namespace

TEST(SemaConstEvalTest, ShortCircuitsAndInConstInitializer) {
    toyc::DiagnosticEngine diagnostics;
    toyc::SemaResult result = analyze_source(R"(
const int a = 0 && (1 / 0);

int main() {
    return a;
}
)",
                                             diagnostics);

    EXPECT_TRUE(result.ok);
    EXPECT_FALSE(diagnostics.has_errors());
}

TEST(SemaConstEvalTest, ShortCircuitsOrInConstInitializer) {
    toyc::DiagnosticEngine diagnostics;
    toyc::SemaResult result = analyze_source(R"(
const int a = 1 || (1 / 0);

int main() {
    return a;
}
)",
                                             diagnostics);

    EXPECT_TRUE(result.ok);
    EXPECT_FALSE(diagnostics.has_errors());
}

TEST(SemaConstEvalTest, StillRejectsNeededRhsDivisionByZero) {
    toyc::DiagnosticEngine diagnostics;
    toyc::SemaResult result = analyze_source(R"(
const int a = 1 && (1 / 0);

int main() {
    return a;
}
)",
                                             diagnostics);

    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(diagnostics.has_errors());
}
