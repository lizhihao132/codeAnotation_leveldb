﻿// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

// AtomicPointer provides storage for a lock-free pointer.
// Platform-dependent implementation of AtomicPointer:
// - If the platform provides a cheap barrier, we use it with raw pointers
// - If cstdatomic is present (on newer versions of gcc, it is), we use
//   a cstdatomic-based AtomicPointer.  However we prefer the memory
//   barrier based version, because at least on a gcc 4.4 32-bit build
//   on linux, we have encountered a buggy <cstdatomic>
//   implementation.  Also, some <cstdatomic> implementations are much
//   slower than a memory-barrier based implementation (~16ns for
//   <cstdatomic> based acquire-load vs. ~1ns for a barrier based
//   acquire-load).
// This code is based on atomicops-internals-* in Google's perftools:
// http://code.google.com/p/google-perftools/source/browse/#svn%2Ftrunk%2Fsrc%2Fbase

#ifndef PORT_ATOMIC_POINTER_H_
#define PORT_ATOMIC_POINTER_H_

#include <stdint.h>
#ifdef LEVELDB_CSTDATOMIC_PRESENT
#include <cstdatomic>
#endif
#ifdef OS_WIN
#include <windows.h>
#endif
#ifdef OS_MACOSX
#include <libkern/OSAtomic.h>
#endif

#if defined(_M_X64) || defined(__x86_64__)
#define ARCH_CPU_X86_FAMILY 1
#elif defined(_M_IX86) || defined(__i386__) || defined(__i386)
#define ARCH_CPU_X86_FAMILY 1
#elif defined(__ARMEL__)
#define ARCH_CPU_ARM_FAMILY 1
#endif

namespace leveldb {
namespace port {

// Define MemoryBarrier() if available
// Windows on x86
#if defined(OS_WIN) && defined(COMPILER_MSVC) && defined(ARCH_CPU_X86_FAMILY)
// windows.h already provides a MemoryBarrier(void) macro
// http://msdn.microsoft.com/en-us/library/ms684208(v=vs.85).aspx
#define LEVELDB_HAVE_MEMORY_BARRIER

// Gcc on x86
#elif defined(ARCH_CPU_X86_FAMILY) && defined(__GNUC__)
inline void MemoryBarrier() {
  // See http://gcc.gnu.org/ml/gcc/2003-04/msg01180.html for a discussion on
  // this idiom. Also see http://en.wikipedia.org/wiki/Memory_ordering.
  __asm__ __volatile__("" : : : "memory");
}
#define LEVELDB_HAVE_MEMORY_BARRIER

// Sun Studio
#elif defined(ARCH_CPU_X86_FAMILY) && defined(__SUNPRO_CC)
inline void MemoryBarrier() {
  // See http://gcc.gnu.org/ml/gcc/2003-04/msg01180.html for a discussion on
  // this idiom. Also see http://en.wikipedia.org/wiki/Memory_ordering.
  asm volatile("" : : : "memory");
}
#define LEVELDB_HAVE_MEMORY_BARRIER

// Mac OS
#elif defined(OS_MACOSX)
inline void MemoryBarrier() {
  OSMemoryBarrier();
}
#define LEVELDB_HAVE_MEMORY_BARRIER

// ARM
#elif defined(ARCH_CPU_ARM_FAMILY)
typedef void (*LinuxKernelMemoryBarrierFunc)(void);
LinuxKernelMemoryBarrierFunc pLinuxKernelMemoryBarrier __attribute__((weak)) =
    (LinuxKernelMemoryBarrierFunc) 0xffff0fa0;
inline void MemoryBarrier() {
  pLinuxKernelMemoryBarrier();
}
#define LEVELDB_HAVE_MEMORY_BARRIER

#endif

// AtomicPointer built using platform-specific MemoryBarrier()
#if defined(LEVELDB_HAVE_MEMORY_BARRIER)

/*
	AtomicPointer 原子指针的设计初衷是对一个指针指向的内容赋值的多个操作是原子的。
	对于一个指针 void *p ，p 可能指向一个结构体
	struct B
	{
		int a;
		int b;
		int c;
	};
	
	thread 1:
	
	B *p = q;	//获得 q

	p.a = 1;
	p.b = 2;
	p.c = 4;

	o = p;


	thread 2:

	int theC = o.c;
	由于存在指令重排(编译器重排或 CPU 重排)，可能导致thread 1 还末将 p.c 成员赋值，就已经将 p 赋给了 o 这个共享变量，
	在 thread 2 中读到的 o.c 可能就是旧值。
	这个问题发生的原因是：对 指针 q 指向的内容赋值不是原子的(如果有多个需要赋值的操作).


	有多个线程会同时读取或更新这个对象，要求任何时刻读取到的内容一定是一致的，即读取到的a/b/c的值一定是相同一次更新的值，而不能出现读到的a是某一次更新的值，而b或c是另外一次更新的值。
	https://zhuanlan.zhihu.com/p/20832611?from=singlemessage

	AtomicPointer 就是解决了这个问题。
*/

class AtomicPointer {
 private:
  void* rep_;
 public:
  AtomicPointer() { }
  explicit AtomicPointer(void* p) : rep_(p) {}
  inline void* NoBarrier_Load() const { return rep_; }
  inline void NoBarrier_Store(void* v) { rep_ = v; }



  /*
	Acquire 语义：前面的普通的读和写操作可以向后越过该读操作，但后面的读和写不能向前越过。
	Release 语义：后面的普通的读和写操作可以向前越过该写操作，但前面的读和写不可向后越过。

	Acquire
	_____________________________________________ line 1
	|									|
	|			operation				|
	|处于 Acquire 和 Release 之间的		|
	|操作不可移到 line1 之前或 line 2 之后|
	|									|
	|_____________________________________________ line 2
	Release

	由 Acquire 加上 Release 的语义即构成一个锁的语义：Acquire  即获得了 Lock，Release 即 UnLock。

	注意 Acquire_Load 和 Release_Store 都是 inline 函数而不是一般的函数，如果是一般的函数，则不需要使用内存栅栏，因为在 C++ 中
	函数调用本身就具有 MemoryBarrier 的语义：http://huchh.com/2015/12/03/leveldb-atomicpointer/


  */

  inline void* Acquire_Load() const {
	//在Acquire_Load函数返回前，MemoryBarrier确保对rep_的写操作已经全部完成，写入内存。
    void* result = rep_;
	//load，处理Invalidate Queues，加载新值。相当于_ReadBarrier
	MemoryBarrier();	//Creates a hardware memory barrier (fence) that prevents the CPU from re-ordering read and write operations. 
						//It may also prevent the compiler from re-ordering read and write operations.
						//实际上就是一个xchg（对于IA-32），属于一个LOCK操作，因此具有Flush Store Buffer的功能
    return result;
  }

  /*含有Release语义的写操作. 相当于一个单向向前的栅障. 普通的读和写可以向前越过该写操作, 但是之前的读和写操作不能向后越过该写操作.

  */
  inline void Release_Store(void* v) {
	  //在Release_Store中，MemoryBarrier保证对rep_的更改“按照顺序”执行。
	//release，将 CPU cache 刷新到 store buffer。相当于 _WriteBarrier
    MemoryBarrier();	//此时，若有线程正在读，则使其读最新的值	
	//store
    rep_ = v;
  }
};

// AtomicPointer based on <cstdatomic>
#elif defined(LEVELDB_CSTDATOMIC_PRESENT)
class AtomicPointer {
 private:
  std::atomic<void*> rep_;
 public:
  AtomicPointer() { }
  explicit AtomicPointer(void* v) : rep_(v) { }
  inline void* Acquire_Load() const {
    return rep_.load(std::memory_order_acquire);
  }
  inline void Release_Store(void* v) {
    rep_.store(v, std::memory_order_release);
  }
  inline void* NoBarrier_Load() const {
    return rep_.load(std::memory_order_relaxed);
  }
  inline void NoBarrier_Store(void* v) {
    rep_.store(v, std::memory_order_relaxed);
  }
};

// We have neither MemoryBarrier(), nor <cstdatomic>
#else
#error Please implement AtomicPointer for this platform.

#endif

#undef LEVELDB_HAVE_MEMORY_BARRIER
#undef ARCH_CPU_X86_FAMILY
#undef ARCH_CPU_ARM_FAMILY

} // namespace leveldb::port
} // namespace leveldb

#endif  // PORT_ATOMIC_POINTER_H_
