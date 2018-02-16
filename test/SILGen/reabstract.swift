// RUN: %target-swift-frontend -Xllvm -sil-full-demangle -emit-silgen -enable-sil-ownership %s | %FileCheck %s

func takeFn<T>(_ f : (T) -> T?) {}
func liftOptional(_ x : Int) -> Int? { return x }

func test0() {
  takeFn(liftOptional)
}
// CHECK:    sil hidden @$S10reabstract5test0yyF : $@convention(thin) () -> () {
//   Emit a generalized reference to liftOptional.
//   TODO: just emit a globalized thunk
// CHECK:      reabstract.liftOptional
// CHECK-NEXT: [[T1:%.*]] = function_ref @$S10reabstract12liftOptional{{[_0-9a-zA-Z]*}}F
// CHECK-NEXT: [[T2:%.*]] = thin_to_thick_function [[T1]]
// CHECK-NEXT: [[CVT:%.*]] = convert_escape_to_noescape [[T2]]
// CHECK-NEXT: reabstraction thunk
// CHECK-NEXT: [[T3:%.*]] = function_ref [[THUNK:@.*]] :
// CHECK-NEXT: [[T4:%.*]] = partial_apply [callee_guaranteed] [[T3]]([[CVT]])
// CHECK-NEXT: [[CVT:%.*]] = convert_escape_to_noescape [[T4]]
// CHECK:      [[T0:%.*]] = function_ref @$S10reabstract6takeFn{{[_0-9a-zA-Z]*}}F
// CHECK-NEXT: apply [[T0]]<Int>([[CVT]])
// CHECK-NEXT: destroy_value
// CHECK-NEXT: destroy_value
// CHECK-NEXT: tuple ()
// CHECK-NEXT: return

// CHECK:    sil shared [transparent] [serializable] [reabstraction_thunk] [[THUNK]] : $@convention(thin) (@in Int, @noescape @callee_guaranteed (Int) -> Optional<Int>) -> @out Optional<Int> {
// CHECK:      [[T0:%.*]] = load [trivial] %1 : $*Int
// CHECK-NEXT: [[T1:%.*]] = apply %2([[T0]])
// CHECK-NEXT: store [[T1]] to [trivial] %0
// CHECK-NEXT: tuple ()
// CHECK-NEXT: return

// CHECK-LABEL: sil hidden @$S10reabstract10testThrowsyyypF
// CHECK:         function_ref @$SytytIegir_Ieg_TR
// CHECK:         function_ref @$Sytyts5Error_pIegirzo_sAA_pIegzo_TR
func testThrows(_ x: Any) {
  _ = x as? () -> ()
  _ = x as? () throws -> ()
}

// Make sure that we preserve inout-ness when lowering types with maximum
// abstraction level -- <rdar://problem/21329377>
class C {}

struct Box<T> {
  let t: T
}

func notFun(_ c: inout C, i: Int) {}

func testInoutOpaque(_ c: C, i: Int) {
  var c = c
  let box = Box(t: notFun)
  box.t(&c, i)
}

// CHECK-LABEL: sil hidden @$S10reabstract15testInoutOpaque_1iyAA1CC_SitF
// CHECK:         function_ref @$S10reabstract6notFun_1iyAA1CCz_SitF
// CHECK:         thin_to_thick_function {{%[0-9]+}}
// CHECK:         function_ref @$S10reabstract1CCSiIegly_ACSiytIeglir_TR
// CHECK:         partial_apply
// CHECK:         store
// CHECK:         load
// CHECK:         function_ref @$S10reabstract1CCSiytIeglir_ACSiIegly_TR
// CHECK:         partial_apply
// CHECK:         apply

// CHECK-LABEL: sil shared [transparent] [serializable] [reabstraction_thunk] @$S10reabstract1CCSiIegly_ACSiytIeglir_TR : $@convention(thin) (@inout C, @in Int, @guaranteed @callee_guaranteed (@inout C, Int) -> ()) -> @out () {
// CHECK-LABEL: sil shared [transparent] [serializable] [reabstraction_thunk] @$S10reabstract1CCSiytIeglir_ACSiIegly_TR : $@convention(thin) (@inout C, Int, @guaranteed @callee_guaranteed (@inout C, @in Int) -> @out ()) -> () {

func closureTakingOptional(_ fn: (Int?) -> ()) {}
closureTakingOptional({ (_: Any) -> () in })

// CHECK-LABEL: sil shared [transparent] [serializable] [reabstraction_thunk] @$SypIgi_SiSgIegy_TR : $@convention(thin) (Optional<Int>, @noescape @callee_guaranteed (@in Any) -> ()) -> ()
// CHECK:   [[ANYADDR:%.*]] = alloc_stack $Any
// CHECK:   [[OPTADDR:%.*]] = init_existential_addr [[ANYADDR]] : $*Any, $Optional<Int>
// CHECK:   store %0 to [trivial] [[OPTADDR]] : $*Optional<Int>
// CHECK:   apply %1([[ANYADDR]]) : $@noescape @callee_guaranteed (@in Any) -> ()
