module type ORDERED = sig type t end module type HEAP = sig module Elem: ORDERED
  end module type MK = functor (Element:ORDERED) -> HEAP with type Elem.t =
  Element.t
