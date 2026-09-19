module type ORDERED = sig type t val leq: t -> t -> bool end module type HEAP =
  sig module Elem: ORDERED type heap val empty: heap end module F (X : HEAP with
  type Elem.t = int) = struct end
