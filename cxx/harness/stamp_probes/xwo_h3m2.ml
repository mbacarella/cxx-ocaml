module type ORDERED = sig type t end module type HEAP = sig module Elem: ORDERED
  end module type MK = HEAP with type Elem.t = int module type MK2 = HEAP with
  type Elem.t = int
