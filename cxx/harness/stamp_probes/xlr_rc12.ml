module type ORDERED = sig type t val leq: t -> t -> bool end module type HEAP =
  sig module Elem: ORDERED type heap val empty: heap end module I = struct type
  t = int let leq = (<=) end module F (X : ORDERED) = struct module rec P : sig
  module Elem : sig module Z : sig type t end end type heap end = struct module
  Elem = struct module Z = struct type t = int end end type heap = int end end
