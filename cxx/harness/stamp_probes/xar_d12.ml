module type S = sig type key module N : sig val x : int end module Q : sig val q
  : int val r : int end end module SS = struct type t = string let compare =
  compare end module MS = Map.Make(SS) module HofM (M: Map.S) : S with type key
  = M.key = struct type key = M.key module N = struct let x = 1 end module Q =
  struct let q = 1 let r = 2 end end module HS1 = HofM(MS) module Test(H: S) =
  struct let f = 1 end module TS1 = Test(HS1)
