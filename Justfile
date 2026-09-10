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

fmt:
    find libs tests -name '*.hpp' -o -name '*.cpp' | xargs clang-format -i

clean:
    rm -rf build
