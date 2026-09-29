(* control: both continuations, the second rebuilds Shallow only *)
module type S = sig
  val a : (int, int) Effect.Deep.continuation
  val b : (int, int) Effect.Shallow.continuation
end
let z = 1
