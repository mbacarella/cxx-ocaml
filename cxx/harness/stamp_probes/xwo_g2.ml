module type ORDERED = sig type t val leq: t -> t -> bool end module I = struct
  type t = int let leq = (<=) end module F (X : ORDERED) : sig module N :
  ORDERED type heap end = struct module N = X type heap = int end module C =
  F(I)
