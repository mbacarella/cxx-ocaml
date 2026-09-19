module type S = sig type key module N : sig type 'a t module P : sig val y : int
  val z : int end end end module SS = struct type t = string let compare =
  compare end module MS = Map.Make(SS) module HofM (M: Map.S) : S with type key
  = M.key = struct type key = M.key module N = struct type 'a t = 'a list module
  P = struct let y = 1 let z = 2 end end end module HS1 = HofM(MS) module
  Test(H: S) = struct let f = 1 end module TS1 = Test(HS1)
