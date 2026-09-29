module type ORDERED = sig type t val leq: t -> t -> bool val eq: t -> t -> bool
  val lt: t -> t -> bool end module type HEAP = sig module Elem: ORDERED type
  heap val empty: heap end module type MK = functor (Element:ORDERED) -> HEAP
  with module Elem = Element
