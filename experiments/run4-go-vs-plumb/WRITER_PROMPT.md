You are writing a small program in {LANG_NAME}, as you would for a colleague
who will review it and put it into use.

- The task is in `{E}/tasks/{TASK}/SPEC.md`. Read it carefully: the program
  will be tested against exactly what it says.
- Write the program in `{E}/solutions/{TASK}/{LANG}/` (your working
  directory). {LANG_FILES}
- **Compile only with `{E}/bin/build`**, run from that directory. It
  compiles to `./app` and logs the attempt. Don't call the compiler
  directly to build the program.
- You may run `./app`, write your own tests, and try inputs, as much as
  you would on a real task. Finish when you believe the program is correct
  and the last `bin/build` succeeded.
- Don't open anything else under `{E}` (other tasks, other solutions,
  results, the harness): this is a controlled experiment, and those files
  contain the hidden tests.
{LANG_DOCS}

At the end, reply with a short summary: what you built, how you checked it,
and anything in the spec you found ambiguous and how you read it.
