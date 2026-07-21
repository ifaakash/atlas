# CFLAGS for C

```bash
| Standard | Year      | Example Flag |
| -------- | --------- | ------------ |
| C89/C90  | 1989/1990 | `-std=c89`   |
| C99      | 1999      | `-std=c99`   |
| C11      | 2011      | `-std=c11`   |
| C17      | 2018      | `-std=c17`   |
| C23      | 2024      | `-std=c23`   |
```

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

