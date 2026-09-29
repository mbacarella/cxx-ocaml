module type ORDERED = sig type t val leq: t -> t -> bool end module type HEAP =
  sig module Elem: ORDERED type heap val empty: heap end module P = struct type
  t = int let leq = (<=) module Q = struct type u end end module type MK = HEAP
  with module Elem = P
