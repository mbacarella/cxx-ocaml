module type ORDERED = sig type t val leq: t -> t -> bool end module type HEAP =
  sig module Elem: ORDERED type heap val empty: heap val insert: Elem.t -> heap
  -> heap val findMin: heap -> Elem.t end module Bootstrap (MakeH: functor
  (Element:ORDERED) -> HEAP with module Elem = Element) (Element: ORDERED) :
  HEAP with module Elem = Element = struct module Elem = Element module BE =
  struct type t = E | H of Elem.t let leq t1 t2 = true end module PrimH : HEAP =
  MakeH(BE) type heap = BE.t let empty = BE.E let insert x h = h let findMin =
  function BE.E -> raise Not_found | BE.H x -> x end
