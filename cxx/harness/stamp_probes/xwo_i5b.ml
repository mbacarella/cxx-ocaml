module type ORDERED = sig type t val leq: t -> t -> bool end module type HEAP =
  sig module Elem: ORDERED type heap val empty: heap end module type MK = sig
  include sig module Elem : sig type t val leq : t -> t -> bool end type heap
  val empty : heap end end
