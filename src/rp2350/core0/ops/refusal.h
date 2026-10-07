#pragma once

// refusal.h — the text an op refuses with, after `err `. Ops print nothing:
// they return nullptr or a refusal, and the command handler prints the line.
//
// One buffer, shared by every op: a refusal is valid until the next refuse().
// An op that runs another op after deciding to refuse (a teardown) formats its
// own refusal last.
const char* refuse(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
