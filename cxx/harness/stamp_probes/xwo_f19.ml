module type ORDERED = sig type t val leq: t -> t -> bool end module type HEAP =
  sig module Elem: ORDERED type heap val empty: heap end module LH (Element:
  ORDERED) : HEAP with module Elem = Element = struct module Elem = Element type
  heap = int let empty = 0 end module I = struct type t = int let leq = (<=) end
  module Boot (MakeH: functor (Element:ORDERED) -> HEAP with module Elem =
  Element) (Element: ORDERED) : HEAP with module Elem = Element = struct module
  Elem = Element type heap = int let empty = 0 end module W = struct module C =
  Boot(LH)(I) end
