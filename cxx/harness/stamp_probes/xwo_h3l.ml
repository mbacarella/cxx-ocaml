module type ORDERED = sig type t end module type HEAP = sig module Elem: ORDERED
  end module X : HEAP with type Elem.t = int = struct module Elem = struct type
  t = int end end
