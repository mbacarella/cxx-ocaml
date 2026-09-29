module type S = sig type key type 'a t val create : int -> 'a t end module SS =
  struct type t = string let compare = compare end module MS = Map.Make(SS)
  module W = struct module HofM (M: Map.S) : S with type key = M.key = struct
  type key = M.key type 'a t = (key, 'a) Hashtbl.t let create n = Hashtbl.create
  n end end module HS1 = W.HofM(MS) module Test(H: S) = struct let f = 1 end
