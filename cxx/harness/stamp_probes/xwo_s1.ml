module type ORDERED = sig type t val leq: t -> t -> bool end module type HEAP =
  sig module Elem: ORDERED type heap val empty: heap val insert: Elem.t -> heap
  -> heap val findMin: heap -> Elem.t end module Bootstrap (MakeH: functor
  (Element:ORDERED) -> HEAP with module Elem = Element) (Element: ORDERED) =
  struct module Elem = Element module rec BE : sig type t = E | H of Elem.t val
  leq: t -> t -> bool end = struct type t = E | H of Elem.t let leq t1 t2 = true
  end and PrimH : HEAP with type Elem.t = BE.t = MakeH(BE) end
