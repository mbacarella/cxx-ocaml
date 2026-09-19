module type ORDERED = sig type t end module type HEAP = sig module Elem: ORDERED
  module Elem2: ORDERED end module type MK = functor (Element:ORDERED) -> HEAP
  with type Elem.t = Element.t
