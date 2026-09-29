module type ORDERED = sig type t val leq: t -> t -> bool end module type HEAP =
  sig module Elem : ORDERED type heap end module I = struct type t = int let leq
  = (<=) end module F (X : ORDERED) : HEAP with module Elem = X = struct module
  Elem = X type heap = int end module C = F(I)
