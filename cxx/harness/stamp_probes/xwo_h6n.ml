module type ORDERED = sig type t end module type HEAP = sig module Elem: sig
  module Z : ORDERED end end module type MK = HEAP with type Elem.Z.t = int
