module type ORDERED = sig type t val leq: t -> t -> bool end module type HEAP =
  sig module Elem: ORDERED type heap val empty: heap end module P : ORDERED =
  struct type t = int let leq = (<=) end module type MK = HEAP with module Elem
  = P
