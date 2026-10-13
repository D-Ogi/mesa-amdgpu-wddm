/* SPDX-License-Identifier: MIT */
#ifndef D3D10UMD_COMPILE_DISPATCH_H
#define D3D10UMD_COMPILE_DISPATCH_H

#include <windows.h>
#include <process.h>
#include <fenv.h>
#include <new>
#include <tuple>
#include <type_traits>
#include <cstdlib>
#pragma warning(push)
#pragma warning(disable: 4201) /* WDK anonymous unions */
#include <d3d10umddi.h>
#pragma warning(pop)

/* The D3D10 DDI serializes device entries (CalcPrivate* can be concurrent).
 * Allocate the compiler thread and both mailboxes during CreateDevice, where
 * OOM is legal. No thread/fiber/stack allocation occurs in later DDI entries.
 * Runtime callbacks run synchronously on the original runtime thread. The
 * mailbox pump admits callback reentry without acquiring the outer lock again.
 * Windows fibers alone cannot give this guarantee: ConvertThreadToFiberEx can
 * allocate on the first entry from each previously unseen runtime thread.
 */
#ifndef BC250_DISPATCH_ALLOCATION_ALLOWED
#define BC250_DISPATCH_ALLOCATION_ALLOWED() true
#endif

class Bc250CompileDispatch {
   enum class Kind { Work, Callback, Reply, Done, Stop };
   struct Message { Kind kind; void (*run)(void *); void *data; fenv_t env; };
   HANDLE work_ready = nullptr, reply_ready = nullptr, thread = nullptr;
   Message to_worker{}, to_caller{};
   SRWLOCK lock = SRWLOCK_INIT;
   DWORD worker_id = 0;
   volatile LONG caller_id = 0;

   static void check(bool ok) { if (!ok) std::abort(); }
   static void send(HANDLE event, Message &slot, Kind kind, void (*run)(void *), void *data) {
      slot.kind = kind; slot.run = run; slot.data = data;
      check(fegetenv(&slot.env) == 0);
      check(SetEvent(event) != FALSE);
   }
   static Message receive(HANDLE event, const Message &slot) {
      check(WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0);
      return slot;
   }
   static void execute(const Message &m) {
      fenv_t saved;
      check(fegetenv(&saved) == 0 && fesetenv(&m.env) == 0);
      m.run(m.data);
      check(fesetenv(&saved) == 0);
   }
   void work(const Message &m) {
      execute(m);
      send(reply_ready, to_caller, Kind::Done, nullptr, nullptr);
   }
   static unsigned __stdcall worker(void *opaque) {
      auto *self = static_cast<Bc250CompileDispatch *>(opaque);
      Scope scope(self);
      for (;;) {
         const Message m = receive(self->work_ready, self->to_worker);
         if (m.kind == Kind::Stop) return 0;
         check(m.kind == Kind::Work);
         self->work(m);
      }
   }

public:
   inline static thread_local Bc250CompileDispatch *active = nullptr;
   struct Scope {
      Bc250CompileDispatch *previous;
      explicit Scope(Bc250CompileDispatch *self) : previous(active) { active = self; }
      ~Scope() { active = previous; }
   };
   D3DDDI_DEVICECALLBACKS kt_original{}, kt{};
   D3D10DDI_CORELAYER_DEVICECALLBACKS um_original{}, um{};
   DXGI_DDI_BASE_CALLBACKS dxgi_original{}, dxgi{};

   template<typename T> const T &original() const {
      if constexpr (std::is_same_v<T, D3DDDI_DEVICECALLBACKS>) return kt_original;
      else if constexpr (std::is_same_v<T, D3D10DDI_CORELAYER_DEVICECALLBACKS>) return um_original;
      else return dxgi_original;
   }

   static Bc250CompileDispatch *create() {
      if (!BC250_DISPATCH_ALLOCATION_ALLOWED()) return nullptr;
      auto *self = new (std::nothrow) Bc250CompileDispatch;
      if (!self) return nullptr;
      if (BC250_DISPATCH_ALLOCATION_ALLOWED()) self->work_ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (BC250_DISPATCH_ALLOCATION_ALLOWED()) self->reply_ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (self->work_ready && self->reply_ready && BC250_DISPATCH_ALLOCATION_ALLOWED()) {
         unsigned id = 0;
         self->thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 8 * 1024 * 1024, worker, self,
                                            STACK_SIZE_PARAM_IS_A_RESERVATION, &id));
         self->worker_id = id;
      }
      if (!self->thread) { delete self; return nullptr; }
      return self;
   }
   ~Bc250CompileDispatch() {
      if (thread) {
         send(work_ready, to_worker, Kind::Stop, nullptr, nullptr);
         check(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
         CloseHandle(thread);
      }
      if (reply_ready) CloseHandle(reply_ready);
      if (work_ready) CloseHandle(work_ready);
   }
   Bc250CompileDispatch(const Bc250CompileDispatch &) = delete;
   Bc250CompileDispatch &operator=(const Bc250CompileDispatch &) = delete;

   void invoke(void (*run)(void *), void *data) {
      if (GetCurrentThreadId() == worker_id) { run(data); return; }
      const DWORD me = GetCurrentThreadId();
      const bool outer = static_cast<DWORD>(InterlockedCompareExchange(&caller_id, 0, 0)) != me;
      if (outer) { AcquireSRWLockExclusive(&lock); InterlockedExchange(&caller_id, static_cast<LONG>(me)); }
      Scope scope(this);
      send(work_ready, to_worker, Kind::Work, run, data);
      for (;;) {
         const Message m = receive(reply_ready, to_caller);
         if (m.kind == Kind::Done) break;
         check(m.kind == Kind::Callback);
         execute(m);
         send(work_ready, to_worker, Kind::Reply, nullptr, nullptr);
      }
      if (outer) { InterlockedExchange(&caller_id, 0); ReleaseSRWLockExclusive(&lock); }
   }
   void callback(void (*run)(void *), void *data) {
      if (GetCurrentThreadId() != worker_id) { run(data); return; }
      send(reply_ready, to_caller, Kind::Callback, run, data);
      for (;;) {
         const Message m = receive(work_ready, to_worker);
         if (m.kind == Kind::Reply) return;
         check(m.kind == Kind::Work);
         work(m);
      }
   }
   void bind(const D3D10DDIARG_CREATEDEVICE &info);
private:
   Bc250CompileDispatch() = default;
};

template<typename T> struct Bc250CallbackOwner;
template<typename Value, typename Owner> struct Bc250CallbackOwner<Value Owner::*> { using type = Owner; };
template<auto Member, typename Signature> struct Bc250Callback;
template<auto Member, typename R, typename... Args>
struct Bc250Callback<Member, R(APIENTRY *)(Args...)> {
   using Owner = typename Bc250CallbackOwner<decltype(Member)>::type;
   static R APIENTRY call(Args... values) {
      auto *dispatch = Bc250CompileDispatch::active;
      if (!dispatch) std::abort();
      struct Call {
         R(APIENTRY *fn)(Args...);
         std::tuple<Args...> args;
         std::conditional_t<std::is_void_v<R>, bool, R> result;
      } call = {dispatch->original<Owner>().*Member, std::make_tuple(values...), {}};
      dispatch->callback([](void *opaque) {
         auto *c = static_cast<Call *>(opaque);
         if constexpr (std::is_void_v<R>) std::apply(c->fn, c->args);
         else c->result = std::apply(c->fn, c->args);
      }, &call);
      if constexpr (!std::is_void_v<R>) return call.result;
   }
};

/* Shared by the production DDI wrappers and their allocation-failure tests. */
template<auto Fn> struct Bc250DdiCall;
template<typename R, typename... Args, R(APIENTRY *Fn)(Args...)>
struct Bc250DdiCall<Fn> {
   static R invoke(Bc250CompileDispatch *dispatch, Args... values) {
      struct Call {
         std::tuple<Args...> args;
         std::conditional_t<std::is_void_v<R>, bool, R> result;
      } call = {std::make_tuple(values...), {}};
      dispatch->invoke([](void *opaque) {
         auto *c = static_cast<Call *>(opaque);
         if constexpr (std::is_void_v<R>) std::apply(Fn, c->args);
         else c->result = std::apply(Fn, c->args);
      }, &call);
      if constexpr (!std::is_void_v<R>) return call.result;
   }
};

#include "CompileCallbacks.h"

/* These are the actual DDI entry wrappers, also instantiated by the host
 * failure-injection test with WDK signatures and deterministic DDI bodies. */
template<auto Fn, typename Traits, bool Compiler, bool Retire = false> struct Bc250DdiEntry;
template<typename R, typename A, typename... Rest, R(APIENTRY *Fn)(A, Rest...),
         typename Traits, bool Compiler, bool Retire>
struct Bc250DdiEntry<Fn, Traits, Compiler, Retire> {
   struct Arguments {
      A first;
      std::tuple<Rest...> rest;
      std::conditional_t<std::is_void_v<R>, bool, R> result;
   };
   static __declspec(noinline) void run(void *opaque) {
      auto *args = static_cast<Arguments *>(opaque);
      typename Traits::Scope scope(Traits::device(args->first));
      auto invoke = [&](Rest... rest) { return Fn(args->first, rest...); };
      if constexpr (std::is_void_v<R>) std::apply(invoke, args->rest);
      else args->result = std::apply(invoke, args->rest);
   }
   static R APIENTRY Call(A a, Rest... rest) {
      auto *device = Traits::device(a);
      auto *dispatch = device->compile_dispatch;
      typename Traits::Scope runtime_scope(device);
      Bc250CompileDispatch::Scope callback_scope(dispatch);
      if constexpr (Compiler) {
         Arguments args = {a, std::make_tuple(rest...), {}};
         dispatch->invoke(run, &args);
         if constexpr (Retire) {
            static_assert(std::is_void_v<R>, "DestroyDevice has no return value");
            device->compile_dispatch = nullptr;
            delete dispatch;
         }
         if constexpr (!std::is_void_v<R>) return args.result;
      } else {
         static_assert(!Retire, "teardown needs the reserved compiler stack");
         return Fn(a, rest...);
      }
   }
};

template<auto Fn> struct Bc250CreateDeviceEntry {
   static HRESULT APIENTRY Call(D3D10DDI_HADAPTER adapter, D3D10DDIARG_CREATEDEVICE *info) {
      auto *dispatch = Bc250CompileDispatch::create();
      if (!dispatch) return E_OUTOFMEMORY;
      dispatch->bind(*info);
      D3D10DDIARG_CREATEDEVICE local = *info;
      local.pKTCallbacks = &dispatch->kt;
      local.pUMCallbacks = &dispatch->um;
      local.DXGIBaseDDI.pDXGIBaseCallbacks = &dispatch->dxgi;
      Bc250CompileDispatch::Scope scope(dispatch);
      HRESULT hr = Bc250DdiCall<Fn>::invoke(dispatch, adapter, &local);
      if (FAILED(hr)) delete dispatch;
      return hr;
   }
};
#endif
