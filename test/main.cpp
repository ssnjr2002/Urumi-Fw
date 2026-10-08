/**
 * main.cpp — doctest's implementation + entry point for the native suites.
 *
 * Exactly ONE translation unit in this suite may define
 * DOCTEST_CONFIG_IMPLEMENT; every other file here just includes <doctest.h>.
 * Keeping it in a file of its own means adding a test_*.cpp never has to think
 * about it.
 */

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include <ostream>

namespace {

// PlatformIO counts a case only from a divider block, which doctest's console
// reporter prints for failures alone, so a passing suite reads as SKIPPED.
// This prints the same block for each passing case. PlatformIO's documented
// fix, the `success` option, prints every passing assertion (~750k lines here).
struct PioPassReporter : doctest::IReporter {
    std::ostream& out;
    const doctest::TestCaseData* tc = nullptr;

    explicit PioPassReporter(const doctest::ContextOptions& opt) : out(*opt.cout) {}

    void test_case_start(const doctest::TestCaseData& in) override { tc = &in; }

    void test_case_end(const doctest::CurrentTestCaseStats& st) override {
        if (!st.testCaseSuccess || !tc) return;   // the console reporter prints failures
        out << "===============================================================================\n"
            << doctest::skipPathFromFilename(tc->m_file.c_str()) << ":" << tc->m_line << ":\n";
        if (tc->m_test_suite && *tc->m_test_suite) out << "TEST SUITE: " << tc->m_test_suite << "\n";
        out << "TEST CASE:  " << tc->m_name << "\n\n";
    }

    void report_query(const doctest::QueryData&) override {}
    void test_run_start() override {}
    void test_run_end(const doctest::TestRunStats&) override {}
    void test_case_reenter(const doctest::TestCaseData&) override {}
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}
};

}  // namespace

REGISTER_LISTENER("pio_pass", 1, PioPassReporter);

int main(int argc, char** argv) {
    doctest::Context context;
    // PlatformIO judges from the reported cases; a non-zero exit reads as a
    // crash (ERRORED) and adds a case of its own.
    context.setOption("no-exitcode", true);
    context.applyCommandLine(argc, argv);
    return context.run();
}
