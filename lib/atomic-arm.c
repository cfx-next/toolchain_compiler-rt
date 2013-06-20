/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef __arm__

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ANDROID_ATOMIC_INLINE inline __attribute__((always_inline))

ANDROID_ATOMIC_INLINE void android_compiler_barrier()
{
    __asm__ __volatile__ ("" : : : "memory");
}

ANDROID_ATOMIC_INLINE void android_memory_barrier()
{
#ifdef __ARM_ARCH_7A__
    __asm__ __volatile__ ("dmb" : : : "memory");
#else
    android_compiler_barrier();
#endif
}

ANDROID_ATOMIC_INLINE void android_memory_store_barrier()
{
#ifdef __ARM_ARCH_7A__
    __asm__ __volatile__ ("dmb st" : : : "memory");
#else
    android_compiler_barrier();
#endif
}

#ifdef __ARM_ARCH_7A__
ANDROID_ATOMIC_INLINE
int android_atomic_cas_bool(int32_t old_value, int32_t new_value,
                            volatile int32_t *ptr)
{
    int32_t prev, status;
    do {
        __asm__ __volatile__ ("ldrex %0, [%3]\n"
                              "mov %1, #0\n"
                              "teq %0, %4\n"
#ifdef __thumb2__
                              "it eq\n"
#endif
                              "strexeq %1, %5, [%3]"
                              : "=&r" (prev), "=&r" (status), "+m"(*ptr)
                              : "r" (ptr), "Ir" (old_value), "r" (new_value)
                              : "cc");
    } while (__builtin_expect(status != 0, 0));
    return prev != old_value;
}
#else
ANDROID_ATOMIC_INLINE
int android_atomic_cas_bool(int32_t old_value, int32_t new_value, volatile int32_t *ptr)
{
    typedef int (kuser_cmpxchg)(int32_t, int32_t, volatile int32_t *);
    int32_t prev, status;
    prev = *ptr;
    do {
        status = (*(kuser_cmpxchg *)0xffff0fc0)(old_value, new_value, ptr);
        if (__builtin_expect(status == 0, 1))
            return 0;
        prev = *ptr;
    } while (prev == old_value);
    return 1;
}
#endif

#ifdef __ARM_ARCH_7A__
ANDROID_ATOMIC_INLINE
int android_atomic_cas_val(int32_t old_value, int32_t new_value,
                            volatile int32_t *ptr)
{
    int32_t prev, status;
    do {
        __asm__ __volatile__ ("ldrex %0, [%3]\n"
                              "mov %1, #0\n"
                              "teq %0, %4\n"
#ifdef __thumb2__
                              "it eq\n"
#endif
                              "strexeq %1, %5, [%3]"
                              : "=&r" (prev), "=&r" (status), "+m"(*ptr)
                              : "r" (ptr), "Ir" (old_value), "r" (new_value)
                              : "cc");
    } while (__builtin_expect(status != 0, 0));
    return prev;
}
#else
ANDROID_ATOMIC_INLINE
int android_atomic_cas_val(int32_t old_value, int32_t new_value, volatile int32_t *ptr)
{
    typedef int (kuser_cmpxchg)(int32_t, int32_t, volatile int32_t *);
    int32_t prev, status;
    prev = *ptr;
    do {
        status = (*(kuser_cmpxchg *)0xffff0fc0)(old_value, new_value, ptr);
        if (__builtin_expect(status == 0, 1))
            return 0;
        prev = *ptr;
    } while (prev == old_value);
    return prev;
}
#endif

ANDROID_ATOMIC_INLINE
int android_atomic_acquire_cas(int32_t old_value, int32_t new_value,
                               volatile int32_t *ptr)
{
    int status = android_atomic_cas_bool(old_value, new_value, ptr);
    android_memory_barrier();
    return status;
}

ANDROID_ATOMIC_INLINE
int android_atomic_release_cas(int32_t old_value, int32_t new_value,
                               volatile int32_t *ptr)
{
    android_memory_barrier();
    return android_atomic_cas_bool(old_value, new_value, ptr);
}

#ifdef __ARM_ARCH_7A__
#define ANDROID_ATOMIC_FETCH_AND_OP(OP, OPCODE)                                   \
int32_t android_atomic_fetch_and_##OP (volatile int32_t *ptr, int32_t val)        \
{                                                                                 \
    int32_t prev, tmp, status;                                                    \
    android_memory_barrier();                                                     \
    do {                                                                          \
        __asm__ __volatile__ ("ldrex %0, [%4]\n"                                  \
                              #OPCODE" %1, %0, %5\n"                              \
                              "strex %2, %1, [%4]"                                \
                              : "=&r" (prev), "=&r" (tmp),                        \
                                "=&r" (status), "+m" (*ptr)                       \
                              : "r" (ptr), "Ir" (val)                             \
                              : "cc");                                            \
    } while (__builtin_expect(status != 0, 0));                                   \
    return prev;                                                                  \
}

ANDROID_ATOMIC_FETCH_AND_OP(add, add)
ANDROID_ATOMIC_FETCH_AND_OP(sub, sub)
ANDROID_ATOMIC_FETCH_AND_OP(and, and)
ANDROID_ATOMIC_FETCH_AND_OP(or,  orr)
ANDROID_ATOMIC_FETCH_AND_OP(xor, eor)

#else

#define ANDROID_ATOMIC_FETCH_AND_OP(OP, OPCODE)                                   \
int32_t android_atomic_fetch_and_##OP (volatile int32_t *ptr, int32_t val)        \
{                                                                                 \
    int32_t prev, status;                                                         \
    android_memory_barrier();                                                     \
    do {                                                                          \
        prev = *ptr;                                                              \
        status = android_atomic_cas_bool(prev, prev OPCODE val, ptr);             \
    } while (__builtin_expect(status != 0, 0));                                   \
    return prev;                                                                  \
}

ANDROID_ATOMIC_FETCH_AND_OP(add, +)
ANDROID_ATOMIC_FETCH_AND_OP(sub, -)
ANDROID_ATOMIC_FETCH_AND_OP(and, &)
ANDROID_ATOMIC_FETCH_AND_OP(or,  |)
ANDROID_ATOMIC_FETCH_AND_OP(xor, ^)

#endif

#ifdef __ARM_ARCH_7A__
int32_t android_atomic_fetch_and_nand (volatile int32_t *ptr, int32_t val)
{
    int32_t prev, tmp, status;
    android_memory_barrier();
    do {
        __asm__ __volatile__ ("ldrex %0, [%4]\n"
                              "and %1, %0, %5\n"
                              "mvn %1, %1\n"
                              "strex %2, %1, [%4]"
                              : "=&r" (prev), "=&r" (tmp),
                                "=&r" (status), "+m" (*ptr)
                              : "r" (ptr), "Ir" (val)
                              : "cc");
    } while (__builtin_expect(status != 0, 0));
    return prev;
}
#else
int32_t android_atomic_fetch_and_nand (volatile int32_t *ptr, int32_t val)
{
    int32_t prev, status;
    android_memory_barrier();
    do {
        prev = *ptr;
        status = android_atomic_cas_bool(prev, ~(prev & val), ptr);
    } while (__builtin_expect(status != 0, 0));
    return prev;
}
#endif

#ifdef __ARM_ARCH_7A__
#define ANDROID_ATOMIC_OP_AND_FETCH(OP, OPCODE)                                   \
int32_t android_atomic_##OP##_and_fetch (volatile int32_t *ptr, int32_t val)      \
{                                                                                 \
    int32_t prev, tmp, status;                                                    \
    android_memory_barrier();                                                     \
    do {                                                                          \
        __asm__ __volatile__ ("ldrex %0, [%4]\n"                                  \
                              #OPCODE" %1, %0, %5\n"                              \
                              "strex %2, %1, [%4]"                                \
                              : "=&r" (prev), "=&r" (tmp),                        \
                                "=&r" (status), "+m" (*ptr)                       \
                              : "r" (ptr), "Ir" (val)                             \
                              : "cc");                                            \
    } while (__builtin_expect(status != 0, 0));                                   \
    return tmp;                                                                   \
}


ANDROID_ATOMIC_OP_AND_FETCH(add, add)
ANDROID_ATOMIC_OP_AND_FETCH(sub, sub)
ANDROID_ATOMIC_OP_AND_FETCH(and, and)
ANDROID_ATOMIC_OP_AND_FETCH(or,  orr)
ANDROID_ATOMIC_OP_AND_FETCH(xor, eor)

#else

#define ANDROID_ATOMIC_OP_AND_FETCH(OP, OPCODE)                                   \
int32_t android_atomic_##OP##_and_fetch (volatile int32_t *ptr, int32_t val)      \
{                                                                                 \
    int32_t prev, status;                                                         \
    android_memory_barrier();                                                     \
    do {                                                                          \
        prev = *ptr;                                                              \
        status = android_atomic_cas_bool(prev, (prev OPCODE val), ptr);           \
    } while (__builtin_expect(status != 0, 0));                                   \
    return (prev OPCODE val);                                                     \
}

ANDROID_ATOMIC_OP_AND_FETCH(add, +)
ANDROID_ATOMIC_OP_AND_FETCH(sub, -)
ANDROID_ATOMIC_OP_AND_FETCH(and, &)
ANDROID_ATOMIC_OP_AND_FETCH(or,  |)
ANDROID_ATOMIC_OP_AND_FETCH(xor, ^)

#endif

#ifdef __ARM_ARCH_7A__
int32_t android_atomic_nand_and_fetch (volatile int32_t *ptr, int32_t val)
{
    int32_t prev, tmp, status;
    android_memory_barrier();
    do {
        __asm__ __volatile__ ("ldrex %0, [%4]\n"
                              "and %1, %0, %5\n"
                              "mvn %1, %1\n"
                              "strex %2, %1, [%4]"
                              : "=&r" (prev), "=&r" (tmp),
                                "=&r" (status), "+m" (*ptr)
                              : "r" (ptr), "Ir" (val)
                              : "cc");
    } while (__builtin_expect(status != 0, 0));
    return tmp;
}
#else
int32_t android_atomic_nand_and_fetch (volatile int32_t *ptr, int32_t val)
{
    int32_t prev, status;
    android_memory_barrier();
    do {
        prev = *ptr;
        status = android_atomic_cas_bool(prev, ~(prev & val), ptr);
    } while (__builtin_expect(status != 0, 0));
    return ~(prev & val);
}
#endif


// gcc compatible interface
// http://gcc.gnu.org/onlinedocs/gcc-4.8.0/gcc/_005f_005fsync-Builtins.html
// TODO: support other data width

void __sync_synchronize()
{
    android_memory_barrier();
}

int __sync_bool_compare_and_swap_4 (volatile int32_t *ptr, int32_t oldval, int32_t newval)
{
    android_memory_barrier();
    return android_atomic_cas_bool(oldval, newval, ptr);
}

uint32_t __sync_val_compare_and_swap_4 (volatile int32_t *ptr, int32_t oldval, int32_t newval)
{
    android_memory_barrier();
    return android_atomic_cas_val(oldval, newval, ptr);
}

/* __sync_op_and_fetch functions */

int32_t __sync_add_and_fetch_4(volatile int32_t *ptr, int32_t val)
{
    return android_atomic_add_and_fetch(ptr, val);
}

int32_t __sync_sub_and_fetch_4(volatile int32_t *ptr, int32_t val)
{
    return android_atomic_sub_and_fetch(ptr, val);
}

int32_t __sync_and_and_fetch_4(volatile int32_t *ptr, int32_t val)
{
    return android_atomic_and_and_fetch(ptr, val);
}

int32_t __sync_or_and_fetch_4(volatile int32_t *ptr, int32_t val)
{
    return android_atomic_or_and_fetch(ptr, val);
}


int32_t __sync_xor_and_fetch_4(volatile int32_t *ptr, int32_t val)
{
    return android_atomic_xor_and_fetch(ptr, val);
}

int32_t __sync_nand_and_fetch_4(volatile int32_t *ptr, int32_t val)
{
    return android_atomic_nand_and_fetch(ptr, val);
}


/* __sync_fetch_and_op functions */

int32_t __sync_fetch_and_add_4(volatile int32_t *ptr, int32_t val)
{
    return android_atomic_fetch_and_add(ptr, val);
}

int32_t __sync_fetch_and_sub_4(volatile int32_t *ptr, int32_t val)
{
    return android_atomic_fetch_and_sub(ptr, val);
}

int32_t __sync_fetch_and_and_4(volatile int32_t *ptr, int32_t val)
{
    return android_atomic_fetch_and_and(ptr, val);
}

int32_t __sync_fetch_and_or_4(volatile int32_t *ptr, int32_t val)
{
    return android_atomic_fetch_and_or(ptr, val);
}

int32_t __sync_fetch_and_xor_4(volatile int32_t *ptr, int32_t val)
{
    return android_atomic_fetch_and_xor(ptr, val);
}

int32_t __sync_fetch_and_nand_4(volatile int32_t *ptr, int32_t val)
{
    return android_atomic_fetch_and_nand(ptr, val);
}


// libgcc has these function, we should provided them too.
#define DEFINE_UNSUPPORT_SYNC_FUNC(FUNCNAME, TYPE, WIDTH)   \
  TYPE __sync_##FUNCNAME##_##WIDTH(volatile TYPE *ptr, TYPE val)   \
  {                                                                \
    fprintf(stderr, "We don't support %s yet!\n", __func__);       \
    abort();                                                       \
  }

DEFINE_UNSUPPORT_SYNC_FUNC(fetch_and_add, int8_t, 1)
DEFINE_UNSUPPORT_SYNC_FUNC(fetch_and_add, int16_t, 2)
DEFINE_UNSUPPORT_SYNC_FUNC(fetch_and_add, int64_t, 8)

DEFINE_UNSUPPORT_SYNC_FUNC(fetch_and_sub, int8_t, 1)
DEFINE_UNSUPPORT_SYNC_FUNC(fetch_and_sub, int16_t, 2)
DEFINE_UNSUPPORT_SYNC_FUNC(fetch_and_sub, int64_t, 8)

DEFINE_UNSUPPORT_SYNC_FUNC(fetch_and_and, int8_t, 1)
DEFINE_UNSUPPORT_SYNC_FUNC(fetch_and_and, int16_t, 2)
DEFINE_UNSUPPORT_SYNC_FUNC(fetch_and_and, int64_t, 8)

DEFINE_UNSUPPORT_SYNC_FUNC(fetch_and_or, int8_t, 1)
DEFINE_UNSUPPORT_SYNC_FUNC(fetch_and_or, int16_t, 2)
DEFINE_UNSUPPORT_SYNC_FUNC(fetch_and_or, int64_t, 8)

DEFINE_UNSUPPORT_SYNC_FUNC(fetch_and_xor, int8_t, 1)
DEFINE_UNSUPPORT_SYNC_FUNC(fetch_and_xor, int16_t, 2)
DEFINE_UNSUPPORT_SYNC_FUNC(fetch_and_xor, int64_t, 8)

DEFINE_UNSUPPORT_SYNC_FUNC(add_and_fetch, int8_t, 1)
DEFINE_UNSUPPORT_SYNC_FUNC(add_and_fetch, int16_t, 2)
DEFINE_UNSUPPORT_SYNC_FUNC(add_and_fetch, int64_t, 8)

DEFINE_UNSUPPORT_SYNC_FUNC(sub_and_fetch, int8_t, 1)
DEFINE_UNSUPPORT_SYNC_FUNC(sub_and_fetch, int16_t, 2)
DEFINE_UNSUPPORT_SYNC_FUNC(sub_and_fetch, int64_t, 8)

DEFINE_UNSUPPORT_SYNC_FUNC(and_and_fetch, int8_t, 1)
DEFINE_UNSUPPORT_SYNC_FUNC(and_and_fetch, int16_t, 2)
DEFINE_UNSUPPORT_SYNC_FUNC(and_and_fetch, int64_t, 8)

DEFINE_UNSUPPORT_SYNC_FUNC(or_and_fetch, int8_t, 1)
DEFINE_UNSUPPORT_SYNC_FUNC(or_and_fetch, int16_t, 2)
DEFINE_UNSUPPORT_SYNC_FUNC(or_and_fetch, int64_t, 8)

DEFINE_UNSUPPORT_SYNC_FUNC(xor_and_fetch, int8_t, 1)
DEFINE_UNSUPPORT_SYNC_FUNC(xor_and_fetch, int16_t, 2)
DEFINE_UNSUPPORT_SYNC_FUNC(xor_and_fetch, int64_t, 8)

DEFINE_UNSUPPORT_SYNC_FUNC(bool_compare_and_swap, int8_t, 1)
DEFINE_UNSUPPORT_SYNC_FUNC(bool_compare_and_swap, int16_t, 2)
DEFINE_UNSUPPORT_SYNC_FUNC(bool_compare_and_swap, int64_t, 8)

DEFINE_UNSUPPORT_SYNC_FUNC(val_compare_and_swap, int8_t, 1)
DEFINE_UNSUPPORT_SYNC_FUNC(val_compare_and_swap, int32_t, 2)
DEFINE_UNSUPPORT_SYNC_FUNC(val_compare_and_swap, int64_t, 8)

#endif // __arm__
