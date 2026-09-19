module type S = sig type t val empty : t val card : t -> int end module F (X :
  sig type t end) : S with type t = X.t = struct type t = X.t let empty =
  Obj.magic 0 let card x = 0 end module I = struct type t = int end module C =
  F(I)
