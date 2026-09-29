module type S = sig type key type 'a t val k : key end module SS = struct type t
  = string let compare = compare end module MS = Map.Make(SS) module SI = struct
  type t = int let compare = compare end module MI = Map.Make(SI) module HofM
  (M: Map.S) : S = struct type key = M.key type 'a t = (key, 'a) Hashtbl.t let k
  = Obj.magic 0 end module HS1 = HofM(MS) module Test(H: S) = struct let f (h :
  'a H.t) = 1 end module TS1 = Test(HS1)
