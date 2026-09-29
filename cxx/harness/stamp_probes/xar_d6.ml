module type S = sig type key module N : sig type 'a t val x : 'a t val y : int
  end end module SS = struct type t = string let compare = compare end module MS
  = Map.Make(SS) module HofM (M: Map.S) : S with type key = M.key = struct type
  key = M.key module N = struct type 'a t = 'a list let x = [] let y = 0 end end
  module HS1 = HofM(MS) module Test(H: sig module N : sig val y : int end end) =
  struct let f = 1 end module TS1 = Test(HS1)
