set shell := ["bash", "-cu"]

default: (build "debug")

configure preset="debug":
    cmake --preset {{preset}}

build preset="debug": (configure preset)
    cmake --build --preset {{preset}}

test preset="asan": (build preset)
    ctest --preset {{preset}}

bench: (build "release")
    ./build/release/libs/eval/poker_eval_bench

play *args="": (build "release")
    ./build/release/apps/cli/poker play {{args}}

py: (build "release-py")
    ctest --preset release-py

example: (build "release-py")
    PYTHONPATH=build/release-py/bindings/python \
      "$(sed -n 's/^Python_EXECUTABLE:FILEPATH=//p' build/release-py/CMakeCache.txt)" \
      bindings/python/example.py

fmt:
    find libs tests -name '*.hpp' -o -name '*.cpp' | xargs clang-format -i

install: (build "release")
    ln -sf {{justfile_directory()}}/build/release/apps/cli/poker /opt/homebrew/bin/poker

clean:
    rm -rf build
