module type ORDERED = sig type t val leq: t -> t -> bool end module type HEAP =
  sig module Elem: ORDERED type heap val empty: heap val insert: Elem.t -> heap
  -> heap val findMin: heap -> Elem.t end module Bootstrap (MakeH: functor
  (Element:ORDERED) -> sig module Elem : ORDERED with type t = Element.t end) =
  struct end
