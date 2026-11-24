/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "Errors.h"
#include "rooftree_catch2.h"

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <string>
#include <string_view>

namespace {
[[gnu::noinline]] void FailingAssertion(int value) {
  ASSERT(value == 2, "value is %d", value);
}

struct ChildResult {
  int Signal = 0;
  std::string Stderr;
};

// The assertion aborts: in a child, its stderr read through a pipe, no core dump
ChildResult RunInChild(void (*failing)()) {
  int fds[2];
  REQUIRE(pipe(fds) == 0);
  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    // Catch2's own handler would report the abort as a failure of this test, from the child
    std::signal(SIGABRT, SIG_DFL);
    rlimit noCore{};
    setrlimit(RLIMIT_CORE, &noCore);
    dup2(fds[1], STDERR_FILENO);
    dup2(fds[1], STDOUT_FILENO);
    close(fds[0]);
    failing();
    _exit(0);
  }

  close(fds[1]);
  ChildResult result;
  char buffer[4096];
  for (ssize_t n; (n = read(fds[0], buffer, sizeof(buffer))) > 0;)
    result.Stderr.append(buffer, std::size_t(n));
  close(fds[0]);

  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFSIGNALED(status))
    result.Signal = WTERMSIG(status);
  return result;
}
}  // namespace

TEST_CASE("A failed ASSERT writes where, what and the stack trace, then aborts", "[Errors]") {
  ChildResult result = RunInChild([] { FailingAssertion(1); });

  CHECK(result.Signal == SIGABRT);
  CHECK(result.Stderr.find("ASSERTION FAILED: value == 2") != std::string::npos);
  CHECK(result.Stderr.find("value is 1") != std::string::npos);
  // The function that asserted is a frame of the trace ("   1#  FailingAssertion at ..."), not only
  // the __func__ of the first line
  bool inTrace = false;
  for (std::size_t start = 0, end; start < result.Stderr.size(); start = end + 1) {
    end = result.Stderr.find('\n', start);
    if (end == std::string::npos)
      end = result.Stderr.size();
    std::string_view line(result.Stderr.data() + start, end - start);
    if (line.find("# ") != std::string_view::npos &&
        line.find("FailingAssertion") != std::string_view::npos)
      inTrace = true;
  }
  CHECK(inTrace);
}
