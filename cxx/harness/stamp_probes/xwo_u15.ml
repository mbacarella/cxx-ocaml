module type ORDERED = sig type t val leq: t -> t -> bool end module type HEAP =
  sig module Elem: ORDERED type heap val empty: heap end module LH (Element:
  ORDERED) : HEAP with module Elem = Element = struct module Elem = Element type
  heap = int let empty = 0 end module BE = struct type t = E | H of int let leq
  t1 t2 = true end module rec PrimH : sig module Elem : ORDERED with type t =
  BE.t end = struct module Elem = BE end
