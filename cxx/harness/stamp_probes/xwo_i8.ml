module type ORDERED = sig type t val leq: t -> t -> bool end module type HEAP =
  sig module Elem: ORDERED type heap val empty: heap end module P = struct type
  t = int let leq = (<=) end module type MK = sig include HEAP with module Elem
  = P end
