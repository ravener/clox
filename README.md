# Extended CLox
This repository is an extension of [clox](https://github.com/munificent/craftinginterpreters/blob/master/c) from the book [Crafting Interpreters](https://craftinginterpreters.com)

It implements various features and optimizations that you might want to learn about and add to your own implementations.

The goal is to take clox to the next level while still keeping it simple to understand the new concepts.

## What's new
Below is all the changes with an explanation and links to various resources to learn more.

## Computed Goto
The code is given a macro switch to enable a different approach for the VM's dispatch loop using `goto` this is known to speed up dispatch by about 15-25%

This approach is also used by Ruby's YARV, CPython and Dalvik (Android's Java VM)

The code structure to implement it was inspired by [Wren][wren]

The opcodes list was also moved into its own [opcodes.h](src/opcodes.h) file using an [X Macro][xmacro]

Note that this is not a standard C feature and not all compilers support it, while `clang` and `gcc` support it, Windows Visual Studio does not. The VM feature is hidden behind a macro and will be disabled for Windows automatically and default to the switch-case.

**Resources**
- [X Macro - Wikipedia][xmacro]
- [Goto - Wikipedia](https://en.wikipedia.org/wiki/Goto)
- [Computed goto for efficient dispatch tables - Eli Bendersky's website](https://eli.thegreenplace.net/2012/07/12/computed-goto-for-efficient-dispatch-tables)
- Related code in Wren: [wren\_opcodes.h](https://github.com/wren-lang/wren/blob/4ffe2ed38b238ff410e70654cbe38883f7533d3f/src/vm/wren_opcodes.h) and [wren\_vm.c](https://github.com/wren-lang/wren/blob/4ffe2ed38b238ff410e70654cbe38883f7533d3f/src/vm/wren_vm.c#L890)

[xmacro]: https://en.wikipedia.org/wiki/X_Macro

## Commonly used variables declared as `register` in the dispatch loop.
This is an optimization that was also mentioned in the first challenge of [Chapter 24 - Calls and Functions](https://craftinginterpreters.com/calls-and-functions.html#challenges)

I've optimized the `ip` into a register as well as some other things like the `CallFrame` itself, this was also again inspired by [Wren][wren]

**Resources**
- [register (keyword) - Wikipedia](https://en.wikipedia.org/wiki/Register_(keyword))
- Bob's own answer in the [book's repository](https://github.com/munificent/craftinginterpreters/blob/master/note/answers/chapter24_calls/1.md)
- Related code in Wren: [wren\_vm.c](https://github.com/wren-lang/wren/blob/4ffe2ed38b238ff410e70654cbe38883f7533d3f/src/vm/wren_vm.c#L832)

## `gperf` for scanning keywords
A tool called `gperf` or the GNU perfect hash function generator was used here to demonstrate generating a hash function to speed up keyword lookup, this leads to a faster scanner overall while making it easy to extend and add new keywords with zero cost.

See [src/keywords](src/keywords) for the keyword definition and the generated file at [src/lex.def](src/lex.def) which is leveraged in [src/scanner.c](src/scanner.c)

The `gperf` command used to generate the code is:
```sh
$ cd src/
$ gperf -c -C -t -N checkKeyword keywords > lex.def
```

This approach is also used by Ruby, in both the main [ruby](https://github.com/ruby/ruby) implementation and also [mruby](https://github.com/mruby/mruby)

**Resources**
- [GNU gperf's website](https://www.gnu.org/software/gperf)
- [Using gperf for Keyword Lookup in Lexers | Ravener](https://ravener.vercel.app/posts/using-gperf-for-keyword-lookup-in-lexers) (my own blog post about this)
- Ruby's definition files: [CRuby](https://github.com/ruby/ruby/blob/master/defs/keywords) and [mruby](https://github.com/mruby/mruby/blob/master/mrbgems/mruby-compiler/core/keywords)

## Extra Native Functions
A few more small functions have been added to make it more useful:
- `gc()` Manually triggers a garbage collection and returns the amount of bytes freed.
- `gcHeapSize()` How much bytes are allocated. (And tracked by GC)
- `exit()` Exits the VM.

For some reason, I enjoy garbage collection statistics, in an ideal language with modules I'd create more functions and put them up under a `gc` module.

## Other Changes
Some other changes that doesn't need its own section include:
- Optimized the dispatch loop with macros for pushing/popping/peeking to avoid the overhead of a function call.
- Support hexadecimal literals (e.g `0xFF`)
- Support ternary conditionals (e.g `a ? b : c`)
- Use `void` for functions that don't take any arguments.

## Building
This is meant to be more so an example rather than an actual project so no build tools have been configured, but since clox is an easy project you can easily come up with something yourself. In the meantime a basic command like:
```sh
$ gcc src/*.c -o clox -O3
```
Should suffice.

Do keep in mind the `gperf` hash function, you may want to integrate that in to your build tool somehow. I recommend doing so for developement but for release it's better to just publish the generated file and let users compile rightaway.

## TODO
Other changes I'd like to demonstrate in this repository include:
- 16-bit short constants so we can use up to `65536` constants.
- Classes for builtin types (e.g adding methods into strings like `str.uppercase()`)
- Arrays. Add a simple array implementation.
- Depending on how complicated it would be, maybe make the GC incremental?
- `break` and `continue`
- `static` members in classes.

If you'd like to help with any of this, feel free to open pull requests. Try to keep things simple!

[wren]: https://wren.io
