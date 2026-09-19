module type ORDERED = sig type t val leq: t -> t -> bool end module type HEAP =
  sig module Elem: ORDERED type heap val empty: heap end module type Z = HEAP
  with type Elem.t = int module type MK = sig include Z end
