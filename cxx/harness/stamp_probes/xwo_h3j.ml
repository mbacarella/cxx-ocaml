module type ORDERED = sig type t end module type HEAP = sig module Elem: ORDERED
  end module F (X : HEAP with type Elem.t = int) = struct end
