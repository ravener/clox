#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

#include "common.h"
#include "compiler.h"
#include "debug.h"
#include "object.h"
#include "memory.h"
#include "vm.h"

VM vm;

static Value clockNative(int argCount, Value* args) {
  return NUMBER_VAL((double)clock() / CLOCKS_PER_SEC);
}

static Value exitNative(int argCount, Value* args) {
  freeVM();
  exit(0);

  return NIL_VAL;
}

static Value gcNative(int argCount, Value* args) {
  int before = vm.bytesAllocated;
  collectGarbage();

  return NUMBER_VAL((double)(before - vm.bytesAllocated));
}

static Value gcHeapSizeNative(int argCount, Value* args) {
  return NUMBER_VAL((double)(vm.bytesAllocated));
}

static void resetStack(void) {
  vm.stackTop = vm.stack;
  vm.frameCount = 0;
  vm.openUpvalues = NULL;
}

static void runtimeError(const char* format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputs("\n", stderr);

  for (int i = vm.frameCount - 1; i >= 0; i--) {
    CallFrame* frame = &vm.frames[i];
    ObjFunction* function = frame->closure->function;
    size_t instruction = frame->ip - function->chunk.code - 1;
    fprintf(stderr, "[line %d] in ",
            function->chunk.lines[instruction]);
    if (function->name == NULL) {
      fprintf(stderr, "script\n");
    } else {
      fprintf(stderr, "%s()\n", function->name->chars);
    }
  }

  resetStack();
}

static void defineNative(const char* name, NativeFn function) {
  push(OBJ_VAL(copyString(name, (int)strlen(name))));
  push(OBJ_VAL(newNative(function)));
  tableSet(&vm.globals, AS_STRING(vm.stack[0]), vm.stack[1]);
  pop();
  pop();
}

void initVM(void) {
  resetStack();
  vm.objects = NULL;
  vm.bytesAllocated = 0;
  vm.nextGC = 1024 * 1024;

  vm.grayCount = 0;
  vm.grayCapacity = 0;
  vm.grayStack = NULL;

  initTable(&vm.globals);
  initTable(&vm.strings);

  vm.initString = NULL;
  vm.initString = copyString("init", 4);

  defineNative("clock", clockNative);
  defineNative("exit", exitNative);
  defineNative("gc", gcNative);
  defineNative("gcHeapSize", gcHeapSizeNative);
}

void freeVM(void) {
  freeTable(&vm.globals);
  freeTable(&vm.strings);
  vm.initString = NULL;
  freeObjects();
}

void push(Value value) {
  *vm.stackTop = value;
  vm.stackTop++;
}

Value pop(void) {
  vm.stackTop--;
  return *vm.stackTop;
}

static Value peek(int distance) {
  return vm.stackTop[-1 - distance];
}

static bool call(ObjClosure* closure, int argCount) {
  if (argCount != closure->function->arity) {
    runtimeError("Expected %d arguments but got %d.",
        closure->function->arity, argCount);
    return false;
  }

  if (vm.frameCount == FRAMES_MAX) {
    runtimeError("Stack overflow.");
    return false;
  }

  CallFrame* frame = &vm.frames[vm.frameCount++];
  frame->closure = closure;
  frame->ip = closure->function->chunk.code;
  frame->slots = vm.stackTop - argCount - 1;
  return true;
}

static bool callValue(Value callee, int argCount) {
  if (IS_OBJ(callee)) {
    switch (OBJ_TYPE(callee)) {
      case OBJ_BOUND_METHOD: {
        ObjBoundMethod* bound = AS_BOUND_METHOD(callee);
        vm.stackTop[-argCount - 1] = bound->receiver;
        return call(bound->method, argCount);
      }
      case OBJ_CLASS: {
        ObjClass* klass = AS_CLASS(callee);
        vm.stackTop[-argCount - 1] = OBJ_VAL(newInstance(klass));
        Value initializer;
        if (tableGet(&klass->methods, vm.initString,
                     &initializer)) {
          return call(AS_CLOSURE(initializer), argCount);
        } else if (argCount != 0) {
          runtimeError("Expected 0 arguments but got %d.",
                       argCount);
          return false;
        }
        return true;
      }
      case OBJ_CLOSURE:
        return call(AS_CLOSURE(callee), argCount);
      case OBJ_NATIVE: {
        NativeFn native = AS_NATIVE(callee);
        Value result = native(argCount, vm.stackTop - argCount);
        vm.stackTop -= argCount + 1;
        push(result);
        return true;
      }
      default:
        break; // Non-callable object type.
    }
  }
  runtimeError("Can only call functions and classes.");
  return false;
}

static bool invokeFromClass(ObjClass* klass, ObjString* name,
                            int argCount) {
  Value method;
  if (!tableGet(&klass->methods, name, &method)) {
    runtimeError("Undefined property '%s'.", name->chars);
    return false;
  }
  return call(AS_CLOSURE(method), argCount);
}

static bool invoke(ObjString* name, int argCount) {
  Value receiver = peek(argCount);

  if (!IS_INSTANCE(receiver)) {
    runtimeError("Only instances have methods.");
    return false;
  }

  ObjInstance* instance = AS_INSTANCE(receiver);

  Value value;
  if (tableGet(&instance->fields, name, &value)) {
    vm.stackTop[-argCount - 1] = value;
    return callValue(value, argCount);
  }

  return invokeFromClass(instance->klass, name, argCount);
}

static bool bindMethod(ObjClass* klass, ObjString* name) {
  Value method;
  if (!tableGet(&klass->methods, name, &method)) {
    runtimeError("Undefined property '%s'.", name->chars);
    return false;
  }

  ObjBoundMethod* bound = newBoundMethod(peek(0),
                                         AS_CLOSURE(method));
  pop();
  push(OBJ_VAL(bound));
  return true;
}

static ObjUpvalue* captureUpvalue(Value* local) {
  ObjUpvalue* prevUpvalue = NULL;
  ObjUpvalue* upvalue = vm.openUpvalues;
  while (upvalue != NULL && upvalue->location > local) {
    prevUpvalue = upvalue;
    upvalue = upvalue->next;
  }

  if (upvalue != NULL && upvalue->location == local) {
    return upvalue;
  }

  ObjUpvalue* createdUpvalue = newUpvalue(local);
  createdUpvalue->next = upvalue;

  if (prevUpvalue == NULL) {
    vm.openUpvalues = createdUpvalue;
  } else {
    prevUpvalue->next = createdUpvalue;
  }

  return createdUpvalue;
}

static void closeUpvalues(Value* last) {
  while (vm.openUpvalues != NULL &&
         vm.openUpvalues->location >= last) {
    ObjUpvalue* upvalue = vm.openUpvalues;
    upvalue->closed = *upvalue->location;
    upvalue->location = &upvalue->closed;
    vm.openUpvalues = upvalue->next;
  }
}

static void defineMethod(ObjString* name) {
  Value method = peek(0);
  ObjClass* klass = AS_CLASS(peek(1));
  tableSet(&klass->methods, name, method);
  pop();
}

static bool isFalsey(Value value) {
  return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static void concatenate(void) {
  ObjString* b = AS_STRING(peek(0));
  ObjString* a = AS_STRING(peek(1));

  int length = a->length + b->length;
  char* chars = ALLOCATE(char, length + 1);
  memcpy(chars, a->chars, a->length);
  memcpy(chars + a->length, b->chars, b->length);
  chars[length] = '\0';

  ObjString* result = takeString(chars, length);
  pop();
  pop();
  push(OBJ_VAL(result));
}

static InterpretResult run(void) {
  register CallFrame* frame;
  register Value* stackStart;
  register uint8_t* ip;
  register ObjFunction* fn;

#define LOAD_FRAME()                     \
  frame = &vm.frames[vm.frameCount - 1]; \
  stackStart = frame->slots;             \
  ip = frame->ip;                        \
  fn = frame->closure->function;

#define STORE_FRAME() frame->ip = ip

#define READ_BYTE() (*ip++)
#define PUSH(value) (*vm.stackTop++ = value)
#define POP()       (*(--vm.stackTop))
#define DROP()      (--vm.stackTop)
#define PEEK()      (*(vm.stackTop - 1))
#define PEEK2()     (*(vm.stackTop - 2))
#define READ_SHORT() (ip += 2, (uint16_t)((ip[-2] << 8) | ip[-1]))

#define READ_CONSTANT() (fn->chunk.constants.values[READ_BYTE()])
#define READ_STRING() AS_STRING(READ_CONSTANT())

#define BINARY_OP(valueType, op) \
    do { \
      if (!IS_NUMBER(PEEK()) || !IS_NUMBER(PEEK2())) { \
        frame->ip = ip; \
        runtimeError("Operands must be numbers."); \
        return INTERPRET_RUNTIME_ERROR; \
      } \
      double b = AS_NUMBER(POP()); \
      double a = AS_NUMBER(POP()); \
      PUSH(valueType(a op b)); \
    } while (false)

#ifdef DEBUG_TRACE_EXECUTION
#define TRACE_EXECUTION()                                        \
  do {                                                            \
    for (Value* slot = vm.stack; slot < vm.stackTop; slot++) {    \
      printf("[ ");                                               \
      printValue(*slot);                                          \
      printf(" ]");                                               \
    }                                                             \
    printf("\n");                                                 \
    disassembleInstruction(&fn->chunk,      \
        (int)(ip - fn->chunk.code)); \
  } while(false)
#else
#define TRACE_EXECUTION() do {} while(false)
#endif

#if COMPUTED_GOTO
  static void* dispatchTable[] = {
    #define OPCODE(op) &&code_##op,
    #include "opcodes.h"
    #undef OPCODE
  };

#define INTERPRET_LOOP DISPATCH();
#define CASE_CODE(name) code_##name
#define DISPATCH()                                           \
  do {                                                       \
    TRACE_EXECUTION();                                       \
    goto *dispatchTable[instruction = (OpCode)READ_BYTE()];  \
  } while(false)


#else

#define INTERPRET_LOOP                                        \
    loop:                                                     \
      TRACE_EXECUTION();                                      \
      switch (instruction = (OpCode)READ_BYTE())

#define CASE_CODE(name)  case OP_##name
#define DISPATCH()       goto loop
#endif

  LOAD_FRAME();

  OpCode instruction;
  INTERPRET_LOOP {
    CASE_CODE(CONSTANT): {
      PUSH(READ_CONSTANT());
      DISPATCH();
    }

    CASE_CODE(NIL): PUSH(NIL_VAL); DISPATCH();
    CASE_CODE(TRUE): PUSH(BOOL_VAL(true)); DISPATCH();
    CASE_CODE(FALSE): PUSH(BOOL_VAL(false)); DISPATCH();
    CASE_CODE(POP): DROP(); DISPATCH();

    CASE_CODE(GET_LOCAL): {
      PUSH(stackStart[READ_BYTE()]);
      DISPATCH();
    }
    
    CASE_CODE(SET_LOCAL): {
      stackStart[READ_BYTE()] = PEEK();
      DISPATCH();
    }
    
    CASE_CODE(GET_GLOBAL): {
      ObjString* name = READ_STRING();
      Value value;
      if (!tableGet(&vm.globals, name, &value)) {
        STORE_FRAME();
        runtimeError("Undefined variable '%s'.", name->chars);
        return INTERPRET_RUNTIME_ERROR;
      }
      PUSH(value);
      DISPATCH();
    }

    CASE_CODE(DEFINE_GLOBAL): {
      ObjString* name = READ_STRING();
      tableSet(&vm.globals, name, PEEK());
      DROP();
      DISPATCH();
    }
    
    CASE_CODE(SET_GLOBAL): {
      ObjString* name = READ_STRING();
      if (tableSet(&vm.globals, name, PEEK())) {
        tableDelete(&vm.globals, name);
        STORE_FRAME();
        runtimeError("Undefined variable '%s'.", name->chars);
        return INTERPRET_RUNTIME_ERROR;
      }
      DISPATCH();
    }

    CASE_CODE(GET_UPVALUE): {
      PUSH(*frame->closure->upvalues[READ_BYTE()]->location);
      DISPATCH();
    }

    CASE_CODE(SET_UPVALUE): {
      *frame->closure->upvalues[READ_BYTE()]->location = PEEK();
      DISPATCH();
    }
    
    CASE_CODE(GET_PROPERTY): {
      if (!IS_INSTANCE(PEEK())) {
        STORE_FRAME();
        runtimeError("Only instances have properties.");
        return INTERPRET_RUNTIME_ERROR;
      }

      ObjInstance* instance = AS_INSTANCE(PEEK());
      ObjString* name = READ_STRING();
        
      Value value;
      if (tableGet(&instance->fields, name, &value)) {
        DROP(); // Instance.
        PUSH(value);
        DISPATCH();
      }

      if (!bindMethod(instance->klass, name)) {
        return INTERPRET_RUNTIME_ERROR;
      }
      DISPATCH();
    }

    CASE_CODE(SET_PROPERTY): {
      if (!IS_INSTANCE(PEEK2())) {
        STORE_FRAME();
        runtimeError("Only instances have fields.");
        return INTERPRET_RUNTIME_ERROR;
      }

      ObjInstance* instance = AS_INSTANCE(PEEK2());
      tableSet(&instance->fields, READ_STRING(), PEEK());
      Value value = POP();
      DROP();
      PUSH(value);
      DISPATCH();
    }

    CASE_CODE(GET_SUPER): {
      ObjString* name = READ_STRING();
      ObjClass* superclass = AS_CLASS(POP());
        
      if (!bindMethod(superclass, name)) {
        return INTERPRET_RUNTIME_ERROR;
      }
      DISPATCH();
    }

    CASE_CODE(EQUAL): {
      Value b = POP();
      Value a = POP();
      PUSH(BOOL_VAL(valuesEqual(a, b)));
      DISPATCH();
    }

    CASE_CODE(GREATER): BINARY_OP(BOOL_VAL, >); DISPATCH();
    CASE_CODE(LESS):    BINARY_OP(BOOL_VAL, <); DISPATCH();

    CASE_CODE(ADD): {
      if (IS_STRING(PEEK()) && IS_STRING(PEEK2())) {
        concatenate();
      } else if (IS_NUMBER(PEEK()) && IS_NUMBER(PEEK2())) {
        double b = AS_NUMBER(POP());
        double a = AS_NUMBER(POP());
        PUSH(NUMBER_VAL(a + b));
      } else {
        STORE_FRAME();
        runtimeError(
            "Operands must be two numbers or two strings.");
        return INTERPRET_RUNTIME_ERROR;
      }
      DISPATCH();
    }

    CASE_CODE(SUBTRACT): BINARY_OP(NUMBER_VAL, -); DISPATCH();
    CASE_CODE(MULTIPLY): BINARY_OP(NUMBER_VAL, *); DISPATCH();
    CASE_CODE(DIVIDE):   BINARY_OP(NUMBER_VAL, /); DISPATCH();

    CASE_CODE(NOT):
      PUSH(BOOL_VAL(isFalsey(POP())));
      DISPATCH();

    CASE_CODE(NEGATE):
      if (!IS_NUMBER(PEEK())) {
        STORE_FRAME();
        runtimeError("Operand must be a number.");
        return INTERPRET_RUNTIME_ERROR;
      }
      PUSH(-AS_NUMBER(POP()));
      DISPATCH();

    CASE_CODE(PRINT): {
      printValue(POP());
      printf("\n");
      DISPATCH();
    }

    CASE_CODE(JUMP): {
      uint16_t offset = READ_SHORT();
      ip += offset;
      DISPATCH();
    }

    CASE_CODE(JUMP_IF_FALSE): {
      uint16_t offset = READ_SHORT();
      if (isFalsey(PEEK())) ip += offset;
      DISPATCH();
    }

    CASE_CODE(LOOP): {
      uint16_t offset = READ_SHORT();
      ip -= offset;
      DISPATCH();
    }

    CASE_CODE(CALL): {
      int argCount = READ_BYTE();
      STORE_FRAME();

      if (!callValue(peek(argCount), argCount)) {
        return INTERPRET_RUNTIME_ERROR;
      }

      LOAD_FRAME();
      DISPATCH();
    }

    CASE_CODE(INVOKE): {
      ObjString* method = READ_STRING();
      int argCount = READ_BYTE();
      STORE_FRAME();

      if (!invoke(method, argCount)) {
        return INTERPRET_RUNTIME_ERROR;
      }

      LOAD_FRAME();
      DISPATCH();
    }
    
    CASE_CODE(SUPER_INVOKE): {
      ObjString* method = READ_STRING();
      int argCount = READ_BYTE();
      ObjClass* superclass = AS_CLASS(POP());
      STORE_FRAME();
      if (!invokeFromClass(superclass, method, argCount)) {
        return INTERPRET_RUNTIME_ERROR;
      }
      LOAD_FRAME();
      DISPATCH();
    }

    CASE_CODE(CLOSURE): {
      ObjFunction* function = AS_FUNCTION(READ_CONSTANT());
      ObjClosure* closure = newClosure(function);
      PUSH(OBJ_VAL(closure));
      for (int i = 0; i < closure->upvalueCount; i++) {
        uint8_t isLocal = READ_BYTE();
        uint8_t index = READ_BYTE();
        if (isLocal) {
          closure->upvalues[i] =
              captureUpvalue(stackStart + index);
        } else {
          closure->upvalues[i] = frame->closure->upvalues[index];
        }
      }
      DISPATCH();
    }

    CASE_CODE(CLOSE_UPVALUE):
      closeUpvalues(vm.stackTop - 1);
      DROP();
      DISPATCH();

    CASE_CODE(RETURN): {
      Value result = POP();
      closeUpvalues(stackStart);
      vm.frameCount--;
      if (vm.frameCount == 0) {
        DROP();
        return INTERPRET_OK;
      }

      vm.stackTop = stackStart;
      PUSH(result);
      LOAD_FRAME();
      DISPATCH();
    }

    CASE_CODE(CLASS):
      PUSH(OBJ_VAL(newClass(READ_STRING())));
      DISPATCH();

    CASE_CODE(INHERIT): {
      Value superclass = PEEK2();
      if (!IS_CLASS(superclass)) {
        STORE_FRAME();
        runtimeError("Superclass must be a class.");
        return INTERPRET_RUNTIME_ERROR;
      }

      ObjClass* subclass = AS_CLASS(PEEK());
      tableAddAll(&AS_CLASS(superclass)->methods,
                  &subclass->methods);
      DROP(); // Subclass.
      DISPATCH();
    }

    CASE_CODE(METHOD):
      defineMethod(READ_STRING());
      DISPATCH();
  }

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef BINARY_OP
}

InterpretResult interpret(const char* source) {
  ObjFunction* function = compile(source);
  if (function == NULL) return INTERPRET_COMPILE_ERROR;

  push(OBJ_VAL(function));
  ObjClosure* closure = newClosure(function);
  pop();
  push(OBJ_VAL(closure));
  call(closure, 0);

  return run();
}
