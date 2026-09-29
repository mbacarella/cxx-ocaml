module type ORDERED = sig type t val leq: t -> t -> bool end module type HEAP =
  sig module Elem: ORDERED type heap val empty: heap val insert: Elem.t -> heap
  -> heap val findMin: heap -> Elem.t end module LH (Element: ORDERED) : HEAP
  with module Elem = Element = struct module Elem = Element type heap = int let
  empty = 0 let insert x h = h let findMin h = raise Not_found end module I =
  struct type t = int let leq = (<=) end module LH2 (E : ORDERED) = struct
  module Elem = E type heap = int let empty = 0 let insert x h = h let findMin h
  = raise Not_found end module C = LH2(I) let h = C.findMin C.empty
