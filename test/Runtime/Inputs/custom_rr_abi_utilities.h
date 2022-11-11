#define NUM_REGS 30

#define ALL_REGS(macro) \
  macro( 0) \
  macro( 1) \
  macro( 2) \
  macro( 3) \
  macro( 4) \
  macro( 5) \
  macro( 6) \
  macro( 7) \
  macro( 8) \
  macro( 9) \
  macro(10) \
  macro(11) \
  macro(12) \
  macro(13) \
  macro(14) \
  macro(15) \
  macro(16) \
  macro(17) \
  macro(19) \
  macro(20) \
  macro(21) \
  macro(22) \
  macro(23) \
  macro(24) \
  macro(25) \
  macro(26) \
  macro(27) \
  macro(28) \
  macro(29)

#define FUNCTION_REGS(macro, ...) \
  macro( 1, __VA_ARGS__) \
  macro( 2, __VA_ARGS__) \
  macro( 3, __VA_ARGS__) \
  macro( 4, __VA_ARGS__) \
  macro( 5, __VA_ARGS__) \
  macro( 6, __VA_ARGS__) \
  macro( 7, __VA_ARGS__) \
  macro( 8, __VA_ARGS__) \
  macro( 9, __VA_ARGS__) \
  macro(10, __VA_ARGS__) \
  macro(11, __VA_ARGS__) \
  macro(12, __VA_ARGS__) \
  macro(13, __VA_ARGS__) \
  macro(14, __VA_ARGS__) \
  macro(15, __VA_ARGS__) \
  macro(19, __VA_ARGS__) \
  macro(20, __VA_ARGS__) \
  macro(21, __VA_ARGS__) \
  macro(22, __VA_ARGS__) \
  macro(23, __VA_ARGS__) \
  macro(24, __VA_ARGS__) \
  macro(25, __VA_ARGS__) \
  macro(26, __VA_ARGS__) \
  macro(27, __VA_ARGS__) \
  macro(28, __VA_ARGS__)

#define ALL_FUNCTIONS(macro) \
  macro(swift_retain, 1) \
  macro(swift_release, 0) \

#define PASS_REGS_HELPER(num) \
  register void *x ## num asm ("x" #num) = regs[num];
#define PASS_REGS ALL_REGS(PASS_REGS_HELPER)

#define REG_INPUTS_HELPER(num) \
  "r" (x ## num),
#define REG_INPUTS ALL_REGS(REG_INPUTS_HELPER)

#define MAKE_CALL_FUNC(reg, func) \
  static inline void call_ ## func ## _x ## reg(void **regs) { \
    PASS_REGS \
    asm("bl _" #func "_x" #reg: : REG_INPUTS "i" (0)); \
  }

#define MAKE_ALL_CALL_FUNCS(function, isRetain) \
  FUNCTION_REGS(MAKE_CALL_FUNC, function)
ALL_FUNCTIONS(MAKE_ALL_CALL_FUNCS)

static inline void foreachRRFunction(void (*call)(void (*)(void **regs), const char *name, int reg, int isRetain)) {
  #define CALL_ONE_FUNCTION(reg, function, isRetain) \
    call(call_ ## function ## _x ## reg, #function, reg, isRetain);
  #define CALL_WITH_FUNCTIONS(function, isRetain) \
    FUNCTION_REGS(CALL_ONE_FUNCTION, function, isRetain)

  ALL_FUNCTIONS(CALL_WITH_FUNCTIONS)
//   call_swift_retain_x21(0);
}
