module type S = sig type key module N : sig type 'a t val x : 'a t end end
  module SS = struct type t = string let compare = compare end module MS =
  Map.Make(SS) module W = struct module M = struct type key = int module N =
  struct type 'a t = 'a list let x = [] end end end module HS1 = W.M module
  Test(H: S) = struct let f = 1 end module TS1 = Test(HS1)
