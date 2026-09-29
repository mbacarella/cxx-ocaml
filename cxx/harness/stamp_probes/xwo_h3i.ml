module type ORDERED = sig type t end module type HEAP = sig module Elem: ORDERED
  end module type MK = sig include HEAP with type Elem.t = int end
