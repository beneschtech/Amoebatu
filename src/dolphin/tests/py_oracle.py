#!/usr/bin/env python3
"""Run a Dolphin program under CPython, as a second implementation of the language.

    python3 tests/py_oracle.py program.dol [args...]

Programs that stick to the Python-compatible subset of Dolphin (no be_* /
bits builtins, no e.kind / e.message) produce identical output under both.
Golden outputs for such programs are generated with this script, so the
interpreter is checked against CPython rather than against itself.

Provides the Dolphin builtins Python lacks (read_file, DecodeError), then
calls main(*args) if the program defines it, as the dolphin driver does.
"""
import sys


class DecodeError(Exception):
    pass


def read_file(path):
    with open(path, "rb") as f:
        return f.read()


def main():
    path = sys.argv[1]
    env = {"__name__": "dolphin_program", "DecodeError": DecodeError, "read_file": read_file}
    with open(path) as f:
        exec(compile(f.read(), path, "exec"), env)
    if callable(env.get("main")):
        rc = env["main"](*sys.argv[2:])
        sys.exit(rc if isinstance(rc, int) else 0)


if __name__ == "__main__":
    main()
