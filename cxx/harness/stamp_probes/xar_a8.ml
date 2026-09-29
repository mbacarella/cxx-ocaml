module type S = sig type key type 'a t val create : int -> 'a t end module type
  K = sig type key val k : key end module Test(H: S) (M: K with type key =
  H.key) = struct let f (h : 'a H.t) = 1 end module SS = struct type t = string
  let compare = compare end module MS = Map.Make(SS) module HofM (M: Map.S) : S
  with type key = M.key = struct type key = M.key type 'a t = (key, 'a)
  Hashtbl.t let create n = Hashtbl.create n end module HS1 = HofM(MS) module KS
  = struct type key = string let k = "" end module TS1 = Test(HS1)(KS)
