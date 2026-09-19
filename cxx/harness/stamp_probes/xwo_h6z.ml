module type ORDERED = sig type t end module type HEAP = sig module Elem: sig
  module Z : ORDERED end end module type MK = functor (Element:ORDERED) -> HEAP
  with module Elem.Z = Element
