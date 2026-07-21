## CFLAGS for C

```bash
| Standard | Year      | Example Flag |
| -------- | --------- | ------------ |
| C89/C90  | 1989/1990 | `-std=c89`   |
| C99      | 1999      | `-std=c99`   |
| C11      | 2011      | `-std=c11`   |
| C17      | 2018      | `-std=c17`   |
| C23      | 2024      | `-std=c23`   |
```

### Why specify -std?

Without it, GCC uses its default language mode, which can vary depending on the compiler version and may include GCC-specific extensions.

The declaration:

```bash
for (int i = 0; i < 5; i++)```

is not allowed in C89. Compile with:

```gcc -std=c89 main.c```

You'll get an error similar to: 
`error: 'for' loop initial declarations are only allowed in C99 or C11 mode`

Now compile with:

```bash
gcc -std=c11 main.c```

It compiles successfully because C11 includes that feature.

### Why -g flag in CFLAGS?
The -g flag tells the compiler to include debugging information in the executable.

This information is not used by your program while it runs. Instead, it's used by debuggers like GDB (GNU Debugger) so they can show you your source code, variable values, function names, and line numbers.
