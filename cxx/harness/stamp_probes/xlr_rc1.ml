module type ORDERED = sig type t val leq: t -> t -> bool end module type HEAP =
  sig module Elem: ORDERED type heap val empty: heap end module I = struct type
  t = int let leq = (<=) end module rec P : sig module Elem : sig type t end
  type heap end = struct module Elem = struct type t = int end type heap = int
  end
