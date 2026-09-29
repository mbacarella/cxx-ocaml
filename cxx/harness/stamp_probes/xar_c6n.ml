module type S = sig type key module N : sig type 'a t val x : 'a t end end
  module SS = struct type t = string let compare = compare end module MS =
  Map.Make(SS) module HofM (M: Map.S) : S with type key = M.key = struct type
  key = M.key module N = struct type 'a t = 'a list let x = [] end end
