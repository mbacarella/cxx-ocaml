module type S = sig type key type 'a t val l : key list end module SS = struct
  type t = string let compare = compare end module MS = Map.Make(SS) module HofM
  (M: Map.S) : S with type key = M.key = struct type key = M.key type 'a t =
  (key, 'a) Hashtbl.t let l = [] end module HS1 = HofM(MS) module Test(H: S) =
  struct let f = 1 end module TS1 = Test(HS1)
