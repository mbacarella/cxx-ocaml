module type ORDERED = sig type t val leq: t -> t -> bool end module type HEAP =
  sig module Elem: ORDERED type heap val empty: heap val insert: Elem.t -> heap
  -> heap val findMin: heap -> Elem.t end module type MK = functor
  (Element:ORDERED) -> HEAP with module Elem = Element module Bootstrap (MakeH:
  MK) = struct end
