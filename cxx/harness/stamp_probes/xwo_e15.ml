module type ORDERED = sig type t val leq: t -> t -> bool end module type HEAP =
  sig module Elem: ORDERED type heap val empty: heap end module X : HEAP with
  type Elem.t = int = struct module Elem = struct type t = int let leq = (<=)
  end type heap = int let empty = 0 end
