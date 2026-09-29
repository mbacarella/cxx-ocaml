module type S = sig type key type 'a t val create : int -> 'a t end module SS =
  struct type t = string let compare = compare end module MS = Map.Make(SS)
  module SI = struct type t = int let compare = compare end module MI =
  Map.Make(SI) module HofM (M: Map.S) : S with type key = M.key = struct type
  key = M.key type 'a t = (key, 'a) Hashtbl.t let create n = Hashtbl.create n
  end module HS1 = HofM(MS) module Test(H: S) : sig val f : int end = struct let
  f = 1 end module TS1 = Test(HS1)
