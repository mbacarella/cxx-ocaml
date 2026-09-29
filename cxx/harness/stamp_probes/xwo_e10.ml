module type ORDERED = sig type t val leq: t -> t -> bool end module type HEAP =
  sig module Elem: ORDERED type heap val empty: heap end module F (Element :
  ORDERED) : HEAP with module Elem = Element = struct module Elem = Element type
  heap = int let empty = 0 end
