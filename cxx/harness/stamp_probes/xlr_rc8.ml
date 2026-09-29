module type ORDERED = sig type t val leq: t -> t -> bool end module type HEAP =
  sig module Elem: ORDERED type heap val empty: heap end module I = struct type
  t = int let leq = (<=) end module F (X : ORDERED) = struct module rec P : sig
  type heap end = struct type heap = int end end
